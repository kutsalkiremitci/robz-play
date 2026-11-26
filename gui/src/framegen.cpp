// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Robz Play - frame generation stage of the stream pipeline.

#include "framegen.h"

#include <robzfg/dedup.h>
#include <robzfg/rife_interpolator.h>
#include <robzfg/timeline.h>

#include <chiaki/time.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QLoggingCategory>
#include <QPainter>
#include <QStandardPaths>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

Q_LOGGING_CATEGORY(robzFrameGen, "robzplay.framegen", QtInfoMsg);

namespace
{

// At most this many frames are generated between two real ones (30 fps -> 120 fps).
const int MAX_INTERMEDIATES = 3;
// More than this many frames waiting means the GPU cannot keep up: stop generating until it drains.
const size_t MAX_QUEUE_DEPTH = 3;
// Generating all the frames of one interval may take at most this share of the interval.
const double MAX_GENERATE_LOAD = 0.8;
// A change this large between two real frames is a cut, not motion: do not blend across it.
const double SCENE_CUT_MEAN_DIFF = 35.0;
const double SCENE_CUT_CHANGED_RATIO = 0.75;
// A picture that differs from the one before by this little is the same picture again as far
// as the eye goes: an encoder still refining a still screen, a clock ticking in a corner.
// Nothing is generated in front of it. It would show no motion, and on a still screen such
// pictures come seconds apart, when the GPU has gone idle and takes several times as long.
const double STILL_MEAN_DIFF = 0.3;
const double STILL_CHANGED_RATIO = 0.0005;
// On a still screen (a menu, a pause) the encoder goes on refining the picture, and such a
// refinement now and then passes for a new picture: a faint change, at no regular distance
// from the one before. A game standing still while it runs sends faint changes too, but one
// every other frame like any other picture. So a faint change is only left without a
// generated frame when it does not come at the distance the game's pictures come at.
// (Measured on a real session: in a minute on a menu such pictures came 4, 6 and 8 frames
// apart with changes of 0.03 to 0.06 levels; each started a frame on a GPU that had been
// idle for seconds, and those frames took 40 to 90 ms.)
const double FAINT_MEAN_DIFF = 0.15;
const double FAINT_CHANGED_RATIO = 0.003;
// A generated frame this late, with the next frame due within LATE_DROP_NEXT_US, is left
// out: it would be on the screen for an instant, or replace a frame nobody had seen yet.
const uint64_t LATE_DROP_US = 8000;
const uint64_t LATE_DROP_NEXT_US = 2000;
// The trace file (see Engine::openTrace) stops growing here.
const qint64 TRACE_MAX_BYTES = 200 * 1024 * 1024;
// A frame handed on later than this after it was due counts as late (see Health).
const uint64_t LATE_FRAME_US = 4000;
// A generated frame that took longer than this is written to the log the moment it happens,
// with what the time went on; so is a frame whose comparison and upload took this long.
const double SLOW_GENERATE_MS = 40.0;
const double SLOW_INTAKE_MS = 25.0;
// At most one such line in this time: a bad second must not flood the log.
const uint64_t SLOW_LOG_INTERVAL_US = 200000;
// The motion detail is the one chosen in the settings and stays that: the delay follows what
// generating costs at it (see OutputTimeline::readyLimitUs). When the first generated frame of
// a pair is not ready within that limit in at least FIT_OVER_SHARE of FIT_WINDOW_PAIRS pairs,
// the level is more than this GPU keeps up with and the stream says so, on screen and in the
// log, until a window fits again. Single hitches do not count: they are too few to fill a
// window.
const unsigned int FIT_WINDOW_PAIRS = 60;
const double FIT_OVER_SHARE = 0.5;
// The notice goes away again once no more than this share of a window misses the limit; in
// between it stays as it is, so that a level at the edge of what the GPU manages does not
// make it flicker.
const double FIT_CLEAR_SHARE = 0.1;
const double FIT_MARGIN_US = 1000.0;

// The estimates of what generating costs decide whether a frame is planned at all, so they have
// to stay honest in both directions.
//
// A single frame that took ten times as long says the GPU was busy for a moment, not that
// frames now take that long: one sample counts for little more than twice the estimate, and
// the estimate follows a drop quickly. An estimate that jumped on such a frame would stop
// generation, since nothing would look as if it could be ready in time.
//
// And a pair that was planned as too late still has its frame made (see probeCost): it is
// not shown, but making it tells what frames cost right now. Without that the estimate
// would stay where the last frames left it, and those were the slow ones.
//
// (Generating frames only to keep an idle GPU fast was tried and measured. A GPU with
// nothing to do does slow down - robzfg-lab idle: 5 ms a frame in a steady run, 30 ms and
// more after seconds without work - but thirty thrown-away frames a second on a still
// screen changed nothing about the first frames after it, so the stage does not do that.)
//
// A GPU that really is too slow would be probed for nothing. After PROBE_LIMIT frames in a
// row that took longer than the whole interval between two pictures, it is left alone for
// PROBE_PAUSE_US.
const int PROBE_LIMIT = 5;
const uint64_t PROBE_PAUSE_US = 30000000;

// Waits until a point in time to within a fraction of a millisecond. std::this_thread::sleep_for
// is no use for this on Windows: it wakes up to a timer tick (15.6 ms) late.
void waitUntil(std::chrono::steady_clock::time_point until)
{
#ifdef _WIN32
	static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	const double sleep_ms = std::chrono::duration<double, std::milli>(until - std::chrono::steady_clock::now()).count() - 0.7;
	if(timer && sleep_ms > 0.3)
	{
		LARGE_INTEGER due;
		due.QuadPart = -(LONGLONG)(sleep_ms * 10000.0); // relative, in 100 ns units
		if(SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
			WaitForSingleObject(timer, INFINITE);
	}
#endif
	// the rest of the way: giving the processor up, but never for long
	while(std::chrono::steady_clock::now() < until)
		std::this_thread::yield();
}

void trackDuration(double &average, double sample_ms)
{
	if(average <= 0.0)
		average = sample_ms;
	else if(sample_ms < average)
		average += (sample_ms - average) * 0.3;
	else
		average += (std::min(sample_ms, average * 2.0 + 2.0) - average) * 0.1;
}

const char *motionDetailName(int flow_divisor)
{
	return flow_divisor <= 1 ? "High" : (flow_divisor == 2 ? "Balanced" : "Fast");
}

template <typename T>
void raiseTo(std::atomic<T> &target, T value)
{
	T current = target.load();
	while(value > current && !target.compare_exchange_weak(current, value))
	{
	}
}

// 10-bit pictures in 16-bit words, the value in the upper bits: what an HDR stream gives
// after hardware decoding.
bool isWideFormat(const AVFrame *frame)
{
	return frame->format == AV_PIX_FMT_P010LE;
}

bool isSupportedFormat(const AVFrame *frame)
{
	if(!frame || frame->hw_frames_ctx)
		return false;
	if(frame->format != AV_PIX_FMT_NV12 && frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P
		&& !isWideFormat(frame))
		return false;
	return frame->width > 0 && frame->height > 0 && (frame->width % 4) == 0 && (frame->height % 2) == 0;
}

robzfg::ColorInfo colorInfoOf(const AVFrame *frame)
{
	robzfg::ColorInfo color;
	color.full_range = frame->color_range == AVCOL_RANGE_JPEG || frame->format == AV_PIX_FMT_YUVJ420P;
	const bool wide = isWideFormat(frame);
	const bool bt601 = frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M
		|| (frame->colorspace == AVCOL_SPC_UNSPECIFIED && frame->height < 720);
	// a 10-bit stream that does not say what it is, is HDR: the console sends nothing else in 10 bits
	const bool bt2020 = frame->colorspace == AVCOL_SPC_BT2020_NCL || frame->colorspace == AVCOL_SPC_BT2020_CL
		|| (frame->colorspace == AVCOL_SPC_UNSPECIFIED && wide);
	color.matrix = bt2020 ? robzfg::ColorMatrix::BT2020 : (bt601 ? robzfg::ColorMatrix::BT601 : robzfg::ColorMatrix::BT709);
	color.pq = frame->color_trc == AVCOL_TRC_SMPTE2084 || (frame->color_trc == AVCOL_TRC_UNSPECIFIED && wide);
	return color;
}

AVFrame *allocLike(const AVFrame *src)
{
	AVFrame *frame = av_frame_alloc();
	if(!frame)
		return nullptr;
	frame->format = src->format;
	frame->width = src->width;
	frame->height = src->height;
	if(av_frame_get_buffer(frame, 0) < 0 || av_frame_copy_props(frame, src) < 0)
	{
		av_frame_free(&frame);
		return nullptr;
	}
	return frame;
}

} // namespace

struct FrameGenStage::Engine
{
	robzfg::RifeInterpolator rife;
	robzfg::DuplicateDetector dedup;
	bool ok = false;

	// ROBZPLAY_FRAMEGEN_FAKE_MS=<ms>: instead of the GPU, a stand-in that takes that long for
	// the first frame of a pair at high motion detail (less at the lower levels, a fifth for
	// every further frame of the pair) and hands back the newer of the two pictures. With it
	// everything else in this stage - recognising repeats, the timeline, handing frames on -
	// runs as in a real session on a machine whose GPU is busy with something else, which
	// is what the simulator (gui/tools/framegen_sim.cpp) is for. Never set in normal use.
	double fake_ms = 0.0;
	int fake_divisor = 1;
	int fake_frames = 0;
	bool fake_estimated = false;
	float fake_field_time = 0.5f;

	bool init(const robzfg::RifeInterpolator::Config &rife_config, std::string *error)
	{
		fake_ms = qEnvironmentVariable("ROBZPLAY_FRAMEGEN_FAKE_MS").toDouble();
		if(fake_ms <= 0.0)
			return rife.init(rife_config, error);
		fake_divisor = rife_config.flow_divisor;
		return true;
	}

	std::string gpuName() const
	{
		return fake_ms > 0.0 ? std::string("a stand-in for the GPU (ROBZPLAY_FRAMEGEN_FAKE_MS)") : rife.gpuName();
	}

	bool push(const robzfg::Nv12View &view, const robzfg::ColorInfo &color, std::string *error)
	{
		if(fake_ms <= 0.0)
			return rife.push(view, color, error);
		fake_frames = std::min(2, fake_frames + 1);
		fake_estimated = false;
		return true;
	}

	bool ready() const { return fake_ms > 0.0 ? fake_frames >= 2 : rife.ready(); }

	void forgetFrames()
	{
		if(fake_ms > 0.0)
			fake_frames = 0;
		else
			rife.reset();
	}

	// `newest`: the real frame the pair ends with (what the stand-in hands back).
	// `field_time`: the point in time the motion estimate is made for. One estimate serves
	// every frame of a pair that asks for the same one, so frames to be shown ask for 0.5,
	// the middle; a frame made only to keep the GPU at work asks for another, which makes
	// the estimate anew.
	bool interpolate(float t, const robzfg::Nv12View &newest, const robzfg::Nv12Mut &out, robzfg::InterpolateTiming *timing, std::string *error,
		float field_time = 0.5f)
	{
		if(fake_ms <= 0.0)
			return rife.interpolate(t, out, timing, error, field_time);

		const auto start = std::chrono::steady_clock::now();
		double cost_ms = fake_ms * (fake_divisor <= 1 ? 1.0 : (fake_divisor == 2 ? 0.4 : 0.25));
		if(fake_estimated && field_time == fake_field_time)
			cost_ms *= 0.2;
		fake_estimated = true;
		fake_field_time = field_time;
		const size_t row_bytes = (size_t)newest.width * newest.sample_bytes;
		for(int row = 0; row < newest.height; row++)
			memcpy(out.y + (size_t)row * out.y_stride, newest.y + (size_t)row * newest.y_stride, row_bytes);
		for(int row = 0; row < newest.height / 2; row++)
			memcpy(out.uv + (size_t)row * out.uv_stride, newest.uv + (size_t)row * newest.uv_stride, row_bytes);
		waitUntil(start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(cost_ms)));
		if(timing)
		{
			timing->total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			timing->gpu_ms = timing->total_ms;
		}
		return true;
	}
	// recent averages: any generated frame, the first frame of a pair (the one that includes
	// the motion estimate), and the further frames of a pair (built from the same estimate)
	double generate_ms_avg = 0.0;
	double first_ms_avg = 0.0;
	double rest_ms_avg = 0.0;
	// how long after a real frame arrived its first generated frame was ready: everything
	// in between counts (waiting for the worker, recognising repeats, upload, synthesis)
	double ready_delay_ms_avg = 0.0;
	// the very first runs are slow (pipelines and buffers are still being set up) and say
	// nothing about the steady cost: they are not measured
	int warmup_left = 4;

	// the motion detail in use: the one chosen in the settings
	int divisor = 1;
	// the current window of pairs for telling whether it fits (see FIT_WINDOW_PAIRS)
	unsigned int fit_pairs = 0;
	unsigned int fit_over = 0;
	double fit_ready_sum_ms = 0.0;

	// context for the log line written when a frame takes unusually long
	uint64_t last_generate_end_us = 0;
	unsigned int generated_since_restart = 0;
	uint64_t last_slow_log_us = 0;
	size_t waiting = 0; // decoded frames still queued when the current one was taken
	// see PROBE_LIMIT
	int failed_probes = 0;
	uint64_t probe_pause_until_us = 0;
	// the slot of the last new picture (see FAINT_MEAN_DIFF)
	bool have_picture_slot = false;
	int64_t picture_slot = 0;
	// where a frame made only to learn what it costs goes (see probeCost)
	std::vector<uint8_t> probe_picture;
	bool probe_flip = false;

	// when each picture is shown (robzfg/timeline.h), and what it said about the frame
	// that is being worked on
	robzfg::OutputTimeline timeline;
	int64_t frame_slot = 0;
	int frame_advance = 1;
	double frame_error_us = 0.0;
	unsigned int clock_jumps_seen = 0;

	// One line per frame of the stream, for working on the timing with what a real session
	// looked like. Only written when asked for: ROBZPLAY_FRAMEGEN_TRACE_FILE=1, or a file
	// called framegen-trace.on next to the executable.
	std::unique_ptr<QFile> trace;
	uint64_t trace_origin_us = 0;
	qint64 trace_bytes = 0;

	void openTrace()
	{
		const bool wanted = qEnvironmentVariableIsSet("ROBZPLAY_FRAMEGEN_TRACE_FILE")
			|| QFile::exists(QCoreApplication::applicationDirPath() + QStringLiteral("/framegen-trace.on"));
		if(!wanted)
			return;
		const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/log");
		QDir().mkpath(dir);
		trace.reset(new QFile(dir + QStringLiteral("/framegen-trace-")
			+ QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss")) + QStringLiteral(".csv")));
		if(!trace->open(QIODevice::WriteOnly | QIODevice::Text))
		{
			trace.reset();
			return;
		}
		traceLine("t_ms,slot,advance,kind,clock_error_ms,picture_late_ms,mean_diff,changed_ratio,slots,generated,held,"
			"latency_ms,first_ms,ready_delay_ms,real_wait_ms,waiting\n");
	}

	void traceLine(const QByteArray &line)
	{
		if(!trace)
			return;
		trace_bytes += trace->write(line);
		if(trace_bytes > TRACE_MAX_BYTES)
			trace.reset();
	}

	// kind: R a repeat, U a new picture, O passed on untouched (the worker was behind),
	// X passed on untouched (cannot be worked on)
	void traceFrame(uint64_t arrival_us, char kind, const robzfg::FrameDifference *diff, const robzfg::OutputTimeline::Plan *plan,
		uint64_t planned_us)
	{
		if(!trace)
			return;
		if(!trace_origin_us)
			trace_origin_us = arrival_us;
		QString line = QString::asprintf("%.3f,%lld,%d,%c,%.3f,%.3f,", (double)(arrival_us - trace_origin_us) / 1000.0,
			(long long)frame_slot, frame_advance, kind, frame_error_us / 1000.0, timeline.pictureLateness() / 1000.0);
		if(diff)
			line += QString::asprintf("%.3f,%.5f,", diff->mean_abs_diff, diff->changed_ratio);
		else
			line += QStringLiteral(",,");
		if(plan)
			line += QString::asprintf("%d,%d,%d,%.3f,%.3f,%.3f,%.3f,", plan->immediate ? 0 : plan->spacing, (int)plan->generated.size(),
				plan->held, plan->latency_us / 1000.0, first_ms_avg, ready_delay_ms_avg,
				plan->real_due_us > planned_us ? (double)(plan->real_due_us - planned_us) / 1000.0 : 0.0);
		else
			line += QStringLiteral(",,,,,,,");
		line += QString::asprintf("%u\n", (unsigned int)waiting);
		traceLine(line.toLatin1());
	}

	// What generating `count` frames for one pair is expected to cost, in ms.
	double pairCostMs(int count) const
	{
		if(count <= 0 || first_ms_avg <= 0.0)
			return 0.0;
		// until further frames have been measured, assume they are cheap and find out
		const double rest = rest_ms_avg > 0.0 ? rest_ms_avg : first_ms_avg * 0.25;
		return first_ms_avg + (count - 1) * rest;
	}
};

// ---------------------------------------------------------------------------- Waiter

void FrameGenStage::Waiter::open()
{
#ifdef _WIN32
	event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if(!timer) // before Windows 10 1803
		timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
#endif
}

void FrameGenStage::Waiter::close()
{
#ifdef _WIN32
	if(event)
		CloseHandle((HANDLE)event);
	if(timer)
		CloseHandle((HANDLE)timer);
	event = nullptr;
	timer = nullptr;
#endif
}

void FrameGenStage::Waiter::signal()
{
#ifdef _WIN32
	if(event)
		SetEvent((HANDLE)event);
#else
	condition.notify_all();
#endif
}

void FrameGenStage::Waiter::wait(std::unique_lock<std::mutex> &lock, uint64_t timeout_us)
{
#ifdef _WIN32
	lock.unlock();
	if(timeout_us < 300 || !event || !timer)
	{
		// too short to sleep through reliably
		SwitchToThread();
	}
	else
	{
		LARGE_INTEGER due;
		due.QuadPart = -(LONGLONG)(timeout_us * 10); // relative, in 100 ns units
		SetWaitableTimer((HANDLE)timer, &due, 0, nullptr, nullptr, FALSE);
		HANDLE handles[2] = {(HANDLE)event, (HANDLE)timer};
		if(WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0)
			CancelWaitableTimer((HANDLE)timer);
	}
	lock.lock();
#else
	condition.wait_for(lock, std::chrono::microseconds(timeout_us));
#endif
}

// ---------------------------------------------------------------------------- lifecycle

FrameGenStage::FrameGenStage(QObject *parent) : QObject(parent)
{
	poll_timer.setInterval(250);
	connect(&poll_timer, &QTimer::timeout, this, [this]() {
		if(status_dirty.exchange(false))
			emit stateChanged();
		emit statsChanged();
	});
}

FrameGenStage::~FrameGenStage()
{
	stop();
}

QString FrameGenStage::status() const
{
	std::lock_guard<std::mutex> lock(status_mutex);
	return QString::fromStdString(status_text);
}

void FrameGenStage::setStatus(const std::string &text)
{
	{
		std::lock_guard<std::mutex> lock(status_mutex);
		if(status_text == text)
			return;
		status_text = text;
	}
	status_dirty.store(true);
}

void FrameGenStage::start(const Config &new_config, PresentFn present_fn)
{
	stop();
	config = new_config;
	present = std::move(present_fn);
	stop_requested = false;
	presenter_stop = false;
	engine_ready.store(false);
	stats_unique.store(0);
	stats_presented.store(0);
	stat_source_fps.store(0.0);
	stat_output_fps.store(0.0);
	stat_generate_ms.store(0.0);
	takeHealth(); // nothing of an earlier stream is left in the counters
	setStatus(config.enabled ? "loading" : "off");
#ifdef _WIN32
	timeBeginPeriod(1);
#endif
	work_waiter.open();
	present_waiter.open();
	running.store(true);
	presenter = std::thread([this]() { presenterMain(); });
	worker = std::thread([this]() { workerMain(); });
	poll_timer.start();
	emit stateChanged();
}

void FrameGenStage::stop()
{
	if(!running.load())
		return;
	// the worker first: it is the one that still feeds the presenter
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		stop_requested = true;
	}
	work_waiter.signal();
	if(worker.joinable())
		worker.join();
	{
		std::lock_guard<std::mutex> lock(schedule_mutex);
		presenter_stop = true;
	}
	present_waiter.signal();
	if(presenter.joinable())
		presenter.join();
	running.store(false);
	poll_timer.stop();
	work_waiter.close();
	present_waiter.close();
#ifdef _WIN32
	timeEndPeriod(1);
#endif
	setStatus("off");
	status_dirty.store(false);
	emit stateChanged();
}

void FrameGenStage::runSelfTest(const Config &test_config, std::function<void()> on_presented)
{
	auto presented = std::make_shared<std::atomic<int>>(0);
	// what is fed below is a new picture every 1/30 s and nothing in between: a 30 fps stream
	Config self_test_config = test_config;
	self_test_config.stream_fps = 30;
	start(self_test_config, [presented, on_presented](ChiakiFfmpegFrame frame, int32_t, qint64) {
		presented->fetch_add(1);
		if(on_presented)
			on_presented();
		av_frame_free(&frame.frame);
	});

	// a bright bar sweeping across a 1080p picture, one new frame every 1/30 s for 8 s,
	// fed from its own thread so the cadence does not depend on how busy the UI is
	auto index = std::make_shared<std::atomic<int>>(0);
	auto feeder = std::make_shared<std::thread>([this, index]() {
		const auto begin = std::chrono::steady_clock::now();
		for(int i = 0; i < 240; i++)
		{
			const auto target = begin + std::chrono::microseconds((int64_t)i * 1000000 / 30);
			while(std::chrono::steady_clock::now() < target)
				std::this_thread::sleep_for(std::chrono::microseconds(500));
			AVFrame *frame = av_frame_alloc();
			frame->format = AV_PIX_FMT_NV12;
			frame->width = 1920;
			frame->height = 1080;
			if(av_frame_get_buffer(frame, 0) < 0)
			{
				av_frame_free(&frame);
				continue;
			}
			const int bar = (i * 24) % (frame->width - 200);
			for(int y = 0; y < frame->height; y++)
			{
				uint8_t *row = frame->data[0] + (size_t)y * frame->linesize[0];
				memset(row, 40 + (y * 100) / frame->height, (size_t)frame->width);
				memset(row + bar, 220, 200);
			}
			for(int y = 0; y < frame->height / 2; y++)
				memset(frame->data[1] + (size_t)y * frame->linesize[1], 128, (size_t)frame->width);
			frame->colorspace = AVCOL_SPC_BT709;
			frame->color_range = AVCOL_RANGE_MPEG;
			ChiakiFfmpegFrame chiaki_frame = {};
			chiaki_frame.frame = frame;
			chiaki_frame.pts = i / 30.0;
			chiaki_frame.duration = 1.0 / 30.0;
			index->fetch_add(1);
			submit(chiaki_frame, 0, static_cast<qint64>(chiaki_time_now_monotonic_us()));
		}
	});

	QTimer::singleShot(7600, this, [this, feeder, index, presented]() {
		// sample the counters while frames are still flowing, then let the feeder finish
		const QString final_status = status();
		const double fps_in = sourceFps();
		const double fps_out = outputFps();
		const double generate_ms = generateMs();
		if(feeder->joinable())
			feeder->join();
		fprintf(stderr, "Robz Play frame generation self-test: status \"%s\", %d frames in, %d frames out, "
			"%.1f fps in, %.1f fps out, %.2f ms per generated frame\n",
			final_status.toUtf8().constData(), index->load(), presented->load(), fps_in, fps_out, generate_ms);
		fflush(stderr);
		stop();
	});
}

void FrameGenStage::submit(ChiakiFfmpegFrame frame, int32_t frames_lost, qint64 delivery_us, int32_t frames_skipped)
{
	if(!frame.frame)
		return;
	if(!running.load())
	{
		av_frame_free(&frame.frame);
		return;
	}
	if(!engine_ready.load())
	{
		// still loading the model, or it could not be loaded: show the frame as it is
		// instead of letting frames pile up behind the load
		stats_unique.fetch_add(1);
		stats_presented.fetch_add(1);
		health_real.fetch_add(1);
		present(frame, frames_lost, delivery_us);
		return;
	}
	Item item;
	item.frame = frame;
	item.frames_lost = frames_lost;
	item.delivery_us = delivery_us;
	item.skipped = frames_skipped;
	item.arrival_us = chiaki_time_now_monotonic_us();
	size_t waiting = 0;
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		queue.push_back(item);
		waiting = queue.size();
	}
	raiseTo(health_queue_max, (unsigned int)waiting);
	work_waiter.signal();
}

// ---------------------------------------------------------------------------- presenter

void FrameGenStage::presenterMain()
{
	std::unique_lock<std::mutex> lock(schedule_mutex);
	while(!presenter_stop)
	{
		if(scheduled.empty())
		{
			present_waiter.wait(lock, 250000);
			continue;
		}
		const uint64_t now = chiaki_time_now_monotonic_us();
		const uint64_t due = scheduled.front().due_us;
		if(due > now)
		{
			present_waiter.wait(lock, due - now);
			continue;
		}
		Item item = scheduled.front().item;
		scheduled.pop_front();
		const uint64_t late_us = now - due;
		if(item.generated && late_us > LATE_DROP_US && !scheduled.empty() && scheduled.front().due_us <= now + LATE_DROP_NEXT_US)
		{
			// too late to be seen (see LATE_DROP_US); what it knew about lost frames goes
			// on with the frame behind it
			scheduled.front().item.frames_lost += item.frames_lost;
			lock.unlock();
			health_dropped.fetch_add(1);
			av_frame_free(&item.frame.frame);
			lock.lock();
			continue;
		}
		lock.unlock();
		stats_presented.fetch_add(1);
		if(late_us > LATE_FRAME_US)
			health_late.fetch_add(1);
		raiseTo(health_late_max_us, late_us);
		(item.generated ? health_generated : health_real).fetch_add(1);
		present(item.frame, item.frames_lost, static_cast<qint64>(now));
		lock.lock();
	}
	// shutting down: nothing waiting is shown any more
	for(Scheduled &entry : scheduled)
		av_frame_free(&entry.item.frame.frame);
	scheduled.clear();
}

// Puts a frame on the output timeline. Due times never run backwards, so frames always
// leave in the order they were scheduled.
void FrameGenStage::schedule(Item &item, uint64_t due_us)
{
	if(!item.frame.frame)
		return;
	item.frames_lost += carried_frames_lost;
	carried_frames_lost = 0;
	if(due_us < last_due_us)
		due_us = last_due_us;
	last_due_us = due_us;
	Scheduled entry;
	entry.item = item;
	entry.due_us = due_us;
	item.frame.frame = nullptr;
	{
		std::lock_guard<std::mutex> lock(schedule_mutex);
		scheduled.push_back(entry);
	}
	present_waiter.signal();
}

void FrameGenStage::scheduleNow(Item &item)
{
	schedule(item, chiaki_time_now_monotonic_us());
}

// Everything still waiting goes out right away, in order, and the timeline starts over:
// used whenever the stage stops generating (pass-through, a hitch, an error).
void FrameGenStage::flushSchedule()
{
	const uint64_t now = chiaki_time_now_monotonic_us();
	{
		std::lock_guard<std::mutex> lock(schedule_mutex);
		for(Scheduled &entry : scheduled)
			entry.due_us = std::min(entry.due_us, now);
	}
	last_due_us = std::min(last_due_us, now);
	present_waiter.signal();
}

// ---------------------------------------------------------------------------- worker

void FrameGenStage::workerMain()
{
	engine.reset(new Engine);
	last_due_us = 0;
	carried_frames_lost = 0;
	engine->timeline.reset(1000000.0 / (config.stream_fps > 0 ? (double)config.stream_fps : 60.0),
		config.target_fps >= 100 ? 120u : 60u, MAX_INTERMEDIATES);
	engine->openTrace();
	stats_window_start_us = chiaki_time_now_monotonic_us();

	if(config.enabled)
	{
		robzfg::RifeInterpolator::Config rife_config;
		// UTF-8: the model sits beside the executable, in a folder any user name can be part of
		rife_config.model_dir = config.model_dir.toUtf8().toStdString();
		rife_config.flow_divisor = config.flow_divisor;
		rife_config.protect_static = config.protect_static;
		// ROBZPLAY_FRAMEGEN_GPU=<index> runs frame generation on that GPU instead of the first
		// discrete one (robzfg-lab gpus lists them): for trying things out on a second GPU
		// while the first one is busy.
		if(qEnvironmentVariableIsSet("ROBZPLAY_FRAMEGEN_GPU"))
			rife_config.gpu_index = qEnvironmentVariableIntValue("ROBZPLAY_FRAMEGEN_GPU");
		std::string error;
		const auto begin = std::chrono::steady_clock::now();
		engine->ok = engine->init(rife_config, &error);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
		if(engine->ok)
		{
			qCInfo(robzFrameGen).nospace() << "Frame generation ready on " << QString::fromStdString(engine->gpuName())
				<< " in " << qRound(ms) << " ms (model " << config.model_dir << ", flow divisor " << config.flow_divisor
				<< ", target " << config.target_fps << " fps)";
			engine->divisor = config.flow_divisor;
			health_divisor.store(config.flow_divisor);
			health_too_heavy.store(false);
			setStatus("waiting for video");
			engine_ready.store(true);
		}
		else
		{
			qCWarning(robzFrameGen) << "Frame generation unavailable:" << QString::fromStdString(error);
			setStatus("unavailable: " + error);
		}
	}

	std::unique_lock<std::mutex> lock(queue_mutex);
	while(!stop_requested)
	{
		if(queue.empty())
		{
			work_waiter.wait(lock, 250000);
			lock.unlock();
			publishStats();
			lock.lock();
			continue;
		}
		Item item = queue.front();
		queue.pop_front();
		const bool overloaded = queue.size() >= MAX_QUEUE_DEPTH;
		engine->waiting = queue.size();
		lock.unlock();

		// Every frame of the stream is counted, whatever happens to it: the count is the
		// stream's own clock (robzfg/timeline.h).
		engine->frame_advance = 1 + std::max(0, item.frames_lost) + std::max(0, item.skipped);
		engine->frame_slot = engine->timeline.observe(item.arrival_us, engine->frame_advance);
		engine->frame_error_us = engine->timeline.streamClock().lastError();
		const unsigned int clock_jumps = engine->timeline.streamClock().jumps();
		if(clock_jumps != engine->clock_jumps_seen)
		{
			health_clock_jumps.fetch_add(clock_jumps - engine->clock_jumps_seen);
			engine->clock_jumps_seen = clock_jumps;
		}

		if(overloaded)
		{
			// falling behind: show frames as they are until the queue is short again
			health_overloads.fetch_add(1);
			flushSchedule();
			if(engine->ok)
			{
				engine->forgetFrames();
				engine->dedup.reset();
			}
			engine->generated_since_restart = 0;
			engine->timeline.forgetPrevious();
			engine->traceFrame(item.arrival_us, 'O', nullptr, nullptr, 0);
			stats_unique.fetch_add(1);
			scheduleNow(item);
		}
		else
			process(item);
		publishStats();
		lock.lock();
	}

	engine_ready.store(false);
	// shutting down: nothing queued is shown any more
	std::deque<Item> leftover;
	leftover.swap(queue);
	lock.unlock();
	for(Item &item : leftover)
		av_frame_free(&item.frame.frame);
	av_frame_free(&compare_prev);
	engine.reset();
}

FrameGenStage::Health FrameGenStage::takeHealth()
{
	Health health;
	health.real_frames = health_real.exchange(0);
	health.generated_frames = health_generated.exchange(0);
	health.repeats = health_repeats.exchange(0);
	health.overloads = health_overloads.exchange(0);
	health.queue_max = health_queue_max.exchange(0);
	health.late_frames = health_late.exchange(0);
	health.late_max_ms = (double)health_late_max_us.exchange(0) / 1000.0;
	const unsigned int generate_count = health_generate_count.exchange(0);
	const uint64_t generate_sum_us = health_generate_sum_us.exchange(0);
	health.generate_avg_ms = generate_count ? (double)generate_sum_us / generate_count / 1000.0 : 0.0;
	health.generate_max_ms = (double)health_generate_max_us.exchange(0) / 1000.0;
	health.motion_divisor = health_divisor.load();
	health.held = health_held.exchange(0);
	health.dropped_late = health_dropped.exchange(0);
	health.clock_jumps = health_clock_jumps.exchange(0);
	health.latency_ms = (double)health_latency_us.load() / 1000.0;
	health.picture_late_ms = (double)health_picture_late_us.load() / 1000.0;
	health.source_slots = health_source_slots.load();
	health.probe_frames = health_probes.exchange(0);
	return health;
}

void FrameGenStage::publishStats()
{
	const uint64_t now = chiaki_time_now_monotonic_us();
	const uint64_t elapsed = now - stats_window_start_us;
	if(elapsed < 500000)
		return;
	const double seconds = elapsed / 1000000.0;
	stat_source_fps.store(stats_unique.exchange(0) / seconds);
	stat_output_fps.store(stats_presented.exchange(0) / seconds);
	stat_generate_ms.store(engine ? engine->generate_ms_avg : 0.0);
	stats_window_start_us = now;
}

void FrameGenStage::process(Item &item)
{
	AVFrame *frame = item.frame.frame;

	if(!config.enabled || !engine->ok || !isSupportedFormat(frame))
	{
		if(config.enabled && engine->ok)
		{
			const char *format_name = frame->hw_frames_ctx ? nullptr : av_get_pix_fmt_name((AVPixelFormat)frame->format);
			const std::string status = frame->hw_frames_ctx ? std::string("passing through (frame stayed on the GPU)")
				: std::string("passing through (pixel format not supported: ") + (format_name ? format_name : "unknown") + ")";
			if(this->status().toStdString() != status)
				qCWarning(robzFrameGen) << "Frame generation:" << QString::fromStdString(status);
			setStatus(status);
		}
		flushSchedule();
		engine->timeline.forgetPrevious();
		engine->traceFrame(item.arrival_us, 'X', nullptr, nullptr, 0);
		stats_unique.fetch_add(1);
		scheduleNow(item);
		return;
	}

	const int w = frame->width;
	const int h = frame->height;
	const bool wide = isWideFormat(frame);
	const bool planar = frame->format != AV_PIX_FMT_NV12 && !wide;

	robzfg::Nv12View view;
	view.width = w;
	view.height = h;
	view.sample_bytes = wide ? 2 : 1;
	view.y = frame->data[0];
	view.y_stride = frame->linesize[0];
	if(planar)
	{
		// software decoders deliver three planes; the engine wants Cb and Cr interleaved
		scratch_uv.resize((size_t)w * h / 2);
		for(int row = 0; row < h / 2; row++)
		{
			const uint8_t *u = frame->data[1] + (size_t)row * frame->linesize[1];
			const uint8_t *v = frame->data[2] + (size_t)row * frame->linesize[2];
			uint8_t *dst = scratch_uv.data() + (size_t)row * w;
			for(int x = 0; x < w / 2; x++)
			{
				dst[2 * x] = u[x];
				dst[2 * x + 1] = v[x];
			}
		}
		view.uv = scratch_uv.data();
		view.uv_stride = w;
	}
	else
	{
		view.uv = frame->data[1];
		view.uv_stride = frame->linesize[1];
	}

	const uint64_t compare_begin_us = chiaki_time_now_monotonic_us();
	const robzfg::FrameDifference diff = engine->dedup.measure(view);
	const double compare_ms = (double)(chiaki_time_now_monotonic_us() - compare_begin_us) / 1000.0;
	if(engine->dedup.classify(diff))
	{
		// a repeat of the frame we already have: the console had nothing new to send
		health_repeats.fetch_add(1);
		engine->traceFrame(item.arrival_us, 'R', &diff, nullptr, 0);
		carried_frames_lost += item.frames_lost;
		av_frame_free(&item.frame.frame);
		return;
	}
	engine->dedup.accept(view);
	stats_unique.fetch_add(1);

	const bool scene_cut = diff.comparable && diff.mean_abs_diff > SCENE_CUT_MEAN_DIFF && diff.changed_ratio > SCENE_CUT_CHANGED_RATIO;
	const int64_t picture_spacing = engine->have_picture_slot ? engine->frame_slot - engine->picture_slot : 0;
	engine->picture_slot = engine->frame_slot;
	engine->have_picture_slot = true;
	const bool faint = diff.comparable && diff.mean_abs_diff < FAINT_MEAN_DIFF && diff.changed_ratio < FAINT_CHANGED_RATIO;
	const bool off_beat = picture_spacing != engine->timeline.nominalSlots();
	const bool nearly_still = diff.comparable
		&& ((diff.mean_abs_diff < STILL_MEAN_DIFF && diff.changed_ratio < STILL_CHANGED_RATIO) || (faint && off_beat));

	std::string error;
	const uint64_t push_begin_us = chiaki_time_now_monotonic_us();
	if(!engine->push(view, colorInfoOf(frame), &error))
	{
		qCWarning(robzFrameGen) << "Frame generation push failed:" << QString::fromStdString(error);
		flushSchedule();
		engine->timeline.forgetPrevious();
		scheduleNow(item);
		return;
	}
	{
		const uint64_t pushed_us = chiaki_time_now_monotonic_us();
		const double push_ms = (double)(pushed_us - push_begin_us) / 1000.0;
		if(compare_ms + push_ms >= SLOW_INTAKE_MS && pushed_us - engine->last_slow_log_us >= SLOW_LOG_INTERVAL_US)
		{
			engine->last_slow_log_us = pushed_us;
			qCInfo(robzFrameGen).noquote() << QString::asprintf(
				"[health] slow frame intake: comparing with the previous frame took %.1f ms, handing the frame to the GPU %.1f ms, %u waiting",
				compare_ms, push_ms, (unsigned int)engine->waiting);
		}
	}

	// When this picture is shown and what goes in front of it. The stream is a row of slots
	// (one per frame the console sends); this picture belongs to its slot, the generated ones
	// to the slots that held repeats. See robzfg/timeline.h.
	robzfg::OutputTimeline::Costs costs;
	costs.first_us = engine->first_ms_avg * 1000.0;
	costs.rest_us = engine->rest_ms_avg * 1000.0;
	costs.ready_delay_us = engine->ready_delay_ms_avg * 1000.0;
	const bool may_generate = engine->ready() && !scene_cut && !nearly_still;
	const uint64_t planned_us = chiaki_time_now_monotonic_us();
	robzfg::OutputTimeline::Plan plan = engine->timeline.plan(planned_us, costs, may_generate);
	const double cadence_us = engine->timeline.cadenceUs();
	const double ready_limit_us = engine->timeline.readyLimitUs();
	if(plan.held > 0)
		health_held.fetch_add((unsigned int)plan.held);
	health_latency_us.store((uint64_t)std::max(0.0, plan.latency_us));
	health_picture_late_us.store((uint64_t)std::max(0.0, engine->timeline.pictureLateness()));
	health_source_slots.store(engine->timeline.nominalSlots());

	// Never take on more than the GPU can finish inside one interval: of several frames
	// only the one nearest the middle is made then.
	bool reduced = false;
	if(plan.generated.size() > 1 && engine->pairCostMs((int)plan.generated.size()) * 1000.0 > cadence_us * MAX_GENERATE_LOAD)
	{
		size_t middle = 0;
		for(size_t i = 1; i < plan.generated.size(); i++)
			if(std::abs(plan.generated[i].t - 0.5f) < std::abs(plan.generated[middle].t - 0.5f))
				middle = i;
		const robzfg::OutputTimeline::Generated kept = plan.generated[middle];
		plan.generated.assign(1, kept);
		reduced = true;
	}
	engine->traceFrame(item.arrival_us, 'U', &diff, &plan, planned_us);

	// ROBZPLAY_FRAMEGEN_TRACE=1 prints one line per real frame: what arrived and what was decided
	static const bool trace = qEnvironmentVariableIsSet("ROBZPLAY_FRAMEGEN_TRACE");
	if(trace)
		fprintf(stderr, "[framegen] slot %lld (+%d) late %.1f ms slots %d typical %d generating %d held %d delay %.1f ms first %.1f rest %.1f ms diff %.2f cut %d still %d ready %d\n",
			(long long)plan.slot, engine->frame_advance, engine->frame_error_us / 1000.0, plan.spacing, engine->timeline.nominalSlots(),
			(int)plan.generated.size(), plan.held, plan.latency_us / 1000.0, engine->first_ms_avg, engine->rest_ms_avg,
			diff.mean_abs_diff, scene_cut ? 1 : 0, nearly_still ? 1 : 0, engine->ready() ? 1 : 0);

	if(plan.immediate)
	{
		// the source already runs at the target rate: nothing to generate, nothing held back
		setStatus(config.target_fps >= 100 ? "passing through (source already near 120 fps)" : "passing through (source already near 60 fps)");
		flushSchedule();
		av_frame_free(&compare_prev);
		scheduleNow(item);
		return;
	}
	if(plan.generated.empty())
	{
		// Nothing goes in front of this picture: a cut, a still screen, a hitch, or it came
		// too late for anything to be ready. It still takes its own slot, so what follows
		// stays where it belongs.
		av_frame_free(&compare_prev);
		schedule(item, plan.real_due_us);
		// Wanted, but could not have been ready. When that is because generating takes too
		// long, it is as bad as a frame that came out late and counts against the motion
		// detail in use; when the picture itself arrived late, it says nothing about it.
		const double would_take_us = costs.first_us + (double)(planned_us - item.arrival_us);
		if(plan.held > 0 && would_take_us > ready_limit_us - FIT_MARGIN_US)
		{
			engine->fit_pairs++;
			engine->fit_over++;
			engine->fit_ready_sum_ms += would_take_us / 1000.0;
			reviewFit(ready_limit_us);
		}
		// When a frame was left out it is made all the same, unseen: what it costs right now
		// is what decides about the next one.
		if(plan.held > 0)
			probeCost(view, cadence_us);
		return;
	}

	// Comparison view: the left half keeps the previous real frame until this one is due,
	// as the stream would show it without frame generation.
	const int count = (int)plan.generated.size();
	// (the comparison view draws into 8-bit pictures only)
	const bool compare = compare_split.load() && !wide;
	AVFrame *compare_cur = compare ? av_frame_clone(frame) : nullptr;
	if(!compare)
		av_frame_free(&compare_prev);
	const double pair_us = engine->timeline.streamClock().period() * std::max(1, plan.spacing);
	const int compare_left_fps = (int)std::lround(1000000.0 / pair_us);
	const int compare_right_fps = (int)std::lround(1000000.0 * (count + 1) / pair_us);

	// what the pair spans in the stream's own time: the frames in between share it
	const double real_duration = item.frame.duration;
	const double pair_duration = real_duration * std::max(1, plan.spacing);
	const double real_pts = item.frame.pts;
	int32_t lost_for_first = item.frames_lost;
	item.frames_lost = 0;

	for(int k = 1; k <= count; k++)
	{
		const float t = plan.generated[(size_t)k - 1].t;
		AVFrame *mid = allocLike(frame);
		if(!mid)
			break;

		robzfg::Nv12Mut out;
		out.width = w;
		out.height = h;
		out.sample_bytes = view.sample_bytes;
		out.y = mid->data[0];
		out.y_stride = mid->linesize[0];
		if(planar)
		{
			scratch_out_uv.resize((size_t)w * h / 2);
			out.uv = scratch_out_uv.data();
			out.uv_stride = w;
		}
		else
		{
			out.uv = mid->data[1];
			out.uv_stride = mid->linesize[1];
		}

		robzfg::InterpolateTiming timing;
		if(!engine->interpolate(t, view, out, &timing, &error))
		{
			qCWarning(robzFrameGen) << "Frame generation failed:" << QString::fromStdString(error);
			av_frame_free(&mid);
			break;
		}
		if(planar)
		{
			for(int row = 0; row < h / 2; row++)
			{
				const uint8_t *src = scratch_out_uv.data() + (size_t)row * w;
				uint8_t *u = mid->data[1] + (size_t)row * mid->linesize[1];
				uint8_t *v = mid->data[2] + (size_t)row * mid->linesize[2];
				for(int x = 0; x < w / 2; x++)
				{
					u[x] = src[2 * x];
					v[x] = src[2 * x + 1];
				}
			}
		}
		if(engine->warmup_left > 0)
			engine->warmup_left--;
		else
		{
			trackDuration(engine->generate_ms_avg, timing.total_ms);
			trackDuration(k == 1 ? engine->first_ms_avg : engine->rest_ms_avg, timing.total_ms);
			const uint64_t generate_us = (uint64_t)(std::max(0.0, timing.total_ms) * 1000.0);
			health_generate_count.fetch_add(1);
			health_generate_sum_us.fetch_add(generate_us);
			raiseTo(health_generate_max_us, generate_us);
			const uint64_t generated_us = chiaki_time_now_monotonic_us();
			if(timing.total_ms >= SLOW_GENERATE_MS && generated_us - engine->last_slow_log_us >= SLOW_LOG_INTERVAL_US)
			{
				// what the time went on, and what the stage had been doing before: enough to
				// tell a busy GPU from a cold start from a problem in this stage
				engine->last_slow_log_us = generated_us;
				const double idle_ms = engine->last_generate_end_us
					? (double)(generated_us - engine->last_generate_end_us) / 1000.0 - timing.total_ms : -1.0;
				qCInfo(robzFrameGen).noquote() << QString::asprintf(
					"[health] slow frame generation: %.1f ms (preparing %.1f, GPU %.1f, copying %.1f), frame %d of %d, detail %s,"
					" %.0f ms after the previous one, %u generated since the last restart, %u waiting, cadence %.1f ms, picture change %.1f",
					timing.total_ms, timing.prepare_ms, timing.gpu_ms, timing.copy_ms, k, count, motionDetailName(engine->divisor),
					idle_ms, engine->generated_since_restart, (unsigned int)engine->waiting, cadence_us / 1000.0, diff.mean_abs_diff);
			}
			if(k == 1)
			{
				const double delay_ms = (double)(generated_us - item.arrival_us) / 1000.0;
				trackDuration(engine->ready_delay_ms_avg, delay_ms);
				noteGenerateCost(timing.total_ms, cadence_us, generated_us);
				engine->fit_pairs++;
				engine->fit_ready_sum_ms += delay_ms;
				if(delay_ms * 1000.0 > ready_limit_us - FIT_MARGIN_US)
					engine->fit_over++;
			}
		}
		engine->last_generate_end_us = chiaki_time_now_monotonic_us();
		engine->generated_since_restart++;

		if(compare)
		{
			if(compare_prev)
				copyLeftHalf(mid, compare_prev);
			drawCompare(mid, compare_left_fps, compare_right_fps);
		}

		Item generated;
		generated.generated = true;
		generated.frame.frame = mid;
		generated.frame.duration = pair_duration / (count + 1);
		generated.frame.pts = real_pts - pair_duration * (1.0 - t);
		generated.frame.recovered = false;
		generated.frames_lost = lost_for_first;
		generated.delivery_us = item.delivery_us;
		lost_for_first = 0;
		schedule(generated, plan.generated[(size_t)k - 1].due_us);
	}

	item.frames_lost = lost_for_first;
	item.frame.duration = pair_duration / (count + 1);
	if(compare && av_frame_make_writable(item.frame.frame) >= 0)
		drawCompare(item.frame.frame, compare_left_fps, compare_right_fps);
	av_frame_free(&compare_prev);
	compare_prev = compare_cur;
	schedule(item, plan.real_due_us);
	setStatus(reduced ? "generating (GPU too slow for the full target, generating fewer frames)" : "generating");
	reviewFit(ready_limit_us);
}

// See PROBE_LIMIT.
void FrameGenStage::noteGenerateCost(double total_ms, double cadence_us, uint64_t now_us)
{
	if(total_ms * 1000.0 <= cadence_us)
	{
		engine->failed_probes = 0;
		return;
	}
	if(++engine->failed_probes >= PROBE_LIMIT)
	{
		engine->failed_probes = 0;
		engine->probe_pause_until_us = now_us + PROBE_PAUSE_US;
	}
}

// Generates a frame nobody will see, to learn what a frame costs right now (see PROBE_LIMIT).
// `view`: the frame being worked on, which gives the size.
void FrameGenStage::probeCost(const robzfg::Nv12View &view, double cadence_us)
{
	if(!engine->ok || !engine->ready() || engine->waiting > 0)
		return;
	if(chiaki_time_now_monotonic_us() < engine->probe_pause_until_us)
		return;

	const size_t row_bytes = (size_t)view.width * view.sample_bytes;
	engine->probe_picture.resize(row_bytes * view.height * 3 / 2);
	robzfg::Nv12Mut out;
	out.width = view.width;
	out.height = view.height;
	out.sample_bytes = view.sample_bytes;
	out.y = engine->probe_picture.data();
	out.y_stride = (int)row_bytes;
	out.uv = engine->probe_picture.data() + row_bytes * view.height;
	out.uv_stride = (int)row_bytes;
	robzfg::InterpolateTiming timing;
	std::string error;
	// a motion estimate of its own: that is the part whose cost matters
	engine->probe_flip = !engine->probe_flip;
	if(!engine->interpolate(0.5f, view, out, &timing, &error, engine->probe_flip ? 0.47f : 0.53f))
		return;
	const uint64_t end_us = chiaki_time_now_monotonic_us();
	engine->last_generate_end_us = end_us;
	health_probes.fetch_add(1);
	if(engine->warmup_left > 0)
	{
		engine->warmup_left--;
		return;
	}
	trackDuration(engine->first_ms_avg, timing.total_ms);
	trackDuration(engine->generate_ms_avg, timing.total_ms);
	noteGenerateCost(timing.total_ms, cadence_us, end_us);
}

// Whether the GPU keeps up with the motion detail chosen in the settings (see FIT_WINDOW_PAIRS).
// The level itself is never changed here: what the user chose stays.
void FrameGenStage::reviewFit(double limit_us)
{
	if(engine->fit_pairs < FIT_WINDOW_PAIRS)
		return;
	const bool was_too_heavy = health_too_heavy.load();
	const bool too_heavy = was_too_heavy
		? engine->fit_over > FIT_WINDOW_PAIRS * FIT_CLEAR_SHARE
		: engine->fit_over >= FIT_WINDOW_PAIRS * FIT_OVER_SHARE;
	const double ready_ms = engine->fit_ready_sum_ms / engine->fit_pairs;
	engine->fit_pairs = 0;
	engine->fit_over = 0;
	engine->fit_ready_sum_ms = 0.0;
	if(too_heavy == was_too_heavy)
		return;
	health_too_heavy.store(too_heavy);
	if(too_heavy)
		qCInfo(robzFrameGen).noquote() << QString::asprintf(
			"[health] motion detail %s is more than this GPU keeps up with: generated frames were ready %.1f ms after the real frame, %.1f ms is what fits",
			motionDetailName(engine->divisor), ready_ms, (limit_us - FIT_MARGIN_US) / 1000.0);
	else
		qCInfo(robzFrameGen).noquote() << QString::asprintf(
			"[health] motion detail %s fits again: generated frames were ready %.1f ms after the real frame",
			motionDetailName(engine->divisor), ready_ms);
}

// ---------------------------------------------------------------------------- comparison view

void FrameGenStage::setCompareLabels(const QString &left_label, const QString &right_label)
{
	std::lock_guard<std::mutex> lock(compare_label_mutex);
	compare_left_label = left_label;
	compare_right_label = right_label;
}

void FrameGenStage::copyLeftHalf(AVFrame *dst, const AVFrame *src)
{
	if(!dst || !src || dst->format != src->format || dst->width != src->width || dst->height != src->height)
		return;
	const int w = dst->width;
	const int h = dst->height;
	const int half = (w / 2) & ~3;
	for(int y = 0; y < h; y++)
		memcpy(dst->data[0] + (size_t)y * dst->linesize[0], src->data[0] + (size_t)y * src->linesize[0], (size_t)half);
	if(dst->format == AV_PIX_FMT_NV12)
	{
		for(int y = 0; y < h / 2; y++)
			memcpy(dst->data[1] + (size_t)y * dst->linesize[1], src->data[1] + (size_t)y * src->linesize[1], (size_t)half);
	}
	else
	{
		for(int plane = 1; plane <= 2; plane++)
			for(int y = 0; y < h / 2; y++)
				memcpy(dst->data[plane] + (size_t)y * dst->linesize[plane], src->data[plane] + (size_t)y * src->linesize[plane], (size_t)half / 2);
	}
}

// Draws the dividing line and the two labels into the picture itself, so they also show in
// a recording (OBS).
void FrameGenStage::drawCompare(AVFrame *frame, int left_fps, int right_fps)
{
	const int w = frame->width;
	const int h = frame->height;
	const bool nv12 = frame->format == AV_PIX_FMT_NV12;
	QString left_text;
	QString right_text;
	{
		std::lock_guard<std::mutex> lock(compare_label_mutex);
		left_text = compare_left_label.arg(left_fps);
		right_text = compare_right_label.arg(right_fps);
	}

	// a label as a coverage bitmap, rendered once per text and picture size
	auto render = [h](LabelBitmap &bitmap, const QString &text) {
		if(bitmap.text == text && bitmap.frame_height == h)
			return;
		QFont font;
		font.setBold(true);
		font.setPixelSize(std::max(14, h / 30));
		const QFontMetrics metrics(font);
		const int pad = std::max(8, h / 90);
		bitmap.width = (metrics.horizontalAdvance(text) + 2 * pad + 3) & ~3;
		bitmap.height = ((metrics.height() + 2 * pad) + 1) & ~1;
		QImage image(bitmap.width, bitmap.height, QImage::Format_Grayscale8);
		image.fill(0);
		QPainter painter(&image);
		painter.setRenderHint(QPainter::TextAntialiasing);
		painter.setFont(font);
		painter.setPen(Qt::white);
		painter.drawText(QRect(0, 0, bitmap.width, bitmap.height), Qt::AlignCenter, text);
		painter.end();
		bitmap.coverage.resize((size_t)bitmap.width * bitmap.height);
		for(int y = 0; y < bitmap.height; y++)
			memcpy(bitmap.coverage.data() + (size_t)y * bitmap.width, image.constScanLine(y), (size_t)bitmap.width);
		bitmap.text = text;
		bitmap.frame_height = h;
	};
	render(compare_left_bitmap, left_text);
	render(compare_right_bitmap, right_text);

	// white text on a dark, colourless box
	auto blit = [&](const LabelBitmap &bitmap, int x0, int y0) {
		x0 &= ~1;
		y0 &= ~1;
		for(int y = 0; y < bitmap.height && y0 + y < h; y++)
		{
			uint8_t *row = frame->data[0] + (size_t)(y0 + y) * frame->linesize[0];
			const uint8_t *cov = bitmap.coverage.data() + (size_t)y * bitmap.width;
			for(int x = 0; x < bitmap.width && x0 + x < w; x++)
			{
				const int dimmed = 16 + (row[x0 + x] - 16) * 30 / 100;
				row[x0 + x] = (uint8_t)(dimmed + (235 - dimmed) * cov[x] / 255);
			}
		}
		for(int y = 0; y < bitmap.height / 2 && y0 / 2 + y < h / 2; y++)
		{
			if(nv12)
			{
				uint8_t *uv = frame->data[1] + (size_t)(y0 / 2 + y) * frame->linesize[1];
				for(int x = x0; x < std::min(w, x0 + bitmap.width); x++)
					uv[x] = (uint8_t)(128 + (uv[x] - 128) * 30 / 100);
			}
			else
			{
				for(int plane = 1; plane <= 2; plane++)
				{
					uint8_t *c = frame->data[plane] + (size_t)(y0 / 2 + y) * frame->linesize[plane];
					for(int x = x0 / 2; x < std::min(w / 2, (x0 + bitmap.width) / 2); x++)
						c[x] = (uint8_t)(128 + (c[x] - 128) * 30 / 100);
				}
			}
		}
	};
	const int margin = std::max(12, h / 40);
	blit(compare_left_bitmap, margin, margin);
	blit(compare_right_bitmap, w / 2 + margin, margin);

	// the dividing line
	const int line = std::max(2, (h / 360) & ~1);
	const int x_start = (w / 2 - line / 2) & ~1;
	for(int y = 0; y < h; y++)
		memset(frame->data[0] + (size_t)y * frame->linesize[0] + x_start, 235, (size_t)line);
	for(int y = 0; y < h / 2; y++)
	{
		if(nv12)
			memset(frame->data[1] + (size_t)y * frame->linesize[1] + x_start, 128, (size_t)line);
		else
			for(int plane = 1; plane <= 2; plane++)
				memset(frame->data[plane] + (size_t)y * frame->linesize[plane] + x_start / 2, 128, (size_t)std::max(1, line / 2));
	}
}
