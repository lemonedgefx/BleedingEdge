/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <fmt/format.h>

#include "DNA_mesh_types.h"

#include "BLO_read_write.hh"

#include "NOD_geometry_edit.hh"
#include "NOD_rna_define.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_captured_selection_cc {

NODE_STORAGE_FUNCS(NodeGeometryCapturedSelection)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry"_ustr)
      .description("The mesh input stage at which this selection was captured");
  b.add_output<decl::Bool>("Selection"_ustr)
      .structure_type(StructureType::Field)
      .propagate_references()
      .description("Captured elements in the picked domain; false if the input stage changed");
  b.add_output<decl::Bool>("Points"_ustr)
      .structure_type(StructureType::Field)
      .propagate_references()
      .description("Explicit affected vertices, including vertices of picked edges or faces");
  b.add_output<decl::Bool>("Valid"_ustr)
      .description("Whether the connected mesh still matches the captured input stage");
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->storage = MEM_new<NodeGeometryCapturedSelection>(__func__);
}

static void node_free(bNode *node)
{
  auto &storage = node_storage(*node);
  MEM_SAFE_DELETE(storage.indices);
  MEM_SAFE_DELETE(storage.point_indices);
  MEM_delete(&storage);
}

static void node_copy(bNodeTree * /*tree*/, bNode *dst, const bNode *src)
{
  const auto &storage = node_storage(*src);
  dst->storage = MEM_new<NodeGeometryCapturedSelection>(__func__);
  geometry_edit::capture_copy(node_storage(*dst), storage);
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  const auto &storage = node_storage(*ptr->data_as<bNode>());
  const char *domain = storage.domain == int8_t(AttrDomain::Edge) ? IFACE_("Edges") :
                       storage.domain == int8_t(AttrDomain::Face) ? IFACE_("Faces") :
                                                                    IFACE_("Points");
  layout.label(fmt::format("{}: {}", domain, storage.indices_num), ICON_NONE);
  layout.label(IFACE_("Locked to captured input"), ICON_LOCKED);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  geometry_edit::capture_execute(params);
}

static void node_blend_write(const bNodeTree & /*tree*/, const bNode &node, BlendWriter &writer)
{
  const auto &storage = node_storage(node);
  writer.write_int32_array(storage.indices_num, storage.indices);
  writer.write_int32_array(storage.point_indices_num, storage.point_indices);
}

static void node_blend_read(bNodeTree & /*tree*/, bNode &node, BlendDataReader &reader)
{
  auto &storage = node_storage(node);
  BLO_read_array_and_validate_size(&reader, &storage.indices, &storage.indices_num);
  BLO_read_array_and_validate_size(&reader, &storage.point_indices, &storage.point_indices_num);
}

static void node_rna(StructRNA *srna)
{
  PropertyRNA *prop = RNA_def_property(srna, "domain", PROP_ENUM, PROP_NONE);
  static const EnumPropertyItem domains[] = {
      {int(AttrDomain::Point), "POINT", 0, "Point", "Captured vertices"},
      {int(AttrDomain::Edge), "EDGE", 0, "Edge", "Captured edges"},
      {int(AttrDomain::Face), "FACE", 0, "Face", "Captured faces"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  RNA_def_property_enum_items(prop, domains);
  RNA_def_property_enum_funcs_runtime(
      prop,
      [](PointerRNA *ptr, PropertyRNA * /*prop*/) {
        return int(node_storage(*ptr->data_as<bNode>()).domain);
      },
      nullptr,
      nullptr,
      nullptr,
      nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  prop = RNA_def_property(srna, "selected_count", PROP_INT, PROP_NONE);
  RNA_def_property_int_funcs_runtime(
      prop,
      [](PointerRNA *ptr, PropertyRNA * /*prop*/) {
        return node_storage(*ptr->data_as<bNode>()).indices_num;
      },
      nullptr,
      nullptr,
      nullptr,
      nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeCapturedSelection"_ustr);
  ntype.ui_name = "Captured Selection";
  ntype.ui_description = "An immutable viewport selection locked to its mesh input stage";
  /* Retain file/RNA compatibility, but new workflows use Select by Index. */
  ntype.add_ui_poll = [](const bContext * /*C*/) { return false; };
  ntype.nclass = NODE_CLASS_INPUT;
  bke::node_type_storage(ntype, "NodeGeometryCapturedSelection", node_free, node_copy);
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.blend_write_storage_content = node_blend_write;
  ntype.blend_data_read_storage_content = node_blend_read;
  bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_captured_selection_cc
