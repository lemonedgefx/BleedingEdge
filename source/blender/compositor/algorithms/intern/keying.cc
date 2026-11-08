/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"

#include "GPU_shader.hh"

#include "COM_algorithm_keying.hh"
#include "COM_algorithm_morphological_distance.hh"
#include "COM_algorithm_morphological_distance_feather.hh"
#include "COM_algorithm_symmetric_separable_blur.hh"
#include "COM_utilities.hh"

namespace blender::compositor {

static void tweak_matte_gpu(Context &context,
                            const Result &input_matte,
                            const Result &garbage_matte,
                            const Result &core_matte,
                            const KeyingMatteSettings &settings,
                            Result &output_matte,
                            Result *output_edges)
{
  gpu::Shader *shader = context.get_shader(output_edges ?
                                               "compositor_keying_tweak_matte_with_edges" :
                                               "compositor_keying_tweak_matte_without_edges");
  GPU_shader_bind(shader);
  GPU_shader_uniform_1i(shader, "edge_search_radius", settings.edge_search_radius);
  GPU_shader_uniform_1f(shader, "edge_tolerance", settings.edge_tolerance);
  GPU_shader_uniform_1f(shader, "black_level", settings.black_level);
  GPU_shader_uniform_1f(shader, "white_level", settings.white_level);

  input_matte.bind_as_texture(shader, "input_matte_tx");
  gpu::Texture *garbage_texture = garbage_matte.bind_as_texture_or_single_value(
      shader, "garbage_matte_tx");
  gpu::Texture *core_texture = core_matte.bind_as_texture_or_single_value(shader, "core_matte_tx");
  output_matte.allocate_texture(input_matte.domain());
  output_matte.bind_as_image(shader, "output_matte_img");
  if (output_edges) {
    output_edges->allocate_texture(input_matte.domain());
    output_edges->bind_as_image(shader, "output_edges_img");
  }

  compute_dispatch_threads_at_least(shader, input_matte.domain().data_size);

  GPU_shader_unbind();
  input_matte.unbind_as_texture();
  garbage_matte.unbind_as_texture_or_single_value(garbage_texture);
  core_matte.unbind_as_texture_or_single_value(core_texture);
  output_matte.unbind_as_image();
  if (output_edges) {
    output_edges->unbind_as_image();
  }
}

static void tweak_matte_cpu(const Result &input_matte,
                            const Result &garbage_matte,
                            const Result &core_matte,
                            const KeyingMatteSettings &settings,
                            Result &output_matte,
                            Result *output_edges)
{
  output_matte.allocate_texture(input_matte.domain());
  if (output_edges) {
    output_edges->allocate_texture(input_matte.domain());
  }

  parallel_for(input_matte.domain().data_size, [&](const int2 texel) {
    const float matte = input_matte.load_pixel<float>(texel);
    bool is_edge = false;
    if (output_edges || settings.black_level != 0.0f || settings.white_level != 1.0f) {
      int count = 0;
      const int radius = settings.edge_search_radius;
      for (int j = -radius; j <= radius; j++) {
        for (int i = -radius; i <= radius; i++) {
          const float neighbor = input_matte.load_pixel_extended<float>(texel + int2(i, j));
          count += int(math::distance(matte, neighbor) < settings.edge_tolerance);
        }
      }
      is_edge = count < ((radius * 2 + 1) * (radius * 2 + 1)) * 0.9f;
    }

    float tweaked = matte;
    if (!is_edge && settings.white_level != settings.black_level) {
      tweaked = math::clamp((matte - settings.black_level) /
                                (settings.white_level - settings.black_level),
                            0.0f,
                            1.0f);
    }
    tweaked = math::min(tweaked, 1.0f - garbage_matte.load_pixel<float, true>(texel));
    tweaked = math::max(tweaked, core_matte.load_pixel<float, true>(texel));
    output_matte.store_pixel(texel, tweaked);
    if (output_edges) {
      output_edges->store_pixel(texel, is_edge ? 1.0f : 0.0f);
    }
  });
}

void keying_matte(Context &context,
                  const Result &input_matte,
                  const Result &garbage_matte,
                  const Result &core_matte,
                  const KeyingMatteSettings &settings,
                  Result *output_matte,
                  Result *output_edges)
{
  BLI_assert(output_matte || output_edges);
  if (input_matte.is_single_value()) {
    BLI_assert(garbage_matte.is_single_value() && core_matte.is_single_value());
    float matte = input_matte.get_single_value<float>();
    if (settings.white_level != settings.black_level) {
      matte = math::clamp((matte - settings.black_level) /
                              (settings.white_level - settings.black_level),
                          0.0f,
                          1.0f);
    }
    matte = math::min(matte, 1.0f - garbage_matte.get_single_value<float>());
    matte = math::max(matte, core_matte.get_single_value<float>());
    if (output_matte) {
      output_matte->allocate_single_value();
      output_matte->set_single_value(matte);
    }
    if (output_edges) {
      output_edges->allocate_single_value();
      output_edges->set_single_value(0.0f);
    }
    return;
  }

  Result tweaked = context.create_result(ResultType::Float);
  const bool no_masks = garbage_matte.is_single_value() && core_matte.is_single_value() &&
                        garbage_matte.get_single_value<float>() == 0.0f &&
                        core_matte.get_single_value<float>() == 0.0f;
  if (!output_edges && settings.black_level == 0.0f && settings.white_level == 1.0f && no_masks) {
    tweaked.share_data(input_matte);
  }
  else if (context.use_gpu()) {
    tweak_matte_gpu(
        context, input_matte, garbage_matte, core_matte, settings, tweaked, output_edges);
  }
  else {
    tweak_matte_cpu(input_matte, garbage_matte, core_matte, settings, tweaked, output_edges);
  }

  if (!output_matte) {
    tweaked.release();
    return;
  }

  Result blurred = context.create_result(ResultType::Float);
  if (settings.blur_size == 0) {
    blurred.share_data(tweaked);
  }
  else {
    symmetric_separable_blur(
        context, tweaked, blurred, float2(settings.blur_size) / 2.0f, math::FilterKernel::Box);
  }
  tweaked.release();

  Result morphed = context.create_result(ResultType::Float);
  if (settings.dilate_size == 0) {
    morphed.share_data(blurred);
  }
  else {
    morphological_distance(context, blurred, morphed, settings.dilate_size);
  }
  blurred.release();

  if (settings.feather_size == 0) {
    output_matte->share_data(morphed);
  }
  else {
    morphological_distance_feather(
        context, morphed, *output_matte, settings.feather_size, settings.feather_falloff);
  }
  morphed.release();
}

}  // namespace blender::compositor
