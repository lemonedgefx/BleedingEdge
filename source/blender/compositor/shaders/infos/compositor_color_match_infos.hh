/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_color_match_statistics)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(bool, luminance_only)
PUSH_CONSTANT(bool, squared_difference)
PUSH_CONSTANT(float3, luminance_coefficients)
PUSH_CONSTANT(float3, mean)
SAMPLER(0, sampler2D, input_tx)
SAMPLER(1, sampler2D, mask_tx)
IMAGE(0, SFLOAT_32_32_32_32, write, image2D, output_img)
COMPUTE_SOURCE("compositor_color_match_statistics.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_color_match_apply)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(float3, gain)
PUSH_CONSTANT(float3, offset)
SAMPLER(0, sampler2D, input_tx)
SAMPLER(1, sampler2D, factor_tx)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, output_img)
COMPUTE_SOURCE("compositor_color_match_apply.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
