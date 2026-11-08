/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_motion_blur_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_motion_blur)

#include "gpu_shader_compositor_texture_utilities.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int2 size = texture_size(input_tx);
  if (any(greaterThanEqual(texel, size))) {
    return;
  }
  uint index = uint(texel.y * size.x + texel.x) * 4u;
  float4 sum = float4(uintBitsToFloat(accumulation_buf[index]),
                      uintBitsToFloat(accumulation_buf[index + 1u]),
                      uintBitsToFloat(accumulation_buf[index + 2u]),
                      uintBitsToFloat(accumulation_buf[index + 3u]));
  float4 original = texture_load(input_tx, texel);
  int2 seed = texture_load(background_tx, texel).xy;
  bool has_background = all(greaterThanEqual(seed, int2(0)));
  if (!has_background && original.w >= 1.0f && sum.w > 1e-8f) {
    imageStore(output_img, texel, float4(sum.xyz / sum.w, 1.0f));
    return;
  }
  float4 background = use_current_background ? original : float4(0.0f);
  if (has_background) {
    background = texture_load(input_tx, seed);
  }
  float opacity = clamp(sum.w, 0.0f, 1.0f);
  float4 foreground = sum / max(sum.w, 1.0f);
  imageStore(output_img, texel, foreground + background * (1.0f - opacity));
}
