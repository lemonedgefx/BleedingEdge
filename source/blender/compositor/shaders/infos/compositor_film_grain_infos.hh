/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_film_grain)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(float, amount)
PUSH_CONSTANT(float3, grain_size)
PUSH_CONSTANT(float, color_amount)
PUSH_CONSTANT(float3, response)
PUSH_CONSTANT(float, pivot)
PUSH_CONSTANT(int, seed)
PUSH_CONSTANT(float3, luminance_coefficients)
PUSH_CONSTANT(int2, data_offset)
SAMPLER(0, sampler2D, input_tx)
SAMPLER(1, sampler2D, mask_tx)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, output_img)
COMPUTE_SOURCE("compositor_film_grain.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
