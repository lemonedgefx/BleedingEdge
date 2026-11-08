/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_compositor_texture_utilities.glsl"

/* Keep the numerical contract in sync with node_composite_optical_flow.cc. */
bool flow_inside(float2 point, int2 size)
{
  return all(greaterThanEqual(point, float2(0.0f))) &&
         all(lessThanEqual(point, float2(size - int2(1))));
}

float2 flow_bounded(float2 value, float maximum)
{
  float magnitude = length(value);
  return magnitude > maximum && magnitude > 0.0f ? value * (maximum / magnitude) : value;
}

float4 flow_sample(sampler2D image, float2 point)
{
  float2 floored = floor(point);
  int2 base = int2(floored);
  float2 t = point - floored;
  return mix(
      mix(texture_load(image, base), texture_load(image, base + int2(1, 0)), t.x),
      mix(texture_load(image, base + int2(0, 1)), texture_load(image, base + int2(1, 1)), t.x),
      t.y);
}

float flow_luminance(float4 value)
{
  return value.y > 1.0e-6f ? value.x / value.y : 0.0f;
}

float flow_gaussian_weight(int offset)
{
  return offset == 0 ? 6.0f : (abs(offset) == 1 ? 4.0f : 1.0f);
}

float4 flow_gradient(sampler2D image, int2 point)
{
  float4 center = texture_load(image, point);
  float4 left = texture_load(image, point - int2(1, 0));
  float4 right = texture_load(image, point + int2(1, 0));
  float4 down = texture_load(image, point - int2(0, 1));
  float4 up = texture_load(image, point + int2(0, 1));
  float alpha = min(center.y, min(min(left.y, right.y), min(down.y, up.y)));
  return float4(flow_luminance(center),
                0.5f * (flow_luminance(right) - flow_luminance(left)),
                0.5f * (flow_luminance(up) - flow_luminance(down)),
                alpha);
}

float4 flow_initial(sampler2D gradient, sampler2D reference, int2 point, int radius, float maximum)
{
  if (texture_load(gradient, point).w <= 1.0e-6f) {
    return float4(0.0f);
  }
  float total = 0.0f, mean = 0.0f, squared = 0.0f;
  for (int y = -radius; y <= radius; y++) {
    for (int x = -radius; x <= radius; x++) {
      float4 a = texture_load(gradient, point + int2(x, y), float4(0.0f));
      total += a.w;
      mean += a.x * a.w;
      squared += a.x * a.x * a.w;
    }
  }
  float variance = total > 1.0e-6f ? max(0.0f, squared / total - (mean / total) * (mean / total)) :
                                     0.0f;
  if (variance < 1.0e-8f) {
    return float4(0.0f);
  }
  int substeps = maximum > 4.0f ? 4 : 2;
  int search_radius = substeps * min(8, int(ceil(maximum)));
  float best_cost = 3.402823466e38f;
  float2 best = float2(0.0f);
  for (int dy = -search_radius; dy <= search_radius; dy++) {
    for (int dx = -search_radius; dx <= search_radius; dx++) {
      float2 candidate = float2(dx, dy) / float(substeps);
      if (length(candidate) > maximum ||
          !flow_inside(float2(point) + candidate, texture_size(reference)))
      {
        continue;
      }
      float cost = 0.0f, weight = 0.0f;
      for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
          int2 p = point + int2(x, y);
          float2 q = float2(p) + candidate;
          if (!flow_inside(q, texture_size(reference))) {
            continue;
          }
          float4 a = texture_load(gradient, p, float4(0.0f));
          float4 b = flow_sample(reference, q);
          float w = min(a.w, b.y);
          float error = a.x - flow_luminance(b);
          cost += w * error * error;
          weight += w;
        }
      }
      if (weight < 0.5f * total || weight <= 1.0e-6f) {
        continue;
      }
      cost /= weight;
      cost += 0.005f * variance * dot(candidate, candidate);
      if (cost < best_cost - 1.0e-8f ||
          (abs(cost - best_cost) <= 1.0e-8f && dot(candidate, candidate) < dot(best, best)))
      {
        best_cost = cost;
        best = candidate;
      }
    }
  }
  return float4(best.x, best.y, 0.0f, 0.0f);
}

float4 flow_iterate(sampler2D gradient,
                    sampler2D reference,
                    sampler2D previous,
                    int2 point,
                    int radius,
                    float maximum)
{
  float2 flow = texture_load(previous, point).xy;
  if (texture_load(gradient, point).w <= 1.0e-6f) {
    return float4(0.0f);
  }
  float xx = 0.0f, xy = 0.0f, yy = 0.0f, bx = 0.0f, by = 0.0f, weight = 0.0f, error_squared = 0.0f;
  for (int y = -radius; y <= radius; y++) {
    for (int x = -radius; x <= radius; x++) {
      int2 p = point + int2(x, y);
      float2 q = float2(p) + flow;
      if (!flow_inside(q, texture_size(reference))) {
        continue;
      }
      float4 a = texture_load(gradient, p, float4(0.0f));
      float4 b = flow_sample(reference, q);
      float w = min(a.w, b.y);
      float error = a.x - flow_luminance(b);
      xx += w * a.y * a.y;
      xy += w * a.y * a.z;
      yy += w * a.z * a.z;
      bx += w * a.y * error;
      by += w * a.z * error;
      error_squared += w * error * error;
      weight += w;
    }
  }
  if (weight <= 1.0e-6f) {
    return float4(flow.x, flow.y, 0.0f, 0.0f);
  }
  float trace = xx + yy;
  float regularization = trace * 0.0001f + weight * 1.0e-8f;
  float determinant = (xx + regularization) * (yy + regularization) - xy * xy;
  float2 delta = float2(0.0f);
  if (trace > weight * 1.0e-7f && determinant > 1.0e-20f) {
    delta = float2((yy + regularization) * bx - xy * by, (xx + regularization) * by - xy * bx) /
            determinant;
  }
  float2 refined = flow_bounded(flow + flow_bounded(delta, 1.0f), maximum);
  float condition = clamp(
      4.0f * max(0.0f, xx * yy - xy * xy) / max(trace * trace, 1.0e-20f), 0.0f, 1.0f);
  float texture = trace / (trace + weight * 1.0e-5f);
  float support = min(1.0f, weight / float((2 * radius + 1) * (2 * radius + 1)));
  float photometric = 1.0f / (1.0f + error_squared / (weight * 0.0025f));
  return float4(refined.x, refined.y, condition * texture * support, photometric);
}
