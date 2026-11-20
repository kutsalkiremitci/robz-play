// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// robzplay-framegen-sim - replays a video file into FrameGenStage as if it were a live
// stream and reports what the renderer would have been handed, and when.
//
//   robzplay-framegen-sim --input stream60.mp4 --model models/rife-v4.6 [--fps 60]
//                         [--frames 300] [--jitter-ms 3] [--flowdiv 2] [--off 1]
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

namespace
{

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

	std::vector<AVFrame *> frames;
	if(!loadFrames(args["input"], max_frames, frames))
	{
		fprintf(stderr, "could not decode %s\n", args["input"].c_str());
		return 1;
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
	std::this_thread::sleep_for(std::chrono::seconds(4));
	printf("status   %s\n", stage.status().toUtf8().constData());

	std::mt19937 rng(7);
	std::uniform_real_distribution<double> jitter(-jitter_ms, jitter_ms);
	const auto begin = std::chrono::steady_clock::now();
	uint64_t first_submit_us = 0;
	std::vector<uint64_t> submit_times;
	for(size_t i = 0; i < frames.size(); i++)
	{
		const double offset_ms = i * 1000.0 / fps + (jitter_ms > 0 ? jitter(rng) : 0.0);
		// sleep most of the way, then spin: the replay itself must not add timing noise
		const auto target = begin + std::chrono::microseconds((int64_t)(std::max(0.0, offset_ms) * 1000.0));
		while(std::chrono::steady_clock::now() < target)
			std::this_thread::yield();
		submit_times.push_back(chiaki_time_now_monotonic_us());
		ChiakiFfmpegFrame frame = {};
		frame.frame = av_frame_clone(frames[i]);
		frame.pts = i / fps;
		frame.duration = 1.0 / fps;
		if(!first_submit_us)
			first_submit_us = chiaki_time_now_monotonic_us();
		stage.submit(frame, 0, (qint64)chiaki_time_now_monotonic_us());
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	const QString final_status = stage.status();
	const double generate_ms = stage.generateMs();
	stage.stop();

	std::vector<double> intervals;
	// skip the first second: the detector and the cadence estimate are still settling
	size_t first = 0;
	while(first < present_times.size() && present_times[first] < first_submit_us + 1000000)
		first++;
	for(size_t i = first + 1; i < present_times.size(); i++)
		intervals.push_back((present_times[i] - present_times[i - 1]) / 1000.0);
	const double span_s = present_times.size() > first + 1 ? (present_times.back() - present_times[first]) / 1000000.0 : 0.0;

	printf("status   %s\n", final_status.toUtf8().constData());
	printf("submitted %zu frames, presented %zu\n", frames.size(), present_times.size());
	{
		std::vector<double> submit_intervals;
		for(size_t i = 1; i < submit_times.size(); i++)
			submit_intervals.push_back((submit_times[i] - submit_times[i - 1]) / 1000.0);
		printf("arrival interval, ms: min %.2f | median %.2f | max %.2f\n", percentile(submit_intervals, 0.0),
			percentile(submit_intervals, 0.5), percentile(submit_intervals, 1.0));
	}
	if(span_s > 0)
		printf("output   %.2f fps after the first second\n", intervals.size() / span_s);
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
