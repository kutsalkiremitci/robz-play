// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Robz Play frame generation - RIFE v4 frame interpolation on the GPU (ncnn / Vulkan).

#ifndef ROBZFG_RIFE_INTERPOLATOR_H
#define ROBZFG_RIFE_INTERPOLATOR_H

#include <memory>
#include <string>
#include <vector>

#include "frame.h"

namespace robzfg
{

struct GpuDevice
{
	int index = -1;
	std::string name;
	bool discrete = false;
};

struct InterpolateTiming
{
	double total_ms = 0.0; // wall time of the whole call, including upload and download
	// the three parts of it: describing the work to the GPU, waiting for the GPU to finish
	// it (upload, motion estimate, picture and download are one submission), and copying
	// the finished picture out
	double prepare_ms = 0.0;
	double gpu_ms = 0.0;
	double copy_ms = 0.0;
};

// Synthesizes in-between frames from two consecutive source frames.
//
// Usage per new (unique) source frame:
//   push(frame)                -> the frame becomes "current", the old current becomes "previous"
//   interpolate(t, out)        -> writes the frame at time t in (0,1) between previous and current
//
// Frames stay on the GPU between calls, so each source frame is uploaded only once
// no matter how many in-between frames are generated from it.
//
// Not thread-safe: call every method from the same thread.
class RifeInterpolator
{
public:
	struct Config
	{
		// Directory holding flownet.param / flownet.bin of a RIFE v4.x model.
		std::string model_dir;
		// ncnn GPU index, or -1 to pick the first discrete GPU (falling back to ncnn's default).
		int gpu_index = -1;
		// Spatial padding multiple required by the model; 0 derives it from the directory name.
		int padding = 0;
		// Picture pixels per network pixel in each direction: 1, 2 or 4. The network only
		// estimates motion; the picture is always built at full resolution. 2 runs the
		// network on a quarter of the pixels, which is what keeps 1080p within a few ms.
		int flow_divisor = 2;
		// Keep picture elements that do not move (a game's HUD: text, map frame, icons)
		// exactly where they are instead of letting the motion of the scene behind drag them.
		bool protect_static = true;
		// Fetch source pixels with a bicubic kernel, which keeps generated frames as crisp
		// as the real ones; off falls back to bilinear filtering (softer, marginally cheaper).
		bool sharp_sampling = true;
	};

	RifeInterpolator();
	~RifeInterpolator();
	RifeInterpolator(const RifeInterpolator &) = delete;
	RifeInterpolator &operator=(const RifeInterpolator &) = delete;

	static std::vector<GpuDevice> enumerateGpus();

	bool init(const Config &config, std::string *error = nullptr);
	bool initialized() const;
	std::string gpuName() const;

	// Changes Config::flow_divisor (1, 2 or 4) while running, e.g. to a cheaper motion
	// estimate when the GPU does not keep up. Drops the history: the next two frames
	// pushed are the first pair again.
	bool setFlowDivisor(int flow_divisor);
	int flowDivisor() const;

	// Makes `frame` the current frame. Width must be a multiple of 4 and height a multiple of 2.
	// A change of size or colour description drops the history.
	bool push(const Nv12View &frame, const ColorInfo &color, std::string *error = nullptr);

	// True once two frames of the same format have been pushed.
	bool ready() const;

	// Writes the frame at `t` (0 < t < 1) between the previous and the current frame into `out`.
	//
	// `field_time` chooses what the motion estimate is made for. Negative (the default):
	// for `t` itself, the most accurate and the most expensive, since the network runs for
	// every frame. A value in (0,1), typically 0.5: the network runs once per pair of
	// source frames for that point in time, and every frame generated from the pair with
	// the same `field_time` is derived from that one estimate, at a fraction of the cost.
	bool interpolate(float t, const Nv12Mut &out, InterpolateTiming *timing = nullptr, std::string *error = nullptr,
		float field_time = -1.f);

	// Drops the history, e.g. after a scene cut or a stream restart.
	void reset();

private:
	struct Impl;
	std::unique_ptr<Impl> d;
};

} // namespace robzfg

#endif // ROBZFG_RIFE_INTERPOLATOR_H
