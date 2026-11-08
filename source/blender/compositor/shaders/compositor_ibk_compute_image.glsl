/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_ibk_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_ibk_compute_image)

#include "gpu_shader_compositor_ibk.glsl"
#include "gpu_shader_compositor_texture_utilities.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(texel, imageSize(output_matte_img)))) {
    return;
  }
  float4 color = texture_load(input_tx, texel);
  float4 plate = texture_load(plate_tx, texel);
  float matte = texture_load(matte_tx, texel).x;
  float4 result = ibk_image(
      color, plate, matte, screen_channel, screen_balance, plate_exposure, despill_strength);
  imageStore(output_matte_img, texel, float4(result.a));
#if defined(COMPUTE_IMAGE)
  imageStore(output_img, texel, result);
#endif
}
