/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <array>

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

namespace blender {
struct CurveMapping;
}

namespace blender::nodes::node_composite_vector_blur_cc {

/** A shutter window and its integral over the full previous-to-next interval. */
class ShutterCurve {
 public:
  static constexpr int segments = 1024;

 private:
  /* x: Window value, y: Integral from zero to this knot. Matches the GPU buffer. */
  std::array<float2, segments + 1> table_;
  float integral(float time) const;

 public:
  explicit ShutterCurve(const CurveMapping *mapping);
  float evaluate(bool next, float time) const;
  float mean(bool next, float start, float end) const;
  float average() const;
  Span<float2> table() const;
};

}  // namespace blender::nodes::node_composite_vector_blur_cc
