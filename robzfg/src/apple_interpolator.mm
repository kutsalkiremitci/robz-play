// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Robz Play frame generation - Apple's own frame interpolation on macOS 26+.
// Uses VideoToolbox's low-latency frame interpolation, which runs on the GPU / Neural
// Engine of Apple silicon and is meant for live content such as games and calls.

#include "robzfg/apple_interpolator.h"

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace robzfg
{

namespace
{
void setError(std::string *error, const std::string &text)
{
	if(error)
		*error = text;
}

// Copies an NV12 picture into a pixel buffer.
void copyIn(const Nv12View &src, CVPixelBufferRef dst)
{
	CVPixelBufferLockBaseAddress(dst, 0);
	uint8_t *y = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(dst, 0);
	uint8_t *uv = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(dst, 1);
	const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(dst, 0);
	const size_t uvs = CVPixelBufferGetBytesPerRowOfPlane(dst, 1);
	for(int row = 0; row < src.height; row++)
		memcpy(y + row * ys, src.y + (size_t)row * src.y_stride, src.width);
	for(int row = 0; row < src.height / 2; row++)
		memcpy(uv + row * uvs, src.uv + (size_t)row * src.uv_stride, src.width);
	CVPixelBufferUnlockBaseAddress(dst, 0);
}

void copyOut(CVPixelBufferRef src, const Nv12Mut &dst)
{
	CVPixelBufferLockBaseAddress(src, kCVPixelBufferLock_ReadOnly);
	const uint8_t *y = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(src, 0);
	const uint8_t *uv = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(src, 1);
	const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(src, 0);
	const size_t uvs = CVPixelBufferGetBytesPerRowOfPlane(src, 1);
	for(int row = 0; row < dst.height; row++)
		memcpy(dst.y + (size_t)row * dst.y_stride, y + row * ys, dst.width);
	for(int row = 0; row < dst.height / 2; row++)
		memcpy(dst.uv + (size_t)row * dst.uv_stride, uv + row * uvs, dst.width);
	CVPixelBufferUnlockBaseAddress(src, kCVPixelBufferLock_ReadOnly);
}

void tagColor(CVPixelBufferRef pb, const ColorInfo &color)
{
	const bool bt601 = color.matrix == ColorMatrix::BT601;
	CVBufferSetAttachment(pb, kCVImageBufferYCbCrMatrixKey,
		bt601 ? kCVImageBufferYCbCrMatrix_ITU_R_601_4 : kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
	CVBufferSetAttachment(pb, kCVImageBufferColorPrimariesKey,
		bt601 ? kCVImageBufferColorPrimaries_SMPTE_C : kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
	CVBufferSetAttachment(pb, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2,
		kCVAttachmentMode_ShouldPropagate);
}
} // namespace

struct AppleInterpolator::Impl
{
	int frames_between = 1;
	int w = 0;
	int h = 0;
	VTFrameProcessor *processor = nil;
	VTLowLatencyFrameInterpolationConfiguration *configuration = nil;
	CVPixelBufferPoolRef source_pool = nullptr;
	CVPixelBufferPoolRef destination_pool = nullptr;
	CVPixelBufferRef prev = nullptr;
	CVPixelBufferRef cur = nullptr;
	int64_t counter = 0;
	ColorInfo color;
	bool color_set = false;
	// generated frames of the current pair, made on the first interpolate() call
	std::vector<CVPixelBufferRef> made;
	std::vector<float> phases;
	bool made_valid = false;

	void dropMade()
	{
		for(CVPixelBufferRef pb : made)
			CVPixelBufferRelease(pb);
		made.clear();
		made_valid = false;
	}
	void releaseFrames()
	{
		if(prev)
			CVPixelBufferRelease(prev);
		if(cur)
			CVPixelBufferRelease(cur);
		prev = cur = nullptr;
		dropMade();
	}
	void closeSession()
	{
		releaseFrames();
		if(processor)
			[processor endSession];
		processor = nil;
		configuration = nil;
		if(source_pool)
			CVPixelBufferPoolRelease(source_pool);
		if(destination_pool)
			CVPixelBufferPoolRelease(destination_pool);
		source_pool = destination_pool = nullptr;
		w = h = 0;
	}

	static CVPixelBufferPoolRef makePool(NSDictionary *attributes, int w, int h)
	{
		NSMutableDictionary *a = [attributes mutableCopy];
		a[(id)kCVPixelBufferWidthKey] = @(w);
		a[(id)kCVPixelBufferHeightKey] = @(h);
		a[(id)kCVPixelBufferPixelFormatTypeKey] = @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
		if(!a[(id)kCVPixelBufferIOSurfacePropertiesKey])
			a[(id)kCVPixelBufferIOSurfacePropertiesKey] = @{};
		CVPixelBufferPoolRef pool = nullptr;
		CVPixelBufferPoolCreate(nullptr, nullptr, (__bridge CFDictionaryRef)a, &pool);
		return pool;
	}

	bool openSession(int width, int height, std::string *error)
	{
		closeSession();
		@autoreleasepool
		{
			configuration = [[VTLowLatencyFrameInterpolationConfiguration alloc] initWithFrameWidth:width
				frameHeight:height numberOfInterpolatedFrames:frames_between];
			if(!configuration)
			{
				setError(error, "Apple frame interpolation does not accept this picture size");
				return false;
			}
			processor = [[VTFrameProcessor alloc] init];
			NSError *ns_error = nil;
			if(![processor startSessionWithConfiguration:configuration error:&ns_error])
			{
				setError(error, std::string("Apple frame interpolation could not start: ")
					+ (ns_error ? ns_error.localizedDescription.UTF8String : "unknown error"));
				processor = nil;
				configuration = nil;
				return false;
			}
			source_pool = makePool(configuration.sourcePixelBufferAttributes, width, height);
			destination_pool = makePool(configuration.destinationPixelBufferAttributes, width, height);
			if(!source_pool || !destination_pool)
			{
				setError(error, "could not allocate picture buffers");
				closeSession();
				return false;
			}
		}
		w = width;
		h = height;
		phases.clear();
		for(int k = 1; k <= frames_between; k++)
			phases.push_back((float)k / (float)(frames_between + 1));
		return true;
	}

	bool makePair(std::string *error)
	{
		dropMade();
		@autoreleasepool
		{
			// Apple's processor wants increasing timestamps; the real frames sit one unit apart
			const int32_t scale = 1000;
			VTFrameProcessorFrame *previous = [[VTFrameProcessorFrame alloc] initWithBuffer:prev
				presentationTimeStamp:CMTimeMake((counter - 1) * scale, scale)];
			VTFrameProcessorFrame *source = [[VTFrameProcessorFrame alloc] initWithBuffer:cur
				presentationTimeStamp:CMTimeMake(counter * scale, scale)];
			if(!previous || !source)
			{
				setError(error, "could not wrap the source pictures");
				return false;
			}
			NSMutableArray<VTFrameProcessorFrame *> *destinations = [NSMutableArray array];
			NSMutableArray<NSNumber *> *phase_list = [NSMutableArray array];
			for(float p : phases)
			{
				CVPixelBufferRef out = nullptr;
				if(CVPixelBufferPoolCreatePixelBuffer(nullptr, destination_pool, &out) != kCVReturnSuccess || !out)
				{
					setError(error, "could not allocate an output picture");
					return false;
				}
				made.push_back(out);
				VTFrameProcessorFrame *f = [[VTFrameProcessorFrame alloc] initWithBuffer:out
					presentationTimeStamp:CMTimeMake((int64_t)std::llround((counter - 1 + p) * scale), scale)];
				if(!f)
				{
					setError(error, "could not wrap an output picture");
					return false;
				}
				[destinations addObject:f];
				[phase_list addObject:@(p)];
			}
			VTLowLatencyFrameInterpolationParameters *parameters = [[VTLowLatencyFrameInterpolationParameters alloc]
				initWithSourceFrame:source previousFrame:previous interpolationPhase:phase_list destinationFrames:destinations];
			if(!parameters)
			{
				setError(error, "invalid interpolation parameters");
				return false;
			}
			NSError *ns_error = nil;
			if(![processor processWithParameters:parameters error:&ns_error])
			{
				setError(error, std::string("Apple frame interpolation failed: ")
					+ (ns_error ? ns_error.localizedDescription.UTF8String : "unknown error"));
				return false;
			}
		}
		made_valid = true;
		return true;
	}
};

AppleInterpolator::AppleInterpolator() : d(new Impl) {}

AppleInterpolator::~AppleInterpolator()
{
	d->closeSession();
}

bool AppleInterpolator::supported()
{
	if(@available(macOS 26.0, *))
		return VTLowLatencyFrameInterpolationConfiguration.isSupported;
	return false;
}

bool AppleInterpolator::init(const Config &config, std::string *error)
{
	if(!supported())
	{
		setError(error, "Apple frame interpolation needs macOS 26 or newer on Apple silicon");
		return false;
	}
	d->frames_between = config.frames_between >= 3 ? 3 : 1;
	return true;
}

std::string AppleInterpolator::gpuName() const
{
	return "Apple (VideoToolbox)";
}

bool AppleInterpolator::push(const Nv12View &frame, const ColorInfo &color, std::string *error)
{
	if(!frame.valid())
	{
		setError(error, "invalid frame");
		return false;
	}
	if(frame.width != d->w || frame.height != d->h || !d->color_set || color != d->color)
	{
		d->color = color;
		d->color_set = true;
		if(!d->openSession(frame.width, frame.height, error))
			return false;
	}
	CVPixelBufferRef pb = nullptr;
	if(CVPixelBufferPoolCreatePixelBuffer(nullptr, d->source_pool, &pb) != kCVReturnSuccess || !pb)
	{
		setError(error, "could not allocate a source picture");
		return false;
	}
	copyIn(frame, pb);
	tagColor(pb, color);
	if(d->prev)
		CVPixelBufferRelease(d->prev);
	d->prev = d->cur;
	d->cur = pb;
	d->counter++;
	d->dropMade();
	return true;
}

bool AppleInterpolator::ready() const
{
	return d->prev && d->cur;
}

bool AppleInterpolator::interpolate(float t, const Nv12Mut &out, InterpolateTiming *timing, std::string *error, float)
{
	const auto start = std::chrono::steady_clock::now();
	if(!ready())
	{
		setError(error, "two frames are needed before interpolating");
		return false;
	}
	if(!out.valid() || out.width != d->w || out.height != d->h)
	{
		setError(error, "output frame does not match the stream size");
		return false;
	}
	if(!d->made_valid && !d->makePair(error))
	{
		d->dropMade();
		return false;
	}
	// the generated frame closest to the wanted time
	size_t best = 0;
	for(size_t i = 1; i < d->phases.size(); i++)
		if(std::abs(d->phases[i] - t) < std::abs(d->phases[best] - t))
			best = i;
	copyOut(d->made[best], out);
	if(timing)
		timing->total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	return true;
}

void AppleInterpolator::reset()
{
	d->releaseFrames();
}

} // namespace robzfg
