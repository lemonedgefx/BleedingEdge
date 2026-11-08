# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Smoke test for importing Viewer selections from the active viewport mesh domain."""

import bpy


def window_region(area):
    return next(region for region in area.regions if region.type == "WINDOW")


def operator_override(area):
    return {
        "window": bpy.context.window,
        "screen": bpy.context.screen,
        "area": area,
        "region": window_region(area),
        "space_data": area.spaces.active,
    }


bpy.ops.mesh.primitive_cube_add()
source_object = bpy.context.object
source_object.name = "Viewer Selection Source"

node_group = bpy.data.node_groups.new("Viewer Selection Smoke", "GeometryNodeTree")
node_group.interface.new_socket(
    name="Geometry", in_out="INPUT", socket_type="NodeSocketGeometry"
)
node_group.interface.new_socket(
    name="Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry"
)
group_input = node_group.nodes.new("NodeGroupInput")
group_output = node_group.nodes.new("NodeGroupOutput")
viewer = node_group.nodes.new("GeometryNodeViewer")
viewer.viewer_items.new(socket_type="GEOMETRY", name="Geometry")
node_group.links.new(group_input.outputs["Geometry"], group_output.inputs["Geometry"])
node_group.links.new(group_input.outputs["Geometry"], viewer.inputs["Geometry"])

modifier = source_object.modifiers.new("Viewer Selection Smoke", "NODES")
modifier.node_group = node_group

node_area = next(area for area in bpy.context.screen.areas if area.type == "DOPESHEET_EDITOR")
view_area = next(area for area in bpy.context.screen.areas if area.type == "VIEW_3D")
node_area.type = "NODE_EDITOR"
node_area.ui_type = "GeometryNodeTree"
node_space = node_area.spaces.active
node_space.node_tree_sub_type = "MODIFIER"

for node in node_group.nodes:
    node.select = False
viewer.select = True
node_group.nodes.active = viewer

with bpy.context.temp_override(**operator_override(node_area)):
    assert bpy.context.space_data.edit_tree == node_group
    assert bpy.ops.node.activate_viewer() == {"FINISHED"}

bpy.context.view_layer.update()
depsgraph = bpy.context.evaluated_depsgraph_get()
evaluated_object = source_object.evaluated_get(depsgraph)
evaluated_object.to_mesh()
evaluated_object.to_mesh_clear()

with bpy.context.temp_override(**operator_override(node_area)):
    assert bpy.ops.node.geometry_viewer_editing_toggle() == {"FINISHED"}

assert bpy.context.mode == "EDIT_MESH"
temporary_object = bpy.context.view_layer.objects.active
assert temporary_object is not None
assert temporary_object.name.startswith("Viewer Editing")

with bpy.context.temp_override(**operator_override(view_area)):
    bpy.ops.mesh.select_all(action="DESELECT")
    bpy.ops.mesh.select_mode(type="EDGE")
    bpy.ops.mesh.select_all(action="SELECT")

assert bpy.context.scene.tool_settings.mesh_select_mode[:] == (False, True, False)

with bpy.context.temp_override(**operator_override(node_area)):
    assert bpy.ops.node.geometry_viewer_import_selection() == {"FINISHED"}

selection_nodes = [
    node for node in node_group.nodes if node.bl_idname == "GeometryNodeSelectByIndex"
]
assert len(selection_nodes) == 1
selection_node = selection_nodes[0]
assert selection_node.domain == "EDGE"
assert selection_node.mode == "CAPTURED" and selection_node.selected_count == 12
assert selection_node.inputs[0].default_value == "0-11"
assert bpy.context.mode == "OBJECT"

node_group.nodes.active = viewer
with bpy.context.temp_override(**operator_override(node_area)):
    assert bpy.ops.node.geometry_viewer_editing_toggle() == {"FINISHED"}

assert bpy.context.mode == "EDIT_MESH"
assert bpy.context.scene.tool_settings.mesh_select_mode[:] == (False, True, False)

with bpy.context.temp_override(**operator_override(node_area)):
    assert bpy.ops.node.geometry_viewer_editing_toggle() == {"FINISHED"}

assert bpy.context.mode == "OBJECT"
assert bpy.context.view_layer.objects.active == source_object
print("GEOMETRY_VIEWER_SELECTION_SMOKE_OK")
