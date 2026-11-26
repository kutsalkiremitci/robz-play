// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Robz Play frame generation - plain frame views shared by every component.

#ifndef ROBZFG_FRAME_H
#define ROBZFG_FRAME_H

#include <cstdint>

namespace robzfg
{

// YCbCr -> RGB matrix of the video signal.
enum class ColorMatrix
{
	BT709,
	BT601,
	BT2020, // non-constant luminance, what HDR streams use
};

struct ColorInfo
{
	ColorMatrix matrix = ColorMatrix::BT709;
	bool full_range = false;
	// The signal is HDR with the PQ curve (SMPTE ST 2084). Only how motion is estimated
	// depends on this; the picture itself is built from the samples as they are.
	bool pq = false;

	bool operator==(const ColorInfo &o) const { return matrix == o.matrix && full_range == o.full_range && pq == o.pq; }
	bool operator!=(const ColorInfo &o) const { return !(*this == o); }
};

// 4:2:0 picture in NV12 layout: a luma plane followed by an interleaved Cb/Cr plane at
// half resolution. Strides are in bytes and may exceed the width.
//
// `sample_bytes` is 1 for 8-bit pictures (NV12 proper) and 2 for the 10-bit ones HDR streams
// carry (P010: each sample a little-endian 16-bit word with the value in its upper bits).
struct Nv12View
{
	int width = 0;
	int height = 0;
	const uint8_t *y = nullptr;
	int y_stride = 0;
	const uint8_t *uv = nullptr;
	int uv_stride = 0;
	int sample_bytes = 1;

	bool valid() const
	{
		return width > 0 && height > 0 && y && uv && (sample_bytes == 1 || sample_bytes == 2)
			&& y_stride >= width * sample_bytes && uv_stride >= width * sample_bytes;
	}
};

struct Nv12Mut
{
	int width = 0;
	int height = 0;
	uint8_t *y = nullptr;
	int y_stride = 0;
	uint8_t *uv = nullptr;
	int uv_stride = 0;
	int sample_bytes = 1;

	bool valid() const
	{
		return width > 0 && height > 0 && y && uv && (sample_bytes == 1 || sample_bytes == 2)
			&& y_stride >= width * sample_bytes && uv_stride >= width * sample_bytes;
	}
};

} // namespace robzfg

#endif // ROBZFG_FRAME_H
