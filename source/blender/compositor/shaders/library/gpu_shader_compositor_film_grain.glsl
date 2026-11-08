/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/* Keep in sync with node_composite_film_grain.cc. All integer arithmetic wraps at 32 bits. */
uint grain_hash(uint value)
{
  value ^= value >> 16;
  value *= 0x7feb352dU;
  value ^= value >> 15;
  value *= 0x846ca68bU;
  value ^= value >> 16;
  return value;
}

float grain_random(uint value)
{
  return float(grain_hash(value) >> 8) * (1.0f / 16777216.0f);
}

float grain_cell(int2 cell, uint random_seed)
{
  uint base = grain_hash(uint(cell.x) ^ grain_hash(uint(cell.y) + 0x9e3779b9U) ^ random_seed);
  return (grain_random(base) + grain_random(base + 0x9e3779b9U) +
          grain_random(base + 0x3c6ef372U) + grain_random(base + 0xdaa66d2bU) - 2.0f) *
         1.7320508075688772f;
}

float grain_noise(float2 position, float size, uint random_seed)
{
  float2 coordinates = position / size - float2(0.5f);
  float2 floored = floor(coordinates);
  int2 cell = int2(floored);
  float2 fraction = coordinates - floored;
  float2 t = fraction * fraction * (float2(3.0f) - 2.0f * fraction);
  float4 weights = float4(
      (1.0f - t.x) * (1.0f - t.y), t.x * (1.0f - t.y), (1.0f - t.x) * t.y, t.x * t.y);
  float4 values = float4(grain_cell(cell, random_seed),
                         grain_cell(cell + int2(1, 0), random_seed),
                         grain_cell(cell + int2(0, 1), random_seed),
                         grain_cell(cell + int2(1, 1), random_seed));
  return dot(weights, values) / sqrt(dot(weights, weights));
}
