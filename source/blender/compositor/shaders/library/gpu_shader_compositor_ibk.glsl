/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_math_vector_reduce.bsl.hh"

#define IBK_SCREEN_EPSILON 1.0e-6f

int ibk_channel(float3 screen, int channel)
{
  if (channel != 0) {
    return channel;
  }
  return screen.x > screen.y ? (screen.x > screen.z ? 0 : 2) : (screen.y > screen.z ? 1 : 2);
}

float ibk_excess(float3 color, int channel, float balance)
{
  int first = (channel + 1) % 3;
  int second = (channel + 2) % 3;
  return color[channel] - mix(color[min(first, second)], color[max(first, second)], balance);
}

float3 ibk_screen_color(float4 plate, float exposure)
{
  return plate.a > IBK_SCREEN_EPSILON ? plate.rgb * (exposure / plate.a) : float3(0.0f);
}

bool ibk_valid_screen(float4 plate, float3 screen, int channel, float balance)
{
  return plate.a > IBK_SCREEN_EPSILON &&
         ibk_excess(screen, channel, balance) >
             IBK_SCREEN_EPSILON * max(1.0f, reduce_max(abs(screen)));
}

float ibk_matte(float4 color, float4 plate, int channel, float balance, float exposure)
{
  if (color.a <= IBK_SCREEN_EPSILON) {
    return 0.0f;
  }
  float3 screen = ibk_screen_color(plate, exposure);
  int index = ibk_channel(screen, channel);
  if (!ibk_valid_screen(plate, screen, index, balance)) {
    return 1.0f;
  }
  float source_excess = ibk_excess(color.rgb / color.a, index, balance);
  float plate_excess = ibk_excess(screen, index, balance);
  return 1.0f - clamp(source_excess / plate_excess, 0.0f, 1.0f);
}

float4 ibk_image(float4 color,
                 float4 plate,
                 float coverage,
                 int channel,
                 float balance,
                 float exposure,
                 float despill)
{
  float alpha = clamp(color.a, 0.0f, 1.0f) * clamp(coverage, 0.0f, 1.0f);
  if (alpha <= 0.0f || color.a <= IBK_SCREEN_EPSILON) {
    return float4(0.0f);
  }
  float3 screen = ibk_screen_color(plate, exposure);
  int index = ibk_channel(screen, channel);
  float3 result = color.rgb * (alpha / color.a);
  if (ibk_valid_screen(plate, screen, index, balance)) {
    float3 foreground = color.rgb - screen * (color.a - alpha);
    result = mix(result, foreground, despill);
  }
  return float4(result, alpha);
}
