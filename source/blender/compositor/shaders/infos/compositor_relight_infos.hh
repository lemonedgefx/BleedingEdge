/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_relight_shared)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(int2, output_size)
PUSH_CONSTANT(int, light_type)
PUSH_CONSTANT(float3, light_color)
PUSH_CONSTANT(float, strength)
PUSH_CONSTANT(float3, light_direction)
PUSH_CONSTANT(float3, light_position)
PUSH_CONSTANT(bool, distance_falloff)
PUSH_CONSTANT(float, minimum_distance)
SAMPLER(0, sampler2D, input_tx)
SAMPLER(1, sampler2D, normal_tx)
SAMPLER(2, sampler2D, position_tx)
SAMPLER(3, sampler2D, diffuse_tx)
SAMPLER(4, sampler2D, mask_tx)
COMPUTE_SOURCE("compositor_relight.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_relight_image)
ADDITIONAL_INFO(compositor_relight_shared)
DEFINE("COMPUTE_IMAGE")
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, output_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_relight_lighting)
ADDITIONAL_INFO(compositor_relight_shared)
DEFINE("COMPUTE_LIGHTING")
IMAGE(1, SFLOAT_16_16_16_16, write, image2D, lighting_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_relight_both)
ADDITIONAL_INFO(compositor_relight_image)
DEFINE("COMPUTE_LIGHTING")
IMAGE(1, SFLOAT_16_16_16_16, write, image2D, lighting_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
