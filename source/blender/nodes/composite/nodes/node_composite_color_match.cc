/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>

#include "BLI_math_vector.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_string.hh"

#include "DNA_space_types.h"

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"

#include "ED_screen.hh"

#include "IMB_colormanagement.hh"

#include "NOD_eval_log.hh"
#include "NOD_rna_define.hh"

#include "RNA_access.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"

#include "COM_algorithm_parallel_reduction.hh"
#include "COM_node_operation.hh"
#include "COM_realize_on_domain_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_color_match_cc {

enum class MatchMode : int { Color = 0, Luminance = 1, Exposure = 2 };
static constexpr int correction_locked = 1;
static constexpr float epsilon = 1.0e-6f;

static const EnumPropertyItem mode_items[] = {
    {int(MatchMode::Color), "COLOR", 0, N_("Color"), N_("Match RGB balance and contrast")},
    {int(MatchMode::Luminance),
     "LUMINANCE",
     0,
     N_("Luminance"),
     N_("Apply the same luminance gain and offset to all color channels")},
    {int(MatchMode::Exposure),
     "EXPOSURE",
     0,
     N_("Exposure"),
     N_("Match average luminance using a single exposure multiplier")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Color>("Image"_ustr)
      .default_value({0.8f, 0.8f, 0.8f, 1.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .compositor_domain_priority(0);
  b.add_output<decl::Color>("Image"_ustr)
      .structure_type(StructureType::Dynamic)
      .align_with_previous();
  b.add_output<decl::Vector>("Gain"_ustr)
      .description("Calculated RGB multiplier, before Factor is applied");
  b.add_output<decl::Vector>("Offset"_ustr)
      .description("Calculated straight-color RGB offset, before Factor is applied");
  b.add_output<decl::Bool>("Valid"_ustr)
      .description(
          "Both analysis regions contain usable pixels, or a locked correction is in use");
  b.add_input<decl::Color>("Reference"_ustr)
      .default_value({0.0f, 0.0f, 0.0f, 0.0f})
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .compositor_realization_mode(CompositorInputRealizationMode::Transforms)
      .description("Image whose color statistics to match; analyzed at its own resolution");
  b.add_input<decl::Float>("Factor"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .structure_type(StructureType::Dynamic)
      .description("Where and how strongly to apply the correction, separate from analysis masks");
  b.add_input<decl::Menu>("Mode"_ustr)
      .default_value(MatchMode::Color)
      .static_items(mode_items)
      .optional_label();
  b.add_input<decl::Float>("Contrast"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description(
          "How strongly to match the reference standard deviation; ignored in Exposure mode");

  PanelDeclarationBuilder &analysis = b.add_panel("Analysis"_ustr).default_closed(true);
  analysis.add_input<decl::Float>("Source Mask"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .structure_type(StructureType::Dynamic)
      .description("Source pixels used to calculate the match, weighted by their alpha");
  analysis.add_input<decl::Float>("Reference Mask"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .structure_type(StructureType::Dynamic)
      .compositor_realization_mode(CompositorInputRealizationMode::None)
      .description(
          "Reference pixels used for analysis, in the reference image's compositing space");

  PanelDeclarationBuilder &locked = b.add_panel("Locked Correction"_ustr).default_closed(true);
  locked.add_input<decl::Vector>("Gain"_ustr, "Stored Gain"_ustr)
      .default_value(float3(1.0f))
      .description("Saved multiplier used after Lock Match; can be adjusted without re-analysis");
  locked.add_input<decl::Vector>("Offset"_ustr, "Stored Offset"_ustr)
      .default_value(float3(0.0f))
      .description("Saved offset used after Lock Match; can be adjusted without re-analysis");
}

using namespace blender::compositor;

struct Statistics {
  float3 mean = float3(0.0f);
  float3 deviation = float3(0.0f);
  bool valid = false;
};

/* Accumulate straight colors, not premultiplied colors. Alpha and the analysis mask are weights.
 * Two passes avoid the loss of precision of E[x*x] - E[x]*E[x] on nearly constant HDR images. */
static float4 statistics_pixel(const float4 &color,
                               const float mask,
                               const bool luminance_only,
                               const float3 &luminance_coefficients,
                               const bool squared_difference,
                               const float3 &mean)
{
  if (!(color.w > epsilon) || !std::isfinite(color.w) || !std::isfinite(mask) || mask <= 0.0f) {
    return float4(0.0f);
  }
  float3 value = color.xyz() / color.w;
  /* Ignore non-finite and extreme pixels rather than poisoning the entire image reduction. */
  if (!(std::abs(value.x) < 1.0e10f && std::abs(value.y) < 1.0e10f && std::abs(value.z) < 1.0e10f))
  {
    return float4(0.0f);
  }
  if (luminance_only) {
    value = float3(math::dot(value, luminance_coefficients));
  }
  const float weight = math::clamp(mask, 0.0f, 1.0f) * math::min(color.w, 1.0f);
  if (squared_difference) {
    value -= mean;
    value *= value;
  }
  return float4(value * weight, weight);
}

class ColorMatchOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    float3 gain(1.0f);
    float3 offset(0.0f);
    bool valid = true;
    if (node().custom1 & correction_locked) {
      gain = get_input("Stored Gain").get_single_value_default<float3>();
      offset = get_input("Stored Offset").get_single_value_default<float3>();
    }
    else {
      float3 luminance_coefficients;
      IMB_colormanagement_get_luminance_coefficients(luminance_coefficients);
      const MatchMode mode = MatchMode(
          get_input("Mode").get_single_value_default<MenuValue>().value);
      const bool luminance_only = mode != MatchMode::Color;
      const Statistics source = statistics(
          get_input("Image"), get_input("Source Mask"), luminance_only, luminance_coefficients);
      const Statistics reference = statistics(get_input("Reference"),
                                              get_input("Reference Mask"),
                                              luminance_only,
                                              luminance_coefficients);
      valid = source.valid && reference.valid;
      if (valid && mode == MatchMode::Exposure) {
        valid = source.mean.x > epsilon && reference.mean.x >= 0.0f;
        if (valid) {
          gain = float3(math::clamp(reference.mean.x / source.mean.x, 0.0f, 10000.0f));
        }
      }
      else if (valid) {
        const float contrast = math::clamp(
            get_input("Contrast").get_single_value_default<float>(), 0.0f, 1.0f);
        for (int i = 0; i < 3; i++) {
          /* A flat source contains no contrast to recover. Match its mean without dividing by
           * zero or amplifying quantization noise into extreme artifacts. */
          const float minimum_deviation = epsilon * math::max(1.0f, std::abs(source.mean[i]));
          const float ratio = source.deviation[i] > minimum_deviation ?
                                  math::clamp(reference.deviation[i] / source.deviation[i],
                                              0.0f,
                                              10000.0f) :
                                  1.0f;
          gain[i] = math::interpolate(1.0f, ratio, contrast);
        }
        offset = reference.mean - source.mean * gain;
      }
      if (!valid) {
        add_warning(NodeWarningType::Warning,
                    "Color Match needs non-empty, finite analysis regions. Exposure matching also "
                    "needs positive source luminance. The image is unchanged.");
      }
    }

    /* These inexpensive single-value results are also logged when unlinked, so Lock Match can
     * capture the latest evaluated correction without writing to node data from a render thread.
     */
    get_result("Gain").allocate_single_value();
    get_result("Gain").set_single_value(gain);
    get_result("Offset").allocate_single_value();
    get_result("Offset").set_single_value(offset);
    get_result("Valid").allocate_single_value();
    get_result("Valid").set_single_value(valid);

    if (!get_result("Image").should_compute()) {
      return;
    }
    if (!valid || (gain == float3(1.0f) && offset == float3(0.0f))) {
      get_result("Image").share_data(get_input("Image"));
      return;
    }
    apply(gain, offset);
  }

 private:
  float4 sum_statistics(const Result &image,
                        const Result &mask,
                        const bool luminance_only,
                        const float3 &luminance_coefficients,
                        const bool squared_difference,
                        const float3 &mean)
  {
    if (image.is_single_value() && mask.is_single_value()) {
      return statistics_pixel(float4(image.get_single_value<Color>()),
                              mask.get_single_value<float>(),
                              luminance_only,
                              luminance_coefficients,
                              squared_difference,
                              mean);
    }
    Result weighted = context().create_result(ResultType::Color, ResultPrecision::Full);
    weighted.allocate_texture(image.is_single_value() ? mask.domain() : image.domain());
    if (context().use_gpu()) {
      gpu::Shader *shader = context().get_shader("compositor_color_match_statistics",
                                                 ResultPrecision::Full);
      GPU_shader_bind(shader);
      GPU_shader_uniform_1b(shader, "luminance_only", luminance_only);
      GPU_shader_uniform_1b(shader, "squared_difference", squared_difference);
      GPU_shader_uniform_3fv(shader, "luminance_coefficients", luminance_coefficients);
      GPU_shader_uniform_3fv(shader, "mean", mean);
      gpu::Texture *image_texture = image.bind_as_texture_or_single_value(shader, "input_tx");
      gpu::Texture *mask_texture = mask.bind_as_texture_or_single_value(shader, "mask_tx");
      weighted.bind_as_image(shader, "output_img");
      compute_dispatch_threads_at_least(shader, weighted.domain().data_size);
      GPU_shader_unbind();
      image.unbind_as_texture_or_single_value(image_texture);
      mask.unbind_as_texture_or_single_value(mask_texture);
      weighted.unbind_as_image();
    }
    else {
      parallel_for(weighted.domain().data_size, [&](const int2 texel) {
        weighted.store_pixel(texel,
                             Color(statistics_pixel(float4(image.load_pixel<Color, true>(texel)),
                                                    mask.load_pixel<float, true>(texel),
                                                    luminance_only,
                                                    luminance_coefficients,
                                                    squared_difference,
                                                    mean)));
      });
    }
    const float4 sum = sum_color(context(), weighted);
    weighted.release();
    return sum;
  }

  Statistics statistics(const Result &image,
                        Result &input_mask,
                        const bool luminance_only,
                        const float3 &luminance_coefficients)
  {
    Result mask = context().create_result(ResultType::Float);
    if (input_mask.is_single_value() || image.is_single_value() ||
        input_mask.domain() == image.domain())
    {
      mask.share_data(input_mask);
    }
    else {
      /* Keep reference analysis independent from the source size, including its mask. */
      RealizeOnDomainOperation realize(context(), image.domain(), ResultType::Float);
      realize.map_input_to_result(&input_mask);
      realize.execute();
      mask.share_data(realize.get_result());
      realize.get_result().release();
    }

    Statistics result;
    const float4 sum = sum_statistics(
        image, mask, luminance_only, luminance_coefficients, false, float3(0.0f));
    if (sum.w > 0.0f && std::isfinite(sum.w)) {
      result.mean = sum.xyz() / sum.w;
      const float4 squared = sum_statistics(
          image, mask, luminance_only, luminance_coefficients, true, result.mean);
      result.deviation = math::sqrt(math::max(squared.xyz() / sum.w, float3(0.0f)));
      result.valid = true;
    }
    mask.release();
    return result;
  }

  static float4 apply_pixel(const float4 &color,
                            const float factor,
                            const float3 &gain,
                            const float3 &offset)
  {
    const float influence = math::clamp(factor, 0.0f, 1.0f);
    const float3 corrected = color.xyz() * gain + offset * color.w;
    return float4(math::interpolate(color.xyz(), corrected, influence), color.w);
  }

  void apply(const float3 &gain, const float3 &offset)
  {
    const Result &image = get_input("Image");
    const Result &factor = get_input("Factor");
    Result &output = get_result("Image");
    if (image.is_single_value() && factor.is_single_value()) {
      output.allocate_single_value();
      output.set_single_value(Color(apply_pixel(float4(image.get_single_value<Color>()),
                                                factor.get_single_value<float>(),
                                                gain,
                                                offset)));
      return;
    }
    output.allocate_texture(compute_domain());
    if (context().use_gpu()) {
      gpu::Shader *shader = context().get_shader("compositor_color_match_apply");
      GPU_shader_bind(shader);
      GPU_shader_uniform_3fv(shader, "gain", gain);
      GPU_shader_uniform_3fv(shader, "offset", offset);
      gpu::Texture *image_texture = image.bind_as_texture_or_single_value(shader, "input_tx");
      gpu::Texture *factor_texture = factor.bind_as_texture_or_single_value(shader, "factor_tx");
      output.bind_as_image(shader, "output_img");
      compute_dispatch_threads_at_least(shader, output.domain().data_size);
      GPU_shader_unbind();
      image.unbind_as_texture_or_single_value(image_texture);
      factor.unbind_as_texture_or_single_value(factor_texture);
      output.unbind_as_image();
    }
    else {
      parallel_for(output.domain().data_size, [&](const int2 texel) {
        output.store_pixel(texel,
                           Color(apply_pixel(float4(image.load_pixel<Color, true>(texel)),
                                             factor.load_pixel<float, true>(texel),
                                             gain,
                                             offset)));
      });
    }
  }
};

static bool lock_match_poll(bContext *C)
{
  const SpaceNode *snode = CTX_wm_space_node(C);
  return ED_operator_node_editable(C) && snode && snode->edittree &&
         snode->edittree->type == NTREE_COMPOSIT;
}

static wmOperatorStatus lock_match_exec(bContext *C, wmOperator *op)
{
  /* Evaluation logs are assembled by render/compositor jobs. Read only completed logs, and do
   * not change a correction while a rendering thread may still be using the node tree. */
  if (G.is_rendering || WM_jobs_has_running(CTX_wm_manager(C), nullptr, WM_JOB_TYPE_COMPOSITE)) {
    BKE_report(
        op->reports, RPT_ERROR, "Wait for rendering and compositing to finish before locking");
    return OPERATOR_CANCELLED;
  }
  SpaceNode &snode = *CTX_wm_space_node(C);
  bNodeTree &tree = *snode.edittree;
  char name[MAX_NAME];
  RNA_string_get(op->ptr, "node_name", name);
  bNode *node = bke::node_find_node_by_name(tree, name);
  if (!node || node->idname != "CompositorNodeColorMatch"_ustr) {
    BKE_report(op->reports, RPT_ERROR, "The Color Match node is no longer available");
    return OPERATOR_CANCELLED;
  }
  if (node->custom1 & correction_locked) {
    node->custom1 &= ~correction_locked;
  }
  else {
    const eval_log::ContextualNodeTreeLogs logs = eval_log::NodesEvalLog::get_contextual_tree_logs(
        snode);
    eval_log::NodeTreeLog *log = logs.get_main_tree_log(*node);
    if (!log) {
      BKE_report(
          op->reports, RPT_ERROR, "Evaluate Color Match in the compositor before locking it");
      return OPERATOR_CANCELLED;
    }
    log->ensure_socket_values();
    const std::optional<float3> gain = log->find_primitive_socket_value<float3>(
        *node->output_by_identifier("Gain"_ustr));
    const std::optional<float3> offset = log->find_primitive_socket_value<float3>(
        *node->output_by_identifier("Offset"_ustr));
    const std::optional<bool> valid = log->find_primitive_socket_value<bool>(
        *node->output_by_identifier("Valid"_ustr));
    if (!gain || !offset || !valid.value_or(false)) {
      BKE_report(op->reports, RPT_ERROR, "No valid evaluated match is available to lock");
      return OPERATOR_CANCELLED;
    }
    bNodeSocket &gain_input = *node->input_by_identifier("Stored Gain"_ustr);
    bNodeSocket &offset_input = *node->input_by_identifier("Stored Offset"_ustr);
    if (gain_input.is_directly_linked() || offset_input.is_directly_linked()) {
      BKE_report(op->reports, RPT_ERROR, "Disconnect the stored correction inputs before locking");
      return OPERATOR_CANCELLED;
    }
    copy_v3_v3(gain_input.default_value_typed<bNodeSocketValueVector>()->value, *gain);
    copy_v3_v3(offset_input.default_value_typed<bNodeSocketValueVector>()->value, *offset);
    BKE_ntree_update_tag_socket_property(&tree, &gain_input);
    BKE_ntree_update_tag_socket_property(&tree, &offset_input);
    node->custom1 |= correction_locked;
  }
  BKE_ntree_update_tag_node_property(&tree, node);
  BKE_main_ensure_invariants(*CTX_data_main(C), tree.id);
  DEG_id_tag_update(&tree.id, ID_RECALC_NTREE_OUTPUT);
  WM_event_add_notifier(C, NC_NODE | NA_EDITED, &tree);
  return OPERATOR_FINISHED;
}

static void lock_match_operator(wmOperatorType *ot)
{
  ot->name = "Lock Color Match";
  ot->idname = "NODE_OT_color_match_lock";
  ot->description =
      "Lock the latest evaluated correction, or unlock it to analyze each frame again";
  ot->poll = lock_match_poll;
  ot->exec = lock_match_exec;
  ot->flag = OPTYPE_UNDO;
  RNA_def_string(ot->srna, "node_name", nullptr, MAX_NAME, "Node", "Color Match node to update");
}

static void node_draw_buttons(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  const bNode &node = *ptr->data_as<bNode>();
  const bool locked = node.custom1 & correction_locked;
  PointerRNA op = layout.op("NODE_OT_color_match_lock",
                            locked ? "Unlock Match" : "Lock Match",
                            locked ? ICON_LOCKED : ICON_UNLOCKED);
  RNA_string_set(&op, "node_name", node.name);
}

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new ColorMatchOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;
  cmp_node_type_base(&ntype, "CompositorNodeColorMatch"_ustr);
  ntype.ui_name = "Color Match";
  ntype.ui_description =
      "Match exposure, color balance and contrast to independently masked reference regions";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_draw_buttons;
  ntype.register_operators = []() { WM_operatortype_append(lock_match_operator); };
  ntype.get_compositor_operation = get_compositor_operation;
  ntype.default_width = bke::NodeWidth::_200;
  bke::node_register_type(ntype);
  RNA_def_node_boolean(ntype.rna_ext.srna,
                       "correction_locked",
                       "Correction Locked",
                       "Use the saved gain and offset instead of analyzing the images",
                       NOD_inline_boolean_accessors(custom1, correction_locked),
                       false);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_composite_color_match_cc
