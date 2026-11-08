/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/* A motion path in pixel space, relative to its source pixel:
 *
 *   offset(t) = tangent * t + curvature * t * t + cubic * t * t * t,   for t in [0, 1]
 *
 * For linear interpolation the tangent is the motion of the selected shutter half and the
 * curvature is zero, which reproduces the straight path offset(t) = motion * t exactly.
 * Curved interpolation uses a quadratic through three frame positions, or two cubic Hermite
 * segments when the two outer frame positions are available. */
struct MotionPath {
  /* Shutter-scaled displacement used as the tangent for a straight path. */
  float2 motion;
  float2 tangent;
  float2 curvature;
  float2 cubic;
};

/* Builds one shutter half's motion path from the source pixel's shutter-scaled motion.
 *
 * The path is the cubic Hermite segment through the current and the one frame away position of
 * the selected side, whose tangents are the central differences of their own neighbors. The
 * neighbor beyond the one frame away position comes from the extended motion, which holds the
 * offsets two frames out. Without it, the two frames out position is chosen so that the cubic
 * term vanishes, which leaves exactly the quadratic through the three available samples. */
MotionPath compute_motion_path(float4 motion,
                               float4 extended,
                               const bool next,
                               bool use_curved,
                               bool use_extended,
                               float shutter_speed)
{
  MotionPath path;
  path.motion = next ? motion.zw : motion.xy;
  path.tangent = path.motion;
  path.curvature = float2(0.0f);
  path.cubic = float2(0.0f);
  if (!use_curved) {
    return path;
  }

  /* Offsets to the neighboring positions of the selected side and of the opposite side. */
  float2 near_offset = next ? motion.zw : motion.xy;
  float2 far_offset = next ? motion.xy : motion.zw;
  float2 outer_offset = use_extended ? (next ? extended.zw : extended.xy) :
                                       (far_offset + near_offset * 3.0f);

  /* Velocity at the current time. Shared by both sides, so the two halves of the path meet
   * without a kink at the current position. */
  path.tangent = (near_offset - far_offset) * 0.5f;
  /* Note the coefficients cancel for constant velocity, where the offsets are symmetric, leaving
   * the path exactly linear. */
  path.curvature = (far_offset + near_offset * 2.0f - outer_offset * 0.5f) * shutter_speed;
  path.cubic = (outer_offset - far_offset - near_offset * 3.0f) *
               (shutter_speed * shutter_speed * 0.5f);
  return path;
}

/* Position along the path at t in [0, 1], as an offset in pixels from the pixel it belongs to. */
float2 motion_path_offset(MotionPath path, float t)
{
  return ((path.cubic * t + path.curvature) * t + path.tangent) * t;
}

/* End of the path, which is where the straight chord ends in the linear case as well. */
float2 motion_path_endpoint(MotionPath path)
{
  return path.tangent + path.curvature + path.cubic;
}

/* Upper bound of the distance in pixels between the path and its chord. The deviation is
 * curvature * (t^2 - t) + cubic * (t^3 - t), whose factors peak at 1/4 and 2/(3*sqrt(3)) over
 * [0, 1]. Together with the endpoint, this bounds movement when classifying source pixels. */
float motion_path_deviation(MotionPath path)
{
  return length(path.curvature) * 0.25f + length(path.cubic) * 0.3849f;
}

/* Conservative distance from the current position, including paths that return to it. */
float motion_path_reach(MotionPath path)
{
  return length(motion_path_endpoint(path)) + motion_path_deviation(path);
}

/* Menu values match FalloffType in node_composite_vector_blur.cc. */
float motion_blur_falloff(int type, float time)
{
  float t = clamp(time, 0.0f, 1.0f);
  switch (type) {
    case 1:
      return 1.0f - t;
    case 2:
      return 1.0f - t * t * (3.0f - 2.0f * t);
    case 3:
      return 1.0f - t * t;
    case 4:
      return (1.0f - t) * (1.0f - t);
  }
  return 1.0f;
}

float exposure_window_average(int type, int count)
{
  switch (type) {
    case 1:
    case 2:
      return 0.5f;
    case 3:
      return 2.0f / 3.0f + 1.0f / (12.0f * float(count) * float(count));
    case 4:
      return 1.0f / 3.0f - 1.0f / (12.0f * float(count) * float(count));
  }
  return 1.0f;
}

int exposure_samples(MotionPath path, int segments)
{
  float derivative_bound = length(path.tangent) + 2.0f * length(path.curvature) +
                           3.0f * length(path.cubic);
  int subdivisions = clamp(
      int(ceil(min(derivative_bound * 2.0f, 8192.0f) / float(segments))), 1, 8192 / segments);
  return segments * subdivisions;
}

float2 sampled_motion_path_offset(MotionPath path, int segments, int sample_index, int count)
{
  int subdivisions = count / segments;
  int segment = sample_index / subdivisions;
  float factor = (float(sample_index % subdivisions) + 0.5f) / float(subdivisions);
  float2 start = motion_path_offset(path, float(segment) / float(segments));
  float2 end = motion_path_offset(path, float(segment + 1) / float(segments));
  return mix(start, end, factor);
}
