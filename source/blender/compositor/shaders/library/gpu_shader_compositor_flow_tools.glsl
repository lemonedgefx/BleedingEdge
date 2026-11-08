/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_compositor_texture_utilities.glsl"

bool tools_finite_motion(float2 value)
{
  return !any(isnan(value)) && !any(isinf(value)) && length(value) <= 2048.0f;
}

float tools_confidence(float value)
{
  return !isnan(value) && !isinf(value) ? clamp(value, 0.0f, 1.0f) : 0.0f;
}

float4 tools_sample(sampler2D image, float2 point)
{
  int2 base = int2(floor(point));
  float2 t = point - float2(base);
  return mix(
      mix(texture_load(image, base), texture_load(image, base + int2(1, 0)), t.x),
      mix(texture_load(image, base + int2(0, 1)), texture_load(image, base + int2(1, 1)), t.x),
      t.y);
}

bool tools_inside(float2 point, int2 size)
{
  return all(greaterThanEqual(point, float2(0.0f))) && all(lessThanEqual(point, float2(size - 1)));
}

/* The returned XY is the source position; Z is confidence. Mirror inverse_warp in C++. */
float3 tools_inverse_warp(sampler2D flow,
                          sampler2D reverse,
                          sampler2D confidence,
                          float2 destination,
                          float time,
                          int2 size)
{
  float2 point = destination;
  for (int iteration = 0; iteration < 12; iteration++) {
    float2 motion = tools_sample(flow, point).xy;
    if (!tools_finite_motion(motion)) {
      return float3(destination, 0.0f);
    }
    point = destination - time * motion;
  }
  if (!tools_inside(point, size)) {
    return float3(destination, 0.0f);
  }
  float2 motion = tools_sample(flow, point).xy;
  float2 end = point + motion;
  if (!tools_finite_motion(motion) || !tools_inside(end, size)) {
    return float3(destination, 0.0f);
  }
  float2 back = tools_sample(reverse, end).xy;
  if (!tools_finite_motion(back)) {
    return float3(destination, 0.0f);
  }
  float2 residual = point + time * motion - destination;
  float tolerance = 1.0f + 0.01f * (length(motion) + length(back));
  float consistency = 1.0f / (1.0f + dot(motion + back, motion + back) / (tolerance * tolerance));
  return float3(point,
                tools_confidence(tools_sample(confidence, point).x) * consistency /
                    (1.0f + dot(residual, residual)));
}
