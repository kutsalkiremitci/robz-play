// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Robz Play - frame generation stage of the stream pipeline.
//
// Sits between the video decoder and the renderer. For a game that renders 30 fps
// inside a 60 fps stream it drops the repeated frames, synthesizes the missing
// in-between frames (robzfg / RIFE) and hands everything to the renderer on a steady
// cadence: one generated frame per real one for 60 fps, three for 120 fps. Anything it
// cannot improve (content already at the target rate, unsupported pixel formats, a
// model that failed to load) passes through untouched.
//
// Two threads do the work. The worker recognises repeats and generates frames; the
// presenter hands frames on at the moment they are due. Keeping them apart is what lets
// several frames be generated in a row without the ones already finished going out late.

#ifndef ROBZPLAY_FRAMEGEN_H
#define ROBZPLAY_FRAMEGEN_H

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <chiaki/ffmpegdecoder.h>
#include <robzfg/frame.h>

class FrameGenStage : public QObject
{
	Q_OBJECT
	// True while a stream is running with frame generation switched on.
	Q_PROPERTY(bool enabled READ enabled NOTIFY stateChanged)
	// Short human-readable state: "loading", "generating", "passing through (...)", an error ...
	Q_PROPERTY(QString status READ status NOTIFY stateChanged)
	// Frames per second of real (non-repeated) game frames arriving from the console.
	Q_PROPERTY(double sourceFps READ sourceFps NOTIFY statsChanged)
	// Frames per second handed to the renderer.
	Q_PROPERTY(double outputFps READ outputFps NOTIFY statsChanged)
	// Time to synthesize one frame, in milliseconds (recent average).
	Q_PROPERTY(double generateMs READ generateMs NOTIFY statsChanged)
	// The graphics card does not keep up with the motion detail chosen in the settings:
	// generated frames are left out. The stage keeps the level; lowering it is up to the user.
	Q_PROPERTY(bool detailTooHeavy READ detailTooHeavy NOTIFY statsChanged)

public:
	struct Config
	{
		bool enabled = false;
		QString model_dir;
		int flow_divisor = 2;
		// Output rate to aim for: 60 or 120 frames per second.
		unsigned int target_fps = 60;
		// Keep static picture elements (HUD text, map frame) from being dragged by scene motion.
		bool protect_static = true;
		unsigned int stream_fps = 60;
	};

	// Called for every frame that should be shown; takes ownership of the frame. Runs on the
	// presenter thread, or on the thread calling submit() while the model is still loading.
	using PresentFn = std::function<void(ChiakiFfmpegFrame frame, int32_t frames_lost, qint64 delivery_us)>;

	explicit FrameGenStage(QObject *parent = nullptr);
	~FrameGenStage() override;

	void start(const Config &config, PresentFn present);
	void stop();

	// Diagnostic: runs the stage for a few seconds on synthetic 30 fps video inside the
	// running application and prints the outcome to stderr. Started by launching with
	// ROBZPLAY_FRAMEGEN_SELFTEST=1; checks that the model loads and generates frames on
	// this machine without needing a console.
	// `on_presented` (optional) is called for every frame the stage hands on.
	void runSelfTest(const Config &config, std::function<void()> on_presented = {});

	// Hands a decoded frame to the stage (takes ownership). Safe to call from any one thread.
	// `frames_lost`: frames of the stream missing in front of this one, as the decoder
	// reports them. `frames_skipped`: frames that were decoded but never handed on (the
	// decoder gives out only the newest one). Both keep the stage's count of the stream's
	// frames right, which is what its timing rests on.
	void submit(ChiakiFfmpegFrame frame, int32_t frames_lost, qint64 delivery_us, int32_t frames_skipped = 0);

	// Comparison view, switched on and off while a stream runs: the left half of every picture
	// shows only the real frames (as the stream looks without frame generation), the right
	// half the generated output, with a line between them and a label on each side.
	// `left_label` and `right_label` take the frame rate as %1.
	void setCompareSplit(bool on) { compare_split.store(on); }
	void setCompareLabels(const QString &left_label, const QString &right_label);

	// What the stage did since the last call; the counters start over. Robz Play writes it to
	// the log once a minute, so stutter nobody was watching can be read afterwards. Safe to
	// call from any thread.
	struct Health
	{
		unsigned int real_frames = 0;      // real frames handed on
		unsigned int generated_frames = 0; // generated frames handed on
		unsigned int repeats = 0;          // repeated frames recognised and left out
		unsigned int overloads = 0;        // frames passed on untouched because the worker was behind
		unsigned int queue_max = 0;        // most decoded frames waiting for the worker at once
		unsigned int late_frames = 0;      // frames handed on more than 4 ms after they were due
		double late_max_ms = 0.0;
		double generate_avg_ms = 0.0;      // per generated frame
		double generate_max_ms = 0.0;
		int motion_divisor = 0;            // motion detail in use: 1 high, 2 balanced, 4 fast
		// generated frames left out because they could not have been ready for their slot:
		// the picture before them stayed up instead
		unsigned int held = 0;
		// generated frames dropped at the last moment: late, with the next frame already due
		unsigned int dropped_late = 0;
		// times the stream's clock had to be set anew in one step (the console sent nothing
		// for a frame or two, or the kind of content changed): see StreamClock::jumps
		unsigned int clock_jumps = 0;
		double latency_ms = 0.0;           // delay between a frame's slot and its showing, now
		double picture_late_ms = 0.0;      // how much later than repeats new pictures arrive, now
		int source_slots = 0;              // stream frames per new picture of the game (2: 30 fps)
		// frames generated unseen, only to learn what a frame costs after one was left out
		unsigned int probe_frames = 0;
	};
	Health takeHealth();

	bool enabled() const { return running.load() && config.enabled; }
	QString status() const;
	double sourceFps() const { return stat_source_fps.load(); }
	double outputFps() const { return stat_output_fps.load(); }
	double generateMs() const { return stat_generate_ms.load(); }
	bool detailTooHeavy() const { return running.load() && config.enabled && health_too_heavy.load(); }

signals:
	void stateChanged();
	void statsChanged();

private:
	struct Item
	{
		ChiakiFfmpegFrame frame = {};
		int32_t frames_lost = 0;
		qint64 delivery_us = 0;
		uint64_t arrival_us = 0;
		bool generated = false;
		int32_t skipped = 0;
	};
	struct Scheduled
	{
		Item item;
		uint64_t due_us = 0;
	};
	struct Engine;

	// A thread's place to sleep until there is work or a deadline has come. On Windows it
	// waits on an event and a high-resolution timer: the default timer tick (15.6 ms) is as
	// long as the frame intervals this stage has to hit.
	struct Waiter
	{
		void open();
		void close();
		void signal();
		// Called with `lock` held; returns with it held.
		void wait(std::unique_lock<std::mutex> &lock, uint64_t timeout_us);

		std::condition_variable condition;
		void *event = nullptr;
		void *timer = nullptr;
	};

	void workerMain();
	void presenterMain();
	void process(Item &item);
	void reviewFit(double limit_us);
	void noteGenerateCost(double total_ms, double cadence_us, uint64_t now_us);
	void probeCost(const robzfg::Nv12View &view, double cadence_us);
	void schedule(Item &item, uint64_t due_us);
	void scheduleNow(Item &item);
	void flushSchedule();
	void setStatus(const std::string &text);
	void publishStats();

	Config config;
	PresentFn present;
	std::atomic<bool> running{false};
	// set once the model is loaded; until then (and if loading fails) frames bypass the worker
	std::atomic<bool> engine_ready{false};

	// decoded frames waiting for the worker
	std::thread worker;
	std::mutex queue_mutex;
	std::deque<Item> queue;
	Waiter work_waiter;
	bool stop_requested = false;

	// frames waiting for their turn on the output timeline, earliest first
	std::thread presenter;
	std::mutex schedule_mutex;
	std::deque<Scheduled> scheduled;
	Waiter present_waiter;
	bool presenter_stop = false;

	// comparison view (see setCompareSplit)
	std::atomic<bool> compare_split{false};
	std::mutex compare_label_mutex;
	QString compare_left_label = QStringLiteral("%1 FPS");
	QString compare_right_label = QStringLiteral("%1 FPS");
	// worker thread: the latest real frame, untouched, for the left half
	AVFrame *compare_prev = nullptr;
	struct LabelBitmap
	{
		QString text;
		int frame_height = 0;
		int width = 0;
		int height = 0;
		std::vector<uint8_t> coverage;
	};
	LabelBitmap compare_left_bitmap;
	LabelBitmap compare_right_bitmap;
	void drawCompare(AVFrame *frame, int left_fps, int right_fps);
	static void copyLeftHalf(AVFrame *dst, const AVFrame *src);

	// worker-thread state
	std::unique_ptr<Engine> engine;
	// the latest due time handed out: due times never run backwards
	uint64_t last_due_us = 0;
	int32_t carried_frames_lost = 0;
	std::vector<uint8_t> scratch_uv;
	std::vector<uint8_t> scratch_out_uv;
	uint64_t stats_window_start_us = 0;
	std::atomic<unsigned int> stats_unique{0};
	std::atomic<unsigned int> stats_presented{0};

	mutable std::mutex status_mutex;
	std::string status_text;
	std::atomic<bool> status_dirty{false};
	std::atomic<double> stat_source_fps{0.0};
	std::atomic<double> stat_output_fps{0.0};
	std::atomic<double> stat_generate_ms{0.0};
	// see takeHealth()
	std::atomic<unsigned int> health_real{0};
	std::atomic<unsigned int> health_generated{0};
	std::atomic<unsigned int> health_repeats{0};
	std::atomic<unsigned int> health_overloads{0};
	std::atomic<unsigned int> health_queue_max{0};
	std::atomic<unsigned int> health_late{0};
	std::atomic<uint64_t> health_late_max_us{0};
	std::atomic<unsigned int> health_generate_count{0};
	std::atomic<uint64_t> health_generate_sum_us{0};
	std::atomic<uint64_t> health_generate_max_us{0};
	std::atomic<int> health_divisor{0};
	std::atomic<bool> health_too_heavy{false};
	std::atomic<unsigned int> health_held{0};
	std::atomic<unsigned int> health_dropped{0};
	std::atomic<unsigned int> health_clock_jumps{0};
	std::atomic<uint64_t> health_latency_us{0};
	std::atomic<uint64_t> health_picture_late_us{0};
	std::atomic<int> health_source_slots{0};
	std::atomic<unsigned int> health_probes{0};
	QTimer poll_timer;
};

#endif // ROBZPLAY_FRAMEGEN_H
