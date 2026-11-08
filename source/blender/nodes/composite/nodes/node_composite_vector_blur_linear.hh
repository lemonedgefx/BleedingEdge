/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "COM_result.hh"
#include "node_composite_vector_blur_shutter.hh"

namespace blender::nodes::node_composite_vector_blur_cc::linear {

/** Stock directional filtering with source-relative exposure falloff. */
void blur(compositor::Context &context,
          const compositor::Result &image,
          const compositor::Result &depth,
          const compositor::Result &velocity,
          compositor::Result &output,
          const compositor::Domain &domain,
          int samples,
          float shutter,
          int falloff,
          const ShutterCurve &curve);

}  // namespace blender::nodes::node_composite_vector_blur_cc::linear
