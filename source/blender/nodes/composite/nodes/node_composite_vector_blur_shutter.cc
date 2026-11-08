/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_composite_vector_blur_shutter.hh"

#include "BKE_colortools.hh"
#include "BLI_math_base.hh"

namespace blender::nodes::node_composite_vector_blur_cc {

ShutterCurve::ShutterCurve(const CurveMapping *mapping)
{
  /* Curve tables are mutable. Bake a private copy before entering parallel gathers. */
  CurveMapping *copy = mapping ? BKE_curvemapping_copy(mapping) : nullptr;
  if (copy) {
    BKE_curvemapping_init(copy);
  }
  for (int i = 0; i <= segments; i++) {
    const float time = float(i) / float(segments);
    const float value = copy ? BKE_curvemapping_evaluateF(copy, 0, time) :
                               1.0f - math::abs(2.0f * time - 1.0f);
    table_[i].x = math::clamp(value, 0.0f, 1.0f);
    table_[i].y = i == 0 ?
                      0.0f :
                      table_[i - 1].y + (table_[i - 1].x + table_[i].x) * (0.5f / float(segments));
  }
  BKE_curvemapping_free(copy);
}

float ShutterCurve::integral(const float time) const
{
  const float position = math::clamp(time, 0.0f, 1.0f) * float(segments);
  const int index = math::min(int(position), segments - 1);
  const float factor = position - float(index);
  const float2 a = table_[index];
  const float2 b = table_[index + 1];
  return a.y + (a.x * factor + (b.x - a.x) * (0.5f * factor * factor)) / float(segments);
}

float ShutterCurve::evaluate(const bool next, const float time) const
{
  const float shutter_time = 0.5f + (next ? 0.5f : -0.5f) * math::clamp(time, 0.0f, 1.0f);
  const float position = shutter_time * float(segments);
  const int index = math::min(int(position), segments - 1);
  return math::interpolate(table_[index].x, table_[index + 1].x, position - float(index));
}

float ShutterCurve::mean(const bool next, const float start, const float end) const
{
  const float a = math::clamp(start, 0.0f, 1.0f);
  const float b = math::clamp(end, 0.0f, 1.0f);
  if (b - a < 1e-8f) {
    return evaluate(next, a);
  }
  const float direction = next ? 0.5f : -0.5f;
  const float first = 0.5f + direction * a;
  const float last = 0.5f + direction * b;
  return math::clamp((integral(last) - integral(first)) / (last - first), 0.0f, 1.0f);
}

float ShutterCurve::average() const
{
  return table_.back().y;
}

Span<float2> ShutterCurve::table() const
{
  return table_;
}

}  // namespace blender::nodes::node_composite_vector_blur_cc
