/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_ibk_shared)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(int, screen_channel)
PUSH_CONSTANT(float, screen_balance)
PUSH_CONSTANT(float, plate_exposure)
SAMPLER(0, sampler2D, input_tx)
SAMPLER(1, sampler2D, plate_tx)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_ibk_compute_matte)
ADDITIONAL_INFO(compositor_ibk_shared)
IMAGE(0, SFLOAT_16, write, image2D, output_img)
COMPUTE_SOURCE("compositor_ibk_compute_matte.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_ibk_output_shared)
ADDITIONAL_INFO(compositor_ibk_shared)
PUSH_CONSTANT(float, despill_strength)
SAMPLER(2, sampler2D, matte_tx)
IMAGE(0, SFLOAT_16, write, image2D, output_matte_img)
COMPUTE_SOURCE("compositor_ibk_compute_image.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_ibk_output_matte)
ADDITIONAL_INFO(compositor_ibk_output_shared)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_ibk_compute_image)
ADDITIONAL_INFO(compositor_ibk_output_shared)
DEFINE("COMPUTE_IMAGE")
IMAGE(1, SFLOAT_16_16_16_16, write, image2D, output_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
