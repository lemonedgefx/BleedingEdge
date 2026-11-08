/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_motion_blur_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_motion_blur_exposure_accumulate)

#include "gpu_shader_compositor_motion_blur_lib.glsl"
#include "gpu_shader_compositor_motion_blur_shutter_lib.glsl"
#include "gpu_shader_compositor_texture_utilities.glsl"

/* Portable floating-point addition on a uint buffer; no vendor float-atomic extension
 * is required. Accumulation is read only after a shader-storage barrier. */
void exposure_atomic_add(uint index, float value)
{
  if (value == 0.0f) {
    return;
  }
  uint previous = atomicAdd(accumulation_buf[index], 0u);
  while (true) {
    uint next = floatBitsToUint(uintBitsToFloat(previous) + value);
    uint actual = atomicCompSwap(accumulation_buf[index], previous, next);
    if (actual == previous) {
      break;
    }
    previous = actual;
  }
}

void exposure_splat(float2 position, float4 color, float source_depth)
{
  int2 size = texture_size(input_tx);
  int2 lower = int2(floor(position));
  float2 fraction = position - float2(lower);
  for (int j = 0; j < 2; j++) {
    for (int i = 0; i < 2; i++) {
      int2 target = lower + int2(i, j);
      if (any(lessThan(target, int2(0))) || any(greaterThanEqual(target, size))) {
        continue;
      }
      if (source_depth > 0.0f && all(equal(texture_load(seeds_tx, target).xy, target)) &&
          texture_load(input_tx, target).w >= 1.0f)
      {
        float target_depth = texture_load(depth_tx, target).x;
        if (target_depth > 0.0f && source_depth > target_depth + 0.01f) {
          continue;
        }
      }
      float coverage = (i != 0 ? fraction.x : 1.0f - fraction.x) *
                       (j != 0 ? fraction.y : 1.0f - fraction.y);
      float4 contribution = color * coverage;
      uint index = uint(target.y * size.x + target.x) * 4u;
      exposure_atomic_add(index, contribution.x);
      exposure_atomic_add(index + 1u, contribution.y);
      exposure_atomic_add(index + 2u, contribution.z);
      exposure_atomic_add(index + 3u, contribution.w);
    }
  }
}

void main()
{
  uint source_index = gl_GlobalInvocationID.x +
                      gl_GlobalInvocationID.y * gl_NumWorkGroups.x * gl_WorkGroupSize.x;
  if (source_index >= uint(sources_count)) {
    return;
  }
  int2 size = texture_size(input_tx);
  uint source = sources_buf[source_index];
  int2 texel = int2(int(source) % size.x, int(source) / size.x);
  float4 color = texture_load(input_tx, texel);
  float source_depth = texture_load(depth_tx, texel).x;
  float4 mask = float4(float2(shutter_speed), float2(-shutter_speed));
  float4 motion = texture_load(velocity_tx, texel) * mask;
  float4 extended = use_extended_motion ? texture_load(velocity_extended_tx, texel) * mask :
                                          float4(0.0f);
  for (int side = 0; side < 2; side++) {
    MotionPath path = compute_motion_path(
        motion, extended, side != 0, use_curved_interpolation, use_extended_motion, shutter_speed);
    int count = exposure_samples(path, samples_count);
    float window_average = falloff_type == 5 ? shutter_curve_buf[SHUTTER_CURVE_SEGMENTS].y :
                                               exposure_window_average(falloff_type, count);
    float normalization = 0.5f / (float(count) * max(window_average, 1e-8f));
    int end_sample = min(start_sample + 8, count);
    for (int sample_index = start_sample; sample_index < end_sample; sample_index++) {
      float time = (float(sample_index) + 0.5f) / float(count);
      float2 position = float2(texel) +
                        sampled_motion_path_offset(path, samples_count, sample_index, count);
      float weight = falloff_type == 5 ?
                         shutter_curve_mean(side != 0,
                                            float(sample_index) / float(count),
                                            float(sample_index + 1) / float(count)) :
                         motion_blur_falloff(falloff_type, time);
      float4 weighted_color = color * (normalization * weight);
      exposure_splat(position, weighted_color, source_depth);
    }
  }
}
