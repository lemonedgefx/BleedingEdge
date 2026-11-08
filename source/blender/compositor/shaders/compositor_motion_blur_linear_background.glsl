/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_motion_blur_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_motion_blur_linear_background)

#include "gpu_shader_compositor_texture_utilities.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(texel, texture_size(input_tx)))) {
    return;
  }
  float4 color = texture_load(input_tx, texel);
  float4 motion = texture_load(velocity_tx, texel) * shutter_speed;
  bool moving = any(notEqual(color, float4(0.0f))) &&
                max(length(motion.xy), length(motion.zw)) >= 1e-5f;
  imageStore(seeds_img, texel, int4(moving ? int2(-1) : texel, 0, 0));
  imageStore(foreground_img, texel, moving ? color : float4(0.0f));
  imageStore(coverage_img, texel, float4(moving ? 1.0f : 0.0f));
}
