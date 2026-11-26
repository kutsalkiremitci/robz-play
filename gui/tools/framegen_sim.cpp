// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// robzplay-framegen-sim - replays a video file into FrameGenStage as if it were a live
// stream and reports what the renderer would have been handed, and when.
//
//   robzplay-framegen-sim --input stream60.mp4 --model models/rife-v4.6 [--fps 60]
//                         [--frames 300] [--jitter-ms 3] [--flowdiv 2] [--off 1]
//                         [--dips 10] [--hole-at 400] [--stall-at 600]
//
// --dips P       every new picture of the clip comes one frame later with P % probability,
//                as a game does that falls below its frame rate
// --hole-at N    at frame N the stream sends nothing for two frames (the console left them out)
// --stall-at N   from frame N on everything is held up for 45 ms and then arrives at once
// --picture-late-ms L   new pictures arrive later than repeats, by 0.5 L to 1.3 L (a real PS5
//                stream at 100 Mbit/s: about 11.5 in the middle)
// --p010 1       the clip is sent as a 10-bit HDR stream would arrive (P010, BT.2020, PQ);
//                the pictures are the same, every 8-bit sample becomes the upper byte of a word
// --still-at N --still-frames M   from frame N on the picture stands still for M frames (a
//                menu, a pause), then the clip goes on
// --settle S     seconds at the start that are left out of the numbers (default 1); a slow
//                GPU needs several before its first frames are through
//
// ROBZPLAY_FRAMEGEN_GPU=<index> picks the GPU (robzfg-lab gpus lists them).
//
// The frames are decoded up front and submitted on a wall-clock schedule, so the
// numbers reflect the stage's own pacing rather than decode speed.

#include "framegen.h"

#include <QCoreApplication>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <chiaki/time.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#ifdef _WIN32
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace
{

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

bool loadFrames(const std::string &path, int max_frames, std::vector<AVFrame *> &frames)
{
	AVFormatContext *fmt = nullptr;
	if(avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
		return false;
	if(avformat_find_stream_info(fmt, nullptr) < 0)
		return false;
	const AVCodec *codec = nullptr;
	const int stream_index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
	if(stream_index < 0 || !codec)
		return false;
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(ctx, fmt->streams[stream_index]->codecpar);
	if(avcodec_open2(ctx, codec, nullptr) < 0)
		return false;
	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	bool eof = false;
	while((int)frames.size() < max_frames)
	{
		if(!eof)
		{
			if(av_read_frame(fmt, packet) < 0)
			{
				eof = true;
				avcodec_send_packet(ctx, nullptr);
			}
			else
			{
				if(packet->stream_index == stream_index)
					avcodec_send_packet(ctx, packet);
				av_packet_unref(packet);
			}
		}
		bool got_end = false;
		for(;;)
		{
			const int received = avcodec_receive_frame(ctx, frame);
			if(received == AVERROR(EAGAIN))
				break;
			if(received < 0)
			{
				got_end = true;
				break;
			}
			frames.push_back(av_frame_clone(frame));
			av_frame_unref(frame);
			if((int)frames.size() >= max_frames)
				break;
		}
		if(got_end)
			break;
	}
	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&ctx);
	avformat_close_input(&fmt);
	return !frames.empty();
}

double percentile(std::vector<double> values, double p)
{
	if(values.empty())
		return 0.0;
	std::sort(values.begin(), values.end());
	return values[(size_t)std::min<double>(values.size() - 1, std::floor(p * (values.size() - 1) + 0.5))];
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	std::map<std::string, std::string> args;
	for(int i = 1; i + 1 < argc; i += 2)
		args[std::string(argv[i]).substr(2)] = argv[i + 1];
	if(!args.count("input") || !args.count("model"))
	{
		fprintf(stderr, "usage: robzplay-framegen-sim --input <video> --model <dir> [--fps 60] [--frames 300] [--jitter-ms 3] [--flowdiv 2] [--target 60|120] [--off 1]\n");
		return 1;
	}
	const double fps = args.count("fps") ? atof(args["fps"].c_str()) : 60.0;
	const int max_frames = args.count("frames") ? atoi(args["frames"].c_str()) : 300;
	const double jitter_ms = args.count("jitter-ms") ? atof(args["jitter-ms"].c_str()) : 0.0;
	const double dips_percent = args.count("dips") ? atof(args["dips"].c_str()) : 0.0;
	const int hole_at = args.count("hole-at") ? atoi(args["hole-at"].c_str()) : 0;
	const int stall_at = args.count("stall-at") ? atoi(args["stall-at"].c_str()) : 0;
	const double settle_s = args.count("settle") ? atof(args["settle"].c_str()) : 1.0;
	const int still_at = args.count("still-at") ? atoi(args["still-at"].c_str()) : 0;
	const int still_frames = args.count("still-frames") ? atoi(args["still-frames"].c_str()) : 0;
	const double picture_late_ms = args.count("picture-late-ms") ? atof(args["picture-late-ms"].c_str()) : 0.0;

	std::vector<AVFrame *> frames;
	if(!loadFrames(args["input"], max_frames, frames))
	{
		fprintf(stderr, "could not decode %s\n", args["input"].c_str());
		return 1;
	}
	if(args.count("p010"))
	{
		for(AVFrame *&frame : frames)
		{
			if(frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P)
			{
				fprintf(stderr, "--p010 needs a clip that decodes to yuv420p\n");
				return 1;
			}
			AVFrame *wide = av_frame_alloc();
			wide->format = AV_PIX_FMT_P010LE;
			wide->width = frame->width;
			wide->height = frame->height;
			if(av_frame_get_buffer(wide, 0) < 0)
				return 1;
			for(int y = 0; y < frame->height; y++)
			{
				const uint8_t *src = frame->data[0] + (size_t)y * frame->linesize[0];
				uint8_t *dst = wide->data[0] + (size_t)y * wide->linesize[0];
				for(int x = 0; x < frame->width; x++)
				{
					dst[2 * x] = 0;
					dst[2 * x + 1] = src[x];
				}
			}
			for(int y = 0; y < frame->height / 2; y++)
			{
				const uint8_t *u = frame->data[1] + (size_t)y * frame->linesize[1];
				const uint8_t *v = frame->data[2] + (size_t)y * frame->linesize[2];
				uint8_t *dst = wide->data[1] + (size_t)y * wide->linesize[1];
				for(int x = 0; x < frame->width / 2; x++)
				{
					dst[4 * x] = 0;
					dst[4 * x + 1] = u[x];
					dst[4 * x + 2] = 0;
					dst[4 * x + 3] = v[x];
				}
			}
			wide->colorspace = AVCOL_SPC_BT2020_NCL;
			wide->color_primaries = AVCOL_PRI_BT2020;
			wide->color_trc = AVCOL_TRC_SMPTE2084;
			wide->color_range = AVCOL_RANGE_MPEG;
			av_frame_free(&frame);
			frame = wide;
		}
	}

	// The order frames are sent in: the clip as it is, or with a repeat put in front of a
	// new picture now and then. (In a "30 in 60" clip every second frame is a new picture.)
	std::vector<size_t> order;
	{
		std::mt19937 dip_rng(11);
		std::uniform_real_distribution<double> chance(0.0, 100.0);
		for(size_t i = 0; i < frames.size(); i++)
		{
			if(dips_percent > 0.0 && i > 0 && i % 2 == 0 && chance(dip_rng) < dips_percent)
				order.push_back(i - 1);
			if(still_at > 0 && (int)i == still_at)
				for(int repeat = 0; repeat < still_frames; repeat++)
					order.push_back(i - 1);
			order.push_back(i);
		}
	}
	printf("input    %s: %zu frames, %dx%d, replayed at %.2f fps, arrival jitter +/- %.1f ms\n", args["input"].c_str(),
		frames.size(), frames[0]->width, frames[0]->height, fps, jitter_ms);

	std::mutex present_mutex;
	std::vector<uint64_t> present_times;

	FrameGenStage stage;
	FrameGenStage::Config config;
	config.enabled = !args.count("off");
	config.model_dir = QString::fromLocal8Bit(args["model"].c_str());
	config.flow_divisor = args.count("flowdiv") ? atoi(args["flowdiv"].c_str()) : 2;
	config.stream_fps = (unsigned int)fps;
	config.target_fps = args.count("target") ? (unsigned int)atoi(args["target"].c_str()) : 60;
	stage.start(config, [&](ChiakiFfmpegFrame frame, int32_t, qint64) {
		{
			std::lock_guard<std::mutex> lock(present_mutex);
			present_times.push_back(chiaki_time_now_monotonic_us());
		}
		av_frame_free(&frame.frame);
	});

	// give the model time to load, as a real session does while it connects
	for(int waited = 0; waited < 900 && (waited < 40 || stage.status() == QStringLiteral("loading")); waited++)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	printf("status   %s\n", stage.status().toUtf8().constData());

	std::mt19937 rng(7);
	std::uniform_real_distribution<double> jitter(-jitter_ms, jitter_ms);
	std::uniform_real_distribution<double> picture_late(0.5, 1.3);
	// --full-rate 1: every frame of the clip is a new picture (a 60 fps game)
	const bool fps_content_is_full = args.count("full-rate") > 0;
	const auto begin = std::chrono::steady_clock::now();
	uint64_t first_submit_us = 0;
	std::vector<uint64_t> submit_times;
	double hole_ms = 0.0;
	double stall_until_ms = 0.0;
	bool settled = false;
	for(size_t i = 0; i < order.size(); i++)
	{
		if(hole_at > 0 && (int)i == hole_at)
			hole_ms = 2.0 * 1000.0 / fps;
		double offset_ms = i * 1000.0 / fps + hole_ms + (jitter_ms > 0 ? jitter(rng) : 0.0);
		// a frame of the clip that is sent for the first time is a new picture
		if(picture_late_ms > 0.0 && (i == 0 || order[i] != order[i - 1]) && (order[i] % 2 == 0 || fps_content_is_full))
			offset_ms += picture_late_ms * picture_late(rng);
		if(stall_at > 0 && (int)i == stall_at)
			stall_until_ms = offset_ms + 45.0;
		offset_ms = std::max(offset_ms, stall_until_ms);
		// sleep most of the way, then spin: the replay itself must not add timing noise
		const auto target = begin + std::chrono::microseconds((int64_t)(std::max(0.0, offset_ms) * 1000.0));
		waitUntil(target);
		submit_times.push_back(chiaki_time_now_monotonic_us());
		ChiakiFfmpegFrame frame = {};
		frame.frame = av_frame_clone(frames[order[i]]);
		frame.pts = i / fps;
		frame.duration = 1.0 / fps;
		if(!first_submit_us)
			first_submit_us = chiaki_time_now_monotonic_us();
		// what the stage counted while settling is not part of the result
		if(!settled && chiaki_time_now_monotonic_us() - first_submit_us >= (uint64_t)(settle_s * 1000000.0))
		{
			settled = true;
			stage.takeHealth();
		}
		stage.submit(frame, 0, (qint64)chiaki_time_now_monotonic_us());
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	const QString final_status = stage.status();
	const double generate_ms = stage.generateMs();
	const FrameGenStage::Health health = stage.takeHealth();
	stage.stop();

	std::vector<double> intervals;
	// skip the start: the detector and the timeline are still settling
	size_t first = 0;
	while(first < present_times.size() && present_times[first] < first_submit_us + (uint64_t)(settle_s * 1000000.0))
		first++;
	for(size_t i = first + 1; i < present_times.size(); i++)
		intervals.push_back((present_times[i] - present_times[i - 1]) / 1000.0);
	const double span_s = present_times.size() > first + 1 ? (present_times.back() - present_times[first]) / 1000000.0 : 0.0;

	printf("status   %s\n", final_status.toUtf8().constData());
	printf("submitted %zu frames, presented %zu\n", order.size(), present_times.size());
	printf("stage    real %u | generated %u | repeats %u | held %u | late %u (max %.1f ms) | dropped late %u | passed on untouched %u | probed %u\n",
		health.real_frames, health.generated_frames, health.repeats, health.held, health.late_frames, health.late_max_ms,
		health.dropped_late, health.overloads, health.probe_frames);
	printf("         detail %d | generating avg %.2f ms max %.1f ms | delay %.1f ms | pictures late by %.1f ms | frames per picture %d | clock set anew %u\n",
		health.motion_divisor, health.generate_avg_ms, health.generate_max_ms, health.latency_ms, health.picture_late_ms,
		health.source_slots, health.clock_jumps);
	{
		std::vector<double> submit_intervals;
		for(size_t i = 1; i < submit_times.size(); i++)
			submit_intervals.push_back((submit_times[i] - submit_times[i - 1]) / 1000.0);
		printf("arrival interval, ms: min %.2f | median %.2f | max %.2f\n", percentile(submit_intervals, 0.0),
			percentile(submit_intervals, 0.5), percentile(submit_intervals, 1.0));
	}
	if(span_s > 0)
		printf("output   %.2f fps after the first %.0f s\n", intervals.size() / span_s, settle_s);
	printf("generate %.2f ms per frame (recent average)\n", generate_ms);
	if(!intervals.empty())
	{
		double mean = 0.0;
		for(double v : intervals)
			mean += v;
		mean /= intervals.size();
		double variance = 0.0;
		for(double v : intervals)
			variance += (v - mean) * (v - mean);
		printf("interval between presented frames, ms\n");
		printf("  mean %.2f | stddev %.2f | min %.2f | p5 %.2f | median %.2f | p95 %.2f | max %.2f\n", mean,
			std::sqrt(variance / intervals.size()), percentile(intervals, 0.0), percentile(intervals, 0.05),
			percentile(intervals, 0.5), percentile(intervals, 0.95), percentile(intervals, 1.0));
	}
	for(AVFrame *frame : frames)
		av_frame_free(&frame);
	return 0;
}
