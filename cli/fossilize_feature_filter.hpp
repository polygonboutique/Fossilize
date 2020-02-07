/* Copyright (c) 2019 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#ifndef VULKAN_CORE_H_
#error "Must include Vulkan headers before including fossilize_feature_filter.hpp"
#endif

namespace Fossilize
{
struct VulkanFeatures
{
	VkPhysicalDevice16BitStorageFeatures storage_16bit;
	VkPhysicalDeviceMultiviewFeatures multiview;
	VkPhysicalDeviceVariablePointersFeatures variable_pointers;
	VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr_conversion;
	VkPhysicalDeviceShaderDrawParametersFeatures draw_parameters;
	VkPhysicalDevice8BitStorageFeatures storage_8bit;
	VkPhysicalDeviceShaderAtomicInt64Features atomic_int64;
	VkPhysicalDeviceShaderFloat16Int8Features float16_int8;
	VkPhysicalDeviceDescriptorIndexingFeatures descriptor_indexing;
	VkPhysicalDeviceVulkanMemoryModelFeatures memory_model;
	VkPhysicalDeviceUniformBufferStandardLayoutFeatures ubo_standard_layout;
	VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures subgroup_extended_types;
	VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures separate_ds_layout;
	VkPhysicalDeviceBufferDeviceAddressFeatures buffer_device_address;
	VkPhysicalDeviceTransformFeedbackFeaturesEXT transform_feedback;
	VkPhysicalDeviceDepthClipEnableFeaturesEXT depth_clip;
	VkPhysicalDeviceInlineUniformBlockFeaturesEXT inline_uniform_block;
	VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT blend_operation_advanced;
	VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT attribute_divisor;
	VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT demote_to_helper;
	VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT shader_interlock;
};

void *build_pnext_chain(VulkanFeatures &features);

struct VulkanProperties
{
	VkPhysicalDeviceDescriptorIndexingProperties descriptor_indexing;
};

void *build_pnext_chain(VulkanProperties &properties);

class FeatureFilter
{
public:
	FeatureFilter();
	~FeatureFilter();
	void operator=(const FeatureFilter &) = delete;
	FeatureFilter(const FeatureFilter &) = delete;

	bool init(uint32_t api_version, const char **device_exts, unsigned count,
	          const VkPhysicalDeviceFeatures2 *enabled_features,
	          const VkPhysicalDeviceProperties2 *props);

	bool sampler_is_supported(const VkSamplerCreateInfo *info) const;
	bool descriptor_set_layout_is_supported(const VkDescriptorSetLayoutCreateInfo *info) const;
	bool pipeline_layout_is_supported(const VkPipelineLayoutCreateInfo *info) const;
	bool shader_module_is_supported(const VkShaderModuleCreateInfo *info) const;
	bool render_pass_is_supported(const VkRenderPassCreateInfo *info) const;
	bool graphics_pipeline_is_supported(const VkGraphicsPipelineCreateInfo *info) const;
	bool compute_pipeline_is_supported(const VkComputePipelineCreateInfo *info) const;

private:
	struct Impl;
	Impl *impl;
};
}
