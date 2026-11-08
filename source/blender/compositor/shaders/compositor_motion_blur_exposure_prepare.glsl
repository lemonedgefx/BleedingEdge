/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_motion_blur_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_motion_blur_exposure_prepare)

#include "gpu_shader_compositor_motion_blur_lib.glsl"
#include "gpu_shader_compositor_texture_utilities.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int2 size = texture_size(input_tx);
  if (any(greaterThanEqual(texel, size))) {
    return;
  }
  float4 color = texture_load(input_tx, texel);
  float4 mask = float4(float2(shutter_speed), float2(-shutter_speed));
  float4 motion = texture_load(velocity_tx, texel) * mask;
  float4 extended = use_extended_motion ? texture_load(velocity_extended_tx, texel) * mask :
                                          float4(0.0f);
  MotionPath previous = compute_motion_path(
      motion, extended, false, use_curved_interpolation, use_extended_motion, shutter_speed);
  MotionPath next = compute_motion_path(
      motion, extended, true, use_curved_interpolation, use_extended_motion, shutter_speed);
  bool moving = any(notEqual(color, float4(0.0f))) &&
                max(motion_path_reach(previous), motion_path_reach(next)) >= 0.5f;
  if (any(notEqual(color, float4(0.0f))) &&
      max(length(previous.curvature) + length(previous.cubic),
          length(next.curvature) + length(next.cubic)) > 1e-4f)
  {
    atomicOr(statistics_buf[2], 1u);
  }
  imageStore(seeds_img, texel, int4(!reconstruct_background || moving ? int2(-1) : texel, 0, 0));
  if (moving) {
    uint index = atomicAdd(statistics_buf[0], 1u);
    sources_buf[index] = uint(texel.y * size.x + texel.x);
    uint count = uint(
        max(exposure_samples(previous, samples_count), exposure_samples(next, samples_count)));
    atomicMax(statistics_buf[1], count);
  }
}
