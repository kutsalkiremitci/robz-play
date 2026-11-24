// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Robz Play frame generation - Apple's own frame interpolation (macOS 26+, VideoToolbox
// VTLowLatencyFrameInterpolation). Same calling pattern as RifeInterpolator, so the
// frame generation stage can use either one. Only built on macOS.

#ifndef ROBZFG_APPLE_INTERPOLATOR_H
#define ROBZFG_APPLE_INTERPOLATOR_H

#include <memory>
#include <string>

#include "frame.h"
#include "rife_interpolator.h" // InterpolateTiming

namespace robzfg
{

class AppleInterpolator
{
public:
	struct Config
	{
		// Frames generated between two real ones: 1 for 60 fps, 3 for 120 fps.
		int frames_between = 1;
	};

	AppleInterpolator();
	~AppleInterpolator();
	AppleInterpolator(const AppleInterpolator &) = delete;
	AppleInterpolator &operator=(const AppleInterpolator &) = delete;

	// True when this Mac and macOS version offer the feature.
	static bool supported();

	bool init(const Config &config, std::string *error = nullptr);
	std::string gpuName() const;

	// Makes `frame` the current frame (the old current becomes the previous one).
	bool push(const Nv12View &frame, const ColorInfo &color, std::string *error = nullptr);
	bool ready() const;
	// Writes the frame at `t` (0 < t < 1) between previous and current. All frames of one
	// pair are made in a single call to Apple's processor and handed out from a cache.
	bool interpolate(float t, const Nv12Mut &out, InterpolateTiming *timing = nullptr, std::string *error = nullptr,
		float field_time = -1.f);
	void reset();

private:
	struct Impl;
	std::unique_ptr<Impl> d;
};

} // namespace robzfg

#endif // ROBZFG_APPLE_INTERPOLATOR_H
