/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/* Match ShutterCurve: a piecewise linear window and its exact integral. */
#define SHUTTER_CURVE_SEGMENTS 1024

float shutter_curve_integral(float time)
{
  float position = clamp(time, 0.0f, 1.0f) * float(SHUTTER_CURVE_SEGMENTS);
  int index = min(int(position), SHUTTER_CURVE_SEGMENTS - 1);
  float factor = position - float(index);
  float2 a = shutter_curve_buf[index];
  float2 b = shutter_curve_buf[index + 1];
  return a.y +
         (a.x * factor + (b.x - a.x) * (0.5f * factor * factor)) / float(SHUTTER_CURVE_SEGMENTS);
}

float shutter_curve_evaluate(bool next, float time)
{
  float shutter_time = 0.5f + (next ? 0.5f : -0.5f) * clamp(time, 0.0f, 1.0f);
  float position = shutter_time * float(SHUTTER_CURVE_SEGMENTS);
  int index = min(int(position), SHUTTER_CURVE_SEGMENTS - 1);
  return mix(shutter_curve_buf[index].x, shutter_curve_buf[index + 1].x, position - float(index));
}

float shutter_curve_mean(bool next, float start, float end)
{
  float a = clamp(start, 0.0f, 1.0f);
  float b = clamp(end, 0.0f, 1.0f);
  if (b - a < 1e-8f) {
    return shutter_curve_evaluate(next, a);
  }
  float direction = next ? 0.5f : -0.5f;
  float first = 0.5f + direction * a;
  float last = 0.5f + direction * b;
  return clamp(
      (shutter_curve_integral(last) - shutter_curve_integral(first)) / (last - first), 0.0f, 1.0f);
}
