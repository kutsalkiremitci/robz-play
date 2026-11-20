// SPDX-License-Identifier: MIT
// "rife.Warp" custom ncnn layer, taken from rife-ncnn-vulkan
// (Copyright (c) 2020 nihui, MIT License - see shaders/LICENSE.rife-ncnn-vulkan).

#ifndef ROBZFG_WARP_LAYER_H
#define ROBZFG_WARP_LAYER_H

#include <vector>

#include <layer.h>
#include <pipeline.h>

namespace robzfg
{

class Warp : public ncnn::Layer
{
public:
	Warp();
	int create_pipeline(const ncnn::Option &opt) override;
	int destroy_pipeline(const ncnn::Option &opt) override;
	int forward(const std::vector<ncnn::Mat> &bottom_blobs, std::vector<ncnn::Mat> &top_blobs, const ncnn::Option &opt) const override;
	int forward(const std::vector<ncnn::VkMat> &bottom_blobs, std::vector<ncnn::VkMat> &top_blobs, ncnn::VkCompute &cmd, const ncnn::Option &opt) const override;

private:
	ncnn::Pipeline *pipeline_warp;
	ncnn::Pipeline *pipeline_warp_pack4;
	ncnn::Pipeline *pipeline_warp_pack8;
};

ncnn::Layer *createWarpLayer(void *userdata);

} // namespace robzfg

#endif // ROBZFG_WARP_LAYER_H
