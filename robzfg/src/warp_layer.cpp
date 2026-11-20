// SPDX-License-Identifier: MIT
// "rife.Warp" custom ncnn layer, taken from rife-ncnn-vulkan
// (Copyright (c) 2020 nihui, MIT License - see shaders/LICENSE.rife-ncnn-vulkan).
// Changes for Robz Play: shader sources are embedded as text, no OpenMP, and no pack8
// pipeline (current ncnn no longer produces pack8 blobs on Vulkan).

#include "warp_layer.h"

#include <algorithm>
#include <cmath>

#include <command.h>
#include <gpu.h>

#include "warp.comp.h"
#include "warp_pack4.comp.h"

namespace robzfg
{

using namespace ncnn;

Warp::Warp()
{
	support_vulkan = true;

	pipeline_warp = 0;
	pipeline_warp_pack4 = 0;
	pipeline_warp_pack8 = 0;
}

static Pipeline *createWarpPipeline(const VulkanDevice *vkdev, const char *source, int source_size, const Option &opt)
{
	std::vector<uint32_t> spirv;
	if(compile_spirv_module(source, source_size, opt, spirv) != 0)
		return 0;
	std::vector<vk_specialization_type> specializations(0);
	Pipeline *pipeline = new Pipeline(vkdev);
	pipeline->set_optimal_local_size_xyz();
	pipeline->create(spirv.data(), spirv.size() * 4, specializations);
	return pipeline;
}

int Warp::create_pipeline(const Option &opt)
{
	if(!vkdev)
		return 0;

	pipeline_warp = createWarpPipeline(vkdev, warp_comp_data, (int)sizeof(warp_comp_data) - 1, opt);
	pipeline_warp_pack4 = createWarpPipeline(vkdev, warp_pack4_comp_data, (int)sizeof(warp_pack4_comp_data) - 1, opt);

	if(!pipeline_warp || !pipeline_warp_pack4)
		return -1;
	return 0;
}

int Warp::destroy_pipeline(const Option & /*opt*/)
{
	delete pipeline_warp;
	pipeline_warp = 0;

	delete pipeline_warp_pack4;
	pipeline_warp_pack4 = 0;

	delete pipeline_warp_pack8;
	pipeline_warp_pack8 = 0;

	return 0;
}

int Warp::forward(const std::vector<Mat> &bottom_blobs, std::vector<Mat> &top_blobs, const Option & /*opt*/) const
{
	const Mat &image_blob = bottom_blobs[0];
	const Mat &flow_blob = bottom_blobs[1];

	int w = image_blob.w;
	int h = image_blob.h;
	int channels = image_blob.c;

	Mat &top_blob = top_blobs[0];
	top_blob.create(w, h, channels);
	if(top_blob.empty())
		return -100;

	for(int q = 0; q < channels; q++)
	{
		float *outptr = top_blob.channel(q);

		const Mat image = image_blob.channel(q);

		const float *fxptr = flow_blob.channel(0);
		const float *fyptr = flow_blob.channel(1);

		for(int y = 0; y < h; y++)
		{
			for(int x = 0; x < w; x++)
			{
				float sample_x = x + fxptr[0];
				float sample_y = y + fyptr[0];

				int x0 = (int)std::floor(sample_x);
				int y0 = (int)std::floor(sample_y);
				int x1 = x0 + 1;
				int y1 = y0 + 1;

				x0 = std::min(std::max(x0, 0), w - 1);
				y0 = std::min(std::max(y0, 0), h - 1);
				x1 = std::min(std::max(x1, 0), w - 1);
				y1 = std::min(std::max(y1, 0), h - 1);

				float alpha = sample_x - x0;
				float beta = sample_y - y0;

				float v0 = image.row(y0)[x0];
				float v1 = image.row(y0)[x1];
				float v2 = image.row(y1)[x0];
				float v3 = image.row(y1)[x1];

				float v4 = v0 * (1 - alpha) + v1 * alpha;
				float v5 = v2 * (1 - alpha) + v3 * alpha;

				outptr[0] = v4 * (1 - beta) + v5 * beta;

				outptr += 1;
				fxptr += 1;
				fyptr += 1;
			}
		}
	}

	return 0;
}

int Warp::forward(const std::vector<VkMat> &bottom_blobs, std::vector<VkMat> &top_blobs, VkCompute &cmd, const Option &opt) const
{
	const VkMat &image_blob = bottom_blobs[0];
	const VkMat &flow_blob = bottom_blobs[1];

	int w = image_blob.w;
	int h = image_blob.h;
	int channels = image_blob.c;
	size_t elemsize = image_blob.elemsize;
	int elempack = image_blob.elempack;

	VkMat &top_blob = top_blobs[0];
	top_blob.create(w, h, channels, elemsize, elempack, opt.blob_vkallocator);
	if(top_blob.empty())
		return -100;

	std::vector<VkMat> bindings(3);
	bindings[0] = image_blob;
	bindings[1] = flow_blob;
	bindings[2] = top_blob;

	std::vector<vk_constant_type> constants(4);
	constants[0].i = top_blob.w;
	constants[1].i = top_blob.h;
	constants[2].i = top_blob.c;
	constants[3].i = top_blob.cstep;

	if(elempack == 8 && pipeline_warp_pack8)
		cmd.record_pipeline(pipeline_warp_pack8, bindings, constants, top_blob);
	else if(elempack == 4)
		cmd.record_pipeline(pipeline_warp_pack4, bindings, constants, top_blob);
	else
		cmd.record_pipeline(pipeline_warp, bindings, constants, top_blob);

	return 0;
}

ncnn::Layer *createWarpLayer(void * /*userdata*/)
{
	return new Warp;
}

} // namespace robzfg
