/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_optical_flow_unary)
LOCAL_GROUP_SIZE(16, 16)
SAMPLER(0, sampler2D, input_tx)
IMAGE(0, SFLOAT_32_32_32_32, write, image2D, output_img)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_optical_flow_prepare)
ADDITIONAL_INFO(compositor_optical_flow_unary)
PUSH_CONSTANT(float3, luminance_coefficients)
COMPUTE_SOURCE("compositor_optical_flow_prepare.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_optical_flow_downsample)
ADDITIONAL_INFO(compositor_optical_flow_unary)
COMPUTE_SOURCE("compositor_optical_flow_downsample.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_optical_flow_gradients)
ADDITIONAL_INFO(compositor_optical_flow_unary)
COMPUTE_SOURCE("compositor_optical_flow_gradients.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_optical_flow_upsample)
ADDITIONAL_INFO(compositor_optical_flow_unary)
COMPUTE_SOURCE("compositor_optical_flow_upsample.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_optical_flow_solver)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(int, window_radius)
PUSH_CONSTANT(float, max_motion)
SAMPLER(0, sampler2D, gradient_tx)
SAMPLER(1, sampler2D, reference_tx)
IMAGE(0, SFLOAT_32_32_32_32, write, image2D, output_img)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_optical_flow_initialize)
ADDITIONAL_INFO(compositor_optical_flow_solver)
COMPUTE_SOURCE("compositor_optical_flow_initialize.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_optical_flow_iterate)
ADDITIONAL_INFO(compositor_optical_flow_solver)
SAMPLER(2, sampler2D, flow_tx)
COMPUTE_SOURCE("compositor_optical_flow_iterate.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_optical_flow_output)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(int2, output_size)
PUSH_CONSTANT(bool, consistency_check)
SAMPLER(0, sampler2D, first_tx)
SAMPLER(1, sampler2D, second_tx)
SAMPLER(2, sampler2D, forward_tx)
SAMPLER(3, sampler2D, backward_tx)
SAMPLER(4, sampler2D, mask_tx)
COMPUTE_SOURCE("compositor_optical_flow_output.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_optical_flow_output_flow)
ADDITIONAL_INFO(compositor_optical_flow_output)
DEFINE("COMPUTE_FLOW")
IMAGE(0, SFLOAT_32_32, write, image2D, flow_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_optical_flow_output_confidence)
ADDITIONAL_INFO(compositor_optical_flow_output)
DEFINE("COMPUTE_CONFIDENCE")
IMAGE(1, SFLOAT_32, write, image2D, confidence_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_optical_flow_output_both)
ADDITIONAL_INFO(compositor_optical_flow_output_flow)
DEFINE("COMPUTE_CONFIDENCE")
IMAGE(1, SFLOAT_32, write, image2D, confidence_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
