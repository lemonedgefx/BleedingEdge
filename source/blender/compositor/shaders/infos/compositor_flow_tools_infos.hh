/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_flow_to_speed)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(int2, output_size)
PUSH_CONSTANT(float, confidence_threshold)
SAMPLER(0, sampler2D, previous_tx)
SAMPLER(1, sampler2D, next_tx)
SAMPLER(2, sampler2D, previous_confidence_tx)
SAMPLER(3, sampler2D, next_confidence_tx)
COMPUTE_SOURCE("compositor_flow_to_speed.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_flow_to_speed_speed)
ADDITIONAL_INFO(compositor_flow_to_speed)
DEFINE("COMPUTE_SPEED")
IMAGE(0, SFLOAT_32_32_32_32, write, image2D, speed_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_flow_to_speed_confidence)
ADDITIONAL_INFO(compositor_flow_to_speed)
DEFINE("COMPUTE_CONFIDENCE")
IMAGE(1, SFLOAT_32, write, image2D, confidence_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_flow_to_speed_both)
ADDITIONAL_INFO(compositor_flow_to_speed_speed)
DEFINE("COMPUTE_CONFIDENCE")
IMAGE(1, SFLOAT_32, write, image2D, confidence_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_flow_visualize)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(int2, output_size)
PUSH_CONSTANT(float, parameter)
SAMPLER(0, sampler2D, flow_tx)
SAMPLER(1, sampler2D, confidence_tx)
IMAGE(0, SFLOAT_32_32_32_32, write, image2D, output_img)
COMPUTE_SOURCE("compositor_flow_visualize.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_flow_interpolate)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(int2, output_size)
PUSH_CONSTANT(float, parameter)
PUSH_CONSTANT(float, confidence_threshold)
PUSH_CONSTANT(int, fallback_mode)
SAMPLER(0, sampler2D, first_tx)
SAMPLER(1, sampler2D, second_tx)
SAMPLER(2, sampler2D, forward_tx)
SAMPLER(3, sampler2D, backward_tx)
SAMPLER(4, sampler2D, forward_confidence_tx)
SAMPLER(5, sampler2D, backward_confidence_tx)
COMPUTE_SOURCE("compositor_flow_interpolate.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_flow_interpolate_image)
ADDITIONAL_INFO(compositor_flow_interpolate)
DEFINE("COMPUTE_IMAGE")
IMAGE(0, SFLOAT_32_32_32_32, write, image2D, output_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_flow_interpolate_confidence)
ADDITIONAL_INFO(compositor_flow_interpolate)
DEFINE("COMPUTE_CONFIDENCE")
IMAGE(1, SFLOAT_32, write, image2D, confidence_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_flow_interpolate_both)
ADDITIONAL_INFO(compositor_flow_interpolate_image)
DEFINE("COMPUTE_CONFIDENCE")
IMAGE(1, SFLOAT_32, write, image2D, confidence_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
