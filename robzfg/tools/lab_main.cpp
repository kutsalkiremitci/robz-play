// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// robzfg-lab - measures Robz Play's frame generation on video files, away from a live stream.
//
//   robzfg-lab gpus
//   robzfg-lab bench   --input clip.mp4 --model models/rife-v4.6 [--frames 120]
//   robzfg-lab quality --input clip60.mp4 --model models/rife-v4.6 [--frames 120] [--out mid.mp4]
//   robzfg-lab dedup   --input stream60.mp4 [--frames 600]
//   robzfg-lab convert --input stream60.mp4 --model models/rife-v4.6 --out smooth.mp4 [--frames 600]
//
// bench    time per generated frame (upload + inference + download)
// quality  drops every second frame of a true 60 fps clip, regenerates it and compares it
//          with the frame that was dropped (luma PSNR), next to two naive baselines
// dedup    prints how many frames of a clip are repeats, as the live path would see them
// convert  the full offline pipeline: drop repeats, generate in-between frames, write a file

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

#include "robzfg/dedup.h"
#include "robzfg/rife_interpolator.h"

using namespace robzfg;

namespace
{

struct Picture
{
	int width = 0;
	int height = 0;
	std::vector<uint8_t> y;
	std::vector<uint8_t> uv;

	Nv12View view() const
	{
		Nv12View v;
		v.width = width;
		v.height = height;
		v.y = y.data();
		v.y_stride = width;
		v.uv = uv.data();
		v.uv_stride = width;
		return v;
	}

	Nv12Mut mut()
	{
		Nv12Mut v;
		v.width = width;
		v.height = height;
		v.y = y.data();
		v.y_stride = width;
		v.uv = uv.data();
		v.uv_stride = width;
		return v;
	}

	void allocate(int w, int h)
	{
		width = w;
		height = h;
		y.assign((size_t)w * h, 0);
		uv.assign((size_t)w * h / 2, 128);
	}
};

struct Clip
{
	std::vector<Picture> frames;
	ColorInfo color;
	double fps = 0.0;
};

bool toNv12(const AVFrame *frame, Picture &out)
{
	const int w = frame->width;
	const int h = frame->height;
	out.allocate(w, h);
	for(int row = 0; row < h; row++)
		memcpy(out.y.data() + (size_t)row * w, frame->data[0] + (size_t)row * frame->linesize[0], (size_t)w);

	switch(frame->format)
	{
		case AV_PIX_FMT_NV12:
			for(int row = 0; row < h / 2; row++)
				memcpy(out.uv.data() + (size_t)row * w, frame->data[1] + (size_t)row * frame->linesize[1], (size_t)w);
			return true;
		case AV_PIX_FMT_YUV420P:
		case AV_PIX_FMT_YUVJ420P:
			for(int row = 0; row < h / 2; row++)
			{
				const uint8_t *u = frame->data[1] + (size_t)row * frame->linesize[1];
				const uint8_t *v = frame->data[2] + (size_t)row * frame->linesize[2];
				uint8_t *dst = out.uv.data() + (size_t)row * w;
				for(int x = 0; x < w / 2; x++)
				{
					dst[2 * x] = u[x];
					dst[2 * x + 1] = v[x];
				}
			}
			return true;
		default:
			return false;
	}
}

bool loadClip(const std::string &path, int max_frames, Clip &clip)
{
	AVFormatContext *fmt = nullptr;
	if(avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
	{
		fprintf(stderr, "could not open %s\n", path.c_str());
		return false;
	}
	if(avformat_find_stream_info(fmt, nullptr) < 0)
	{
		avformat_close_input(&fmt);
		return false;
	}
	const AVCodec *codec = nullptr;
	const int stream_index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
	if(stream_index < 0 || !codec)
	{
		fprintf(stderr, "no video stream in %s\n", path.c_str());
		avformat_close_input(&fmt);
		return false;
	}
	AVStream *stream = fmt->streams[stream_index];
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(ctx, stream->codecpar);
	ctx->thread_count = 0;
	if(avcodec_open2(ctx, codec, nullptr) < 0)
	{
		avcodec_free_context(&ctx);
		avformat_close_input(&fmt);
		return false;
	}
	const AVRational rate = av_guess_frame_rate(fmt, stream, nullptr);
	clip.fps = rate.den ? av_q2d(rate) : 0.0;

	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	bool ok = true;
	bool draining = false;
	bool color_known = false;
	while(ok && (int)clip.frames.size() < max_frames)
	{
		if(!draining)
		{
			const int read = av_read_frame(fmt, packet);
			if(read < 0)
			{
				draining = true;
				avcodec_send_packet(ctx, nullptr);
			}
			else
			{
				if(packet->stream_index == stream_index)
					avcodec_send_packet(ctx, packet);
				av_packet_unref(packet);
			}
		}
		for(;;)
		{
			const int received = avcodec_receive_frame(ctx, frame);
			if(received == AVERROR(EAGAIN))
				break;
			if(received < 0)
			{
				draining = true;
				max_frames = (int)clip.frames.size(); // end of stream
				break;
			}
			if(!color_known)
			{
				clip.color.full_range = frame->color_range == AVCOL_RANGE_JPEG || frame->format == AV_PIX_FMT_YUVJ420P;
				const bool bt601 = frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M
					|| (frame->colorspace == AVCOL_SPC_UNSPECIFIED && frame->height < 720);
				clip.color.matrix = bt601 ? ColorMatrix::BT601 : ColorMatrix::BT709;
				color_known = true;
			}
			Picture picture;
			if(!toNv12(frame, picture))
			{
				fprintf(stderr, "unsupported pixel format %s (8-bit 4:2:0 only)\n",
					av_get_pix_fmt_name((AVPixelFormat)frame->format));
				ok = false;
				break;
			}
			clip.frames.push_back(std::move(picture));
			av_frame_unref(frame);
			if((int)clip.frames.size() >= max_frames)
				break;
		}
	}
	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&ctx);
	avformat_close_input(&fmt);
	return ok && !clip.frames.empty();
}

double lumaPsnr(const Picture &a, const Picture &b)
{
	uint64_t sse = 0;
	const size_t n = a.y.size();
	for(size_t i = 0; i < n; i++)
	{
		const int delta = (int)a.y[i] - (int)b.y[i];
		sse += (uint64_t)(delta * delta);
	}
	if(sse == 0)
		return 99.0;
	const double mse = (double)sse / (double)n;
	return 10.0 * std::log10(255.0 * 255.0 / mse);
}

struct Region
{
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;
};

// "x,y,w,h;x,y,w,h;..." - areas of special interest, e.g. where a HUD sits
std::vector<Region> parseRegions(const std::string &text)
{
	std::vector<Region> regions;
	size_t pos = 0;
	while(pos < text.size())
	{
		size_t end = text.find(';', pos);
		if(end == std::string::npos)
			end = text.size();
		Region region;
		if(sscanf(text.substr(pos, end - pos).c_str(), "%d,%d,%d,%d", &region.x, &region.y, &region.w, &region.h) == 4)
			regions.push_back(region);
		pos = end + 1;
	}
	return regions;
}

double lumaPsnrIn(const Picture &a, const Picture &b, const std::vector<Region> &regions)
{
	uint64_t sse = 0;
	uint64_t count = 0;
	for(const Region &region : regions)
	{
		for(int y = std::max(0, region.y); y < std::min(a.height, region.y + region.h); y++)
		{
			for(int x = std::max(0, region.x); x < std::min(a.width, region.x + region.w); x++)
			{
				const int delta = (int)a.y[(size_t)y * a.width + x] - (int)b.y[(size_t)y * b.width + x];
				sse += (uint64_t)(delta * delta);
				count++;
			}
		}
	}
	if(!count)
		return 0.0;
	if(sse == 0)
		return 99.0;
	return 10.0 * std::log10(255.0 * 255.0 / ((double)sse / (double)count));
}

// PSNR over the pixels inside `regions` that did not change from frame `a` through the real
// in-between frame to frame `b`: the static picture elements (HUD text, frames, icons).
// Also reports the largest error among them, which is what shows up as shimmer.
double staticPsnrIn(const Picture &generated, const Picture &truth, const Picture &a, const Picture &b,
	const std::vector<Region> &regions, int *worst_error)
{
	uint64_t sse = 0;
	uint64_t count = 0;
	int worst = 0;
	for(const Region &region : regions)
	{
		for(int y = std::max(0, region.y); y < std::min(truth.height, region.y + region.h); y++)
		{
			for(int x = std::max(0, region.x); x < std::min(truth.width, region.x + region.w); x++)
			{
				const size_t i = (size_t)y * truth.width + x;
				if(std::abs((int)a.y[i] - (int)truth.y[i]) > 1 || std::abs((int)b.y[i] - (int)truth.y[i]) > 1)
					continue;
				const int delta = std::abs((int)generated.y[i] - (int)truth.y[i]);
				sse += (uint64_t)(delta * delta);
				worst = std::max(worst, delta);
				count++;
			}
		}
	}
	if(worst_error)
		*worst_error = worst;
	if(!count)
		return 0.0;
	if(sse == 0)
		return 99.0;
	return 10.0 * std::log10(255.0 * 255.0 / ((double)sse / (double)count));
}

// Fine detail of a picture: mean absolute response of a Laplacian over the luma plane.
// The ratio between a generated frame and the real frame it stands in for says how crisp
// the generated one is: 1.0 keeps all the detail, lower is softer. (PSNR cannot tell:
// it rewards a blurred frame over a crisp one that is a fraction of a pixel off.)
double fineDetail(const Picture &picture)
{
	uint64_t sum = 0;
	uint64_t count = 0;
	const int w = picture.width;
	for(int y = 1; y < picture.height - 1; y++)
	{
		const uint8_t *row = picture.y.data() + (size_t)y * w;
		for(int x = 1; x < w - 1; x++)
		{
			const int laplacian = 4 * row[x] - row[x - 1] - row[x + 1] - row[x - w] - row[x + w];
			sum += (uint64_t)std::abs(laplacian);
			count++;
		}
	}
	return count ? (double)sum / (double)count : 0.0;
}

void blend(const Picture &a, const Picture &b, Picture &out)
{
	out.allocate(a.width, a.height);
	for(size_t i = 0; i < a.y.size(); i++)
		out.y[i] = (uint8_t)(((int)a.y[i] + (int)b.y[i] + 1) / 2);
	for(size_t i = 0; i < a.uv.size(); i++)
		out.uv[i] = (uint8_t)(((int)a.uv[i] + (int)b.uv[i] + 1) / 2);
}

struct Stats
{
	std::vector<double> values;
	void add(double v) { values.push_back(v); }
	double percentile(double p) const
	{
		if(values.empty())
			return 0.0;
		std::vector<double> sorted = values;
		std::sort(sorted.begin(), sorted.end());
		const size_t index = (size_t)std::min<double>(sorted.size() - 1, std::floor(p * (sorted.size() - 1) + 0.5));
		return sorted[index];
	}
	double mean() const
	{
		if(values.empty())
			return 0.0;
		double sum = 0.0;
		for(double v : values)
			sum += v;
		return sum / values.size();
	}
};

class VideoWriter
{
public:
	bool open(const std::string &path, int w, int h, double fps, const ColorInfo &color)
	{
		// tag the file like the source, so players and converters decode both the same way
		const char *matrix = color.matrix == ColorMatrix::BT709 ? "bt709" : "smpte170m";
		const char *range = color.full_range ? "pc" : "tv";
		char command[1024];
		snprintf(command, sizeof(command),
			"ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt nv12 -s %dx%d -r %.6f "
			"-colorspace %s -color_range %s -i - "
			"-c:v libx264 -preset veryfast -crf 14 -pix_fmt yuv420p -colorspace %s -color_range %s \"%s\"",
			w, h, fps, matrix, range, matrix, range, path.c_str());
#ifdef _WIN32
		pipe = _popen(command, "wb");
#else
		pipe = popen(command, "w");
#endif
		return pipe != nullptr;
	}
	void write(const Picture &picture)
	{
		if(!pipe)
			return;
		fwrite(picture.y.data(), 1, picture.y.size(), pipe);
		fwrite(picture.uv.data(), 1, picture.uv.size(), pipe);
	}
	void close()
	{
		if(!pipe)
			return;
#ifdef _WIN32
		_pclose(pipe);
#else
		pclose(pipe);
#endif
		pipe = nullptr;
	}
	~VideoWriter() { close(); }

private:
	FILE *pipe = nullptr;
};

bool initInterpolator(RifeInterpolator &rife, const std::map<std::string, std::string> &args)
{
	RifeInterpolator::Config config;
	auto model = args.find("model");
	if(model == args.end())
	{
		fprintf(stderr, "--model <dir> is required\n");
		return false;
	}
	config.model_dir = model->second;
	auto gpu = args.find("gpu");
	if(gpu != args.end())
		config.gpu_index = atoi(gpu->second.c_str());
	auto divisor = args.find("flowdiv");
	if(divisor != args.end())
		config.flow_divisor = atoi(divisor->second.c_str());
	if(args.count("nostatic"))
		config.protect_static = false;
	if(args.count("bilinear"))
		config.sharp_sampling = false;
	std::string error;
	const auto start = std::chrono::steady_clock::now();
	if(!rife.init(config, &error))
	{
		fprintf(stderr, "RIFE init failed: %s\n", error.c_str());
		return false;
	}
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	printf("model   %s\n", config.model_dir.c_str());
	printf("flowdiv %d (flow network runs at 1/%d of the picture size per axis)\n", config.flow_divisor, config.flow_divisor);
	printf("gpu     %s\n", rife.gpuName().c_str());
	printf("init    %.0f ms\n", ms);
	return true;
}

int runGpus()
{
	for(const GpuDevice &device : RifeInterpolator::enumerateGpus())
		printf("%d  %s%s\n", device.index, device.name.c_str(), device.discrete ? "  (discrete)" : "");
	return 0;
}

int runBench(const Clip &clip, const std::map<std::string, std::string> &args)
{
	RifeInterpolator rife;
	if(!initInterpolator(rife, args))
		return 1;

	Picture out;
	out.allocate(clip.frames[0].width, clip.frames[0].height);
	Stats stats;
	std::string error;
	const int warmup = 5;
	for(size_t i = 0; i < clip.frames.size(); i++)
	{
		const auto start = std::chrono::steady_clock::now();
		if(!rife.push(clip.frames[i].view(), clip.color, &error))
		{
			fprintf(stderr, "push failed: %s\n", error.c_str());
			return 1;
		}
		if(!rife.ready())
			continue;
		if(!rife.interpolate(0.5f, out.mut(), nullptr, &error))
		{
			fprintf(stderr, "interpolate failed: %s\n", error.c_str());
			return 1;
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		if((int)i > warmup)
			stats.add(ms);
	}
	printf("size    %dx%d\n", out.width, out.height);
	printf("frames  %zu generated (after %d warm-up)\n", stats.values.size(), warmup);
	printf("ms per generated frame (push + interpolate, incl. upload and download)\n");
	printf("  min %.2f | median %.2f | mean %.2f | p95 %.2f | p99 %.2f | max %.2f\n",
		stats.percentile(0.0), stats.percentile(0.5), stats.mean(), stats.percentile(0.95), stats.percentile(0.99), stats.percentile(1.0));
	return 0;
}

int runQuality(const Clip &clip, const std::map<std::string, std::string> &args)
{
	RifeInterpolator rife;
	if(!initInterpolator(rife, args))
		return 1;

	VideoWriter writer;
	const bool write_output = args.count("out") != 0;
	const int w = clip.frames[0].width;
	const int h = clip.frames[0].height;
	if(write_output && !writer.open(args.at("out"), w, h, clip.fps > 0 ? clip.fps : 60.0, clip.color))
		fprintf(stderr, "could not start ffmpeg for %s\n", args.at("out").c_str());

	Picture generated;
	generated.allocate(w, h);
	Picture blended;
	Stats rife_psnr, blend_psnr, repeat_psnr;
	Stats rife_roi_psnr, blend_roi_psnr, static_psnr, static_worst;
	Stats detail_kept, blend_detail_kept;
	const std::vector<Region> regions = args.count("roi") ? parseRegions(args.at("roi")) : std::vector<Region>();
	std::string error;
	// --step N: every N-th frame is a source, the N - 1 frames in between are regenerated
	// and compared with the real ones (2 = one frame in between, 4 = three, as for 30 -> 120 fps).
	// --reuse 1: one motion estimate per pair (for the middle), as the live path does.
	const size_t step = args.count("step") ? (size_t)std::max(2, atoi(args.at("step").c_str())) : 2;
	const float field_time = args.count("reuse") ? 0.5f : -1.f;
	for(size_t i = 0; i + step < clip.frames.size(); i += step)
	{
		const Picture &a = clip.frames[i];
		const Picture &b = clip.frames[i + step];
		if(i == 0 && !rife.push(a.view(), clip.color, &error))
		{
			fprintf(stderr, "push failed: %s\n", error.c_str());
			return 1;
		}
		if(!rife.push(b.view(), clip.color, &error))
		{
			fprintf(stderr, "push failed: %s\n", error.c_str());
			return 1;
		}
		if(write_output)
			writer.write(a);
		for(size_t k = 1; k < step; k++)
		{
			const float t = (float)k / (float)step;
			const Picture &truth = clip.frames[i + k];
			if(!rife.interpolate(t, generated.mut(), nullptr, &error, field_time))
			{
				fprintf(stderr, "interpolation failed: %s\n", error.c_str());
				return 1;
			}
			blended.allocate(w, h);
			for(size_t n = 0; n < a.y.size(); n++)
				blended.y[n] = (uint8_t)((1.f - t) * a.y[n] + t * b.y[n] + 0.5f);
			rife_psnr.add(lumaPsnr(generated, truth));
			blend_psnr.add(lumaPsnr(blended, truth));
			const double truth_detail = fineDetail(truth);
			if(truth_detail > 0.0)
			{
				detail_kept.add(fineDetail(generated) / truth_detail);
				blend_detail_kept.add(fineDetail(blended) / truth_detail);
			}
			repeat_psnr.add(lumaPsnr(a, truth));
			if(!regions.empty())
			{
				rife_roi_psnr.add(lumaPsnrIn(generated, truth, regions));
				blend_roi_psnr.add(lumaPsnrIn(blended, truth, regions));
				int worst_error = 0;
				static_psnr.add(staticPsnrIn(generated, truth, a, b, regions, &worst_error));
				static_worst.add(worst_error);
			}
			if(write_output)
				writer.write(generated);
		}
	}
	writer.close();
	printf("pairs   %zu\n", rife_psnr.values.size());
	printf("luma PSNR against the real in-between frame (higher is better)\n");
	printf("  generated (RIFE)      mean %.2f dB | median %.2f | worst %.2f\n", rife_psnr.mean(), rife_psnr.percentile(0.5), rife_psnr.percentile(0.0));
	printf("  blend of neighbours   mean %.2f dB | median %.2f | worst %.2f\n", blend_psnr.mean(), blend_psnr.percentile(0.5), blend_psnr.percentile(0.0));
	printf("  repeat previous frame mean %.2f dB | median %.2f | worst %.2f\n", repeat_psnr.mean(), repeat_psnr.percentile(0.5), repeat_psnr.percentile(0.0));
	printf("fine detail kept, relative to the real in-between frame (1.00 = as crisp as a real frame)\n");
	printf("  generated (RIFE)      mean %.3f | lowest %.3f\n", detail_kept.mean(), detail_kept.percentile(0.0));
	printf("  blend of neighbours   mean %.3f\n", blend_detail_kept.mean());
	if(!regions.empty())
	{
		printf("inside the %zu marked region(s) only\n", regions.size());
		printf("  generated (RIFE)      mean %.2f dB | median %.2f | worst %.2f\n", rife_roi_psnr.mean(), rife_roi_psnr.percentile(0.5), rife_roi_psnr.percentile(0.0));
		printf("  blend of neighbours   mean %.2f dB | median %.2f | worst %.2f\n", blend_roi_psnr.mean(), blend_roi_psnr.percentile(0.5), blend_roi_psnr.percentile(0.0));
		printf("static pixels in those regions (unchanged across the three real frames)\n");
		printf("  generated (RIFE)      mean %.2f dB | worst frame %.2f | largest single error: mean %.0f, max %.0f levels\n",
			static_psnr.mean(), static_psnr.percentile(0.0), static_worst.mean(), static_worst.percentile(1.0));
	}
	return 0;
}

int runDedup(const Clip &clip, const std::map<std::string, std::string> &args)
{
	DuplicateDetector detector;
	const bool verbose = args.count("verbose") != 0;
	int unique = 0;
	int repeats = 0;
	Stats unique_level, repeat_level;
	for(size_t i = 0; i < clip.frames.size(); i++)
	{
		const Nv12View view = clip.frames[i].view();
		const FrameDifference diff = detector.measure(view);
		const bool duplicate = detector.classify(diff);
		if(verbose)
			printf("%5zu  %s  mean diff %.3f  changed %.5f  motion level %.3f\n", i, duplicate ? "repeat" : "new   ",
				diff.mean_abs_diff, diff.changed_ratio, detector.motionLevel());
		if(duplicate)
		{
			repeats++;
			repeat_level.add(diff.mean_abs_diff);
		}
		else
		{
			unique++;
			if(diff.comparable)
				unique_level.add(diff.mean_abs_diff);
			detector.accept(view);
		}
	}
	const double duration = clip.fps > 0 ? clip.frames.size() / clip.fps : 0.0;
	printf("frames  %zu at %.3f fps\n", clip.frames.size(), clip.fps);
	printf("new     %d (%.1f%%)\n", unique, 100.0 * unique / clip.frames.size());
	printf("repeat  %d (%.1f%%)\n", repeats, 100.0 * repeats / clip.frames.size());
	if(duration > 0)
		printf("real frame rate  %.2f fps\n", unique / duration);
	printf("mean luma difference   new: min %.3f median %.3f | repeat: median %.3f max %.3f\n",
		unique_level.percentile(0.0), unique_level.percentile(0.5), repeat_level.percentile(0.5), repeat_level.percentile(1.0));
	return 0;
}

int runConvert(const Clip &clip, const std::map<std::string, std::string> &args)
{
	if(!args.count("out"))
	{
		fprintf(stderr, "--out <file> is required\n");
		return 1;
	}
	RifeInterpolator rife;
	if(!initInterpolator(rife, args))
		return 1;

	const int w = clip.frames[0].width;
	const int h = clip.frames[0].height;
	DuplicateDetector detector;
	int unique = 0;
	for(const Picture &picture : clip.frames)
	{
		const FrameDifference diff = detector.measure(picture.view());
		if(!detector.classify(diff))
		{
			unique++;
			detector.accept(picture.view());
		}
	}
	const double duration = clip.fps > 0 ? clip.frames.size() / clip.fps : clip.frames.size() / 60.0;
	const double unique_fps = unique / duration;
	const double out_fps = unique_fps * 2.0;
	printf("input   %zu frames, %d new (%.2f fps real) -> output %.2f fps\n", clip.frames.size(), unique, unique_fps, out_fps);

	VideoWriter writer;
	if(!writer.open(args.at("out"), w, h, out_fps, clip.color))
	{
		fprintf(stderr, "could not start ffmpeg\n");
		return 1;
	}
	detector.reset();
	Picture generated;
	generated.allocate(w, h);
	Stats stats;
	std::string error;
	for(const Picture &picture : clip.frames)
	{
		const FrameDifference diff = detector.measure(picture.view());
		if(detector.classify(diff))
			continue;
		detector.accept(picture.view());
		const auto start = std::chrono::steady_clock::now();
		if(!rife.push(picture.view(), clip.color, &error))
		{
			fprintf(stderr, "push failed: %s\n", error.c_str());
			return 1;
		}
		if(rife.ready())
		{
			if(!rife.interpolate(0.5f, generated.mut(), nullptr, &error))
			{
				fprintf(stderr, "interpolate failed: %s\n", error.c_str());
				return 1;
			}
			stats.add(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
			writer.write(generated);
		}
		writer.write(picture);
	}
	writer.close();
	printf("generated %zu frames, median %.2f ms, p95 %.2f ms\n", stats.values.size(), stats.percentile(0.5), stats.percentile(0.95));
	printf("wrote   %s\n", args.at("out").c_str());
	return 0;
}

void usage()
{
	fprintf(stderr,
		"usage:\n"
		"  robzfg-lab gpus\n"
		"  robzfg-lab bench   --input <video> --model <dir> [--frames N] [--gpu I] [--flowdiv 1|2|4]\n"
		"  robzfg-lab quality --input <60 fps video> --model <dir> [--frames N] [--out file.mp4]\n"
		"  robzfg-lab dedup   --input <video> [--frames N] [--verbose 1]\n"
		"  robzfg-lab convert --input <video> --model <dir> --out file.mp4 [--frames N]\n");
}

} // namespace

int main(int argc, char **argv)
{
	if(argc < 2)
	{
		usage();
		return 1;
	}
	const std::string command = argv[1];
	std::map<std::string, std::string> args;
	for(int i = 2; i + 1 < argc; i += 2)
	{
		std::string key = argv[i];
		if(key.rfind("--", 0) != 0)
		{
			usage();
			return 1;
		}
		args[key.substr(2)] = argv[i + 1];
	}

	if(command == "gpus")
		return runGpus();

	if(!args.count("input"))
	{
		usage();
		return 1;
	}
	const int max_frames = args.count("frames") ? std::max(3, atoi(args["frames"].c_str())) : 120;
	Clip clip;
	if(!loadClip(args["input"], max_frames, clip))
		return 1;
	printf("input   %s: %zu frames, %dx%d, %.3f fps, %s %s range\n", args["input"].c_str(), clip.frames.size(),
		clip.frames[0].width, clip.frames[0].height, clip.fps,
		clip.color.matrix == ColorMatrix::BT709 ? "BT.709" : "BT.601", clip.color.full_range ? "full" : "limited");

	if(command == "bench")
		return runBench(clip, args);
	if(command == "quality")
		return runQuality(clip, args);
	if(command == "dedup")
		return runDedup(clip, args);
	if(command == "convert")
		return runConvert(clip, args);
	usage();
	return 1;
}
