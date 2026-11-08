/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <utility>

#include "NOD_rna_define.hh"

#include "RNA_access.hh"

#include "DNA_mesh_types.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "GEO_mesh_edge_loop.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_edge_loop_selection_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Bool>("Selection"_ustr, "Start Edges"_ustr)
      .default_value(false)
      .hide_value()
      .structure_type(StructureType::Field)
      .description("Edges or faces from which to find connected loops");
  b.add_output<decl::Bool>("Selection"_ustr)
      .structure_type(StructureType::Field)
      .propagate_references()
      .description("The edge or face loops found from the input selection");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "domain", UI_ITEM_NONE, "", ICON_NONE);
  if (RNA_enum_get(ptr, "domain") == int(AttrDomain::Face)) {
    layout.prop(ptr, "direction", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int16_t(geometry::FaceLoopDirection::U);
  node->custom2 = int16_t(AttrDomain::Edge);
}

static AttrDomain node_domain(const bNode &node)
{
  return node.custom2 == int(AttrDomain::Face) ? AttrDomain::Face : AttrDomain::Edge;
}

static geometry::FaceLoopDirection node_direction(const bNode &node)
{
  return node.custom1 == int(geometry::FaceLoopDirection::V) ? geometry::FaceLoopDirection::V :
                                                               geometry::FaceLoopDirection::U;
}

class LoopSelectionFieldInput final : public bke::MeshFieldInput {
 private:
  Field<bool> input_selection_;
  AttrDomain selection_domain_;
  geometry::FaceLoopDirection direction_;

 public:
  LoopSelectionFieldInput(Field<bool> input_selection,
                          const AttrDomain selection_domain,
                          const geometry::FaceLoopDirection direction)
      : bke::MeshFieldInput(CPPType::get<bool>(), "Loop Selection"),
        input_selection_(std::move(input_selection)),
        selection_domain_(selection_domain),
        direction_(direction)
  {
  }

  GVArray get_varray_for_context(const Mesh &mesh,
                                 const AttrDomain domain,
                                 const IndexMask & /*mask*/) const final
  {
    const int64_t domain_size = mesh.attributes().domain_size(selection_domain_);
    if (domain_size == 0) {
      return {};
    }

    const bke::MeshFieldContext context{mesh, selection_domain_};
    fn::FieldEvaluator evaluator{context, domain_size};
    evaluator.add(input_selection_);
    evaluator.evaluate();
    const IndexMask input_selection = evaluator.get_evaluated_as_mask(0);
    if (input_selection.is_empty()) {
      return {};
    }

    VArray<bool> selection;
    if (input_selection.size() == domain_size) {
      selection = VArray<bool>::from_single(true, domain_size);
    }
    else if (selection_domain_ == AttrDomain::Edge) {
      selection = VArray<bool>::from_container(
          geometry::edge_loop_selection(mesh, input_selection));
    }
    else {
      selection = VArray<bool>::from_container(
          geometry::face_loop_selection(mesh, input_selection, direction_));
    }
    return mesh.attributes().adapt_domain<bool>(selection, selection_domain_, domain);
  }

  void foreach_recursive_field(FunctionRef<void(const GField &)> fn) const override
  {
    fn(input_selection_);
  }

  void hash_unique(UniqueHashBytes &hash, fn::FieldHashDeep &deep_hash_cache) const override
  {
    static constexpr int8_t id = 0;
    hash.add(&id);
    hash.add(deep_hash_cache.ensure(input_selection_));
    hash.add(selection_domain_);
    hash.add(uint8_t(direction_));
  }

  std::optional<AttrDomain> preferred_domain(const Mesh & /*mesh*/) const override
  {
    return selection_domain_;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<bool> input_selection = params.extract_input<Field<bool>>("Start Edges"_ustr);
  const AttrDomain domain = node_domain(params.node());
  const geometry::FaceLoopDirection direction = node_direction(params.node());
  params.set_output("Selection"_ustr,
                    Field<bool>::from_input<LoopSelectionFieldInput>(
                        std::move(input_selection), domain, direction));
}

static void node_rna(StructRNA *srna)
{
  static const EnumPropertyItem domain_items[] = {
      {int(AttrDomain::Edge), "EDGE", 0, "Edge", "Select edge loops from the input edges"},
      {int(AttrDomain::Face), "FACE", 0, "Face", "Select face loops from the input faces"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  static const EnumPropertyItem direction_items[] = {
      {int(geometry::FaceLoopDirection::U),
       "U",
       0,
       "U",
       "Continue horizontally through the seed face's side edges"},
      {int(geometry::FaceLoopDirection::V),
       "V",
       0,
       "V",
       "Continue vertically through the seed face's bottom and top edges"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  const EnumRNAAccessors domain_accessors(
      [](PointerRNA *ptr, PropertyRNA * /*prop*/) -> int {
        return int(node_domain(*static_cast<const bNode *>(ptr->data)));
      },
      [](PointerRNA *ptr, PropertyRNA * /*prop*/, const int value) {
        bNode &node = *static_cast<bNode *>(ptr->data);
        node.custom2 = int16_t(value == int(AttrDomain::Face) ? AttrDomain::Face :
                                                                AttrDomain::Edge);
      });
  RNA_def_node_enum(srna,
                    "domain",
                    "Domain",
                    "Mesh element type used for the input and output selections",
                    domain_items,
                    domain_accessors,
                    int(AttrDomain::Edge));

  const EnumRNAAccessors direction_accessors(
      [](PointerRNA *ptr, PropertyRNA * /*prop*/) -> int {
        return int(node_direction(*static_cast<const bNode *>(ptr->data)));
      },
      [](PointerRNA *ptr, PropertyRNA * /*prop*/, const int value) {
        bNode &node = *static_cast<bNode *>(ptr->data);
        node.custom1 = int16_t(value == int(geometry::FaceLoopDirection::V) ?
                                   geometry::FaceLoopDirection::V :
                                   geometry::FaceLoopDirection::U);
      });
  RNA_def_node_enum(srna,
                    "direction",
                    "Direction",
                    "Face-loop direction relative to the seed face's corner order",
                    direction_items,
                    direction_accessors,
                    int(geometry::FaceLoopDirection::U));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeEdgeLoopSelection"_ustr);
  ntype.ui_name = "Loop Selection";
  ntype.ui_description = "Expand selected edges or faces into loops like Alt-click in Edit Mode";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.default_width = bke::NodeWidth::_160;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_edge_loop_selection_cc
