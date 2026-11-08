# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Generated Viewer edits follow their input horizontally, including frame parenting.

Run --background --factory-startup --python-exit-code 1 --python this_file.py.
Require GEOMETRY_VIEWER_EDIT_LAYOUT_SMOKE_OK and a clean process exit.
"""

import bmesh
import bpy


NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
checks = 0


def override(area):
    return dict(window=bpy.context.window, screen=bpy.context.screen, area=area,
                region=next(r for r in area.regions if r.type == 'WINDOW'))


def refresh():
    with bpy.context.temp_override(**override(NODE_AREA)):
        assert bpy.ops.node.geometry_viewer_edit_transaction(action='VALIDATE') == {'FINISHED'}


def result(group, viewer):
    return next(link.from_node for link in group.links
                if link.to_node == viewer and link.to_socket.type == 'GEOMETRY')


def select_face():
    with bpy.context.temp_override(**override(VIEW_AREA)):
        bpy.ops.mesh.select_all(action='DESELECT')
        bpy.ops.mesh.select_mode(type='FACE')
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    bm.faces.ensure_lookup_table()
    face = bm.faces[0]
    face.select_set(True)
    bm.faces.active = face
    bm.select_history.add(face)
    bm.select_flush_mode()
    bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)


def assert_stage(group, viewer, previous, added):
    global checks
    operation = result(group, viewer)
    if operation.bl_idname == 'GeometryNodeGroup':
        assert operation.label == 'Set Rotation' and added == [operation]
        assert any(n.bl_idname == 'GeometryNodeSelectByIndex' for n in operation.node_tree.nodes)
        assert operation.parent == viewer.parent
        assert operation.location_absolute.x >= previous.location_absolute.x + previous.width + 40
        assert viewer.location_absolute.x >= operation.location_absolute.x + operation.width + 40
        assert operation.location_absolute.y == previous.location_absolute.y
        checks += 1
        return operation
    capture = next(link.from_node for link in group.links
                   if link.to_node == operation and link.to_socket.name == 'Selection')
    assert capture.bl_idname == 'GeometryNodeSelectByIndex' and capture.mode == 'CAPTURED'
    assert capture.location_absolute.x >= previous.location_absolute.x + previous.width + 40
    ordered = sorted(added, key=lambda node: node.location_absolute.x)
    assert ordered[0] == capture and ordered[-1] == operation
    for left, right in zip(ordered, ordered[1:] + [viewer]):
        assert right.location_absolute.x >= left.location_absolute.x + left.width + 40, (
            left.name, tuple(left.location_absolute), right.name, tuple(right.location_absolute))
    for node in added:
        assert node.parent == viewer.parent
        assert node.location_absolute.y == previous.location_absolute.y
    checks += 1
    return operation


for framed in (False, True):
    bpy.ops.mesh.primitive_cube_add()
    source = bpy.context.object
    group = bpy.data.node_groups.new('Viewer Edit Layout', 'GeometryNodeTree')
    group.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
    group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
    input_node = group.nodes.new('NodeGroupInput')
    output = group.nodes.new('NodeGroupOutput')
    viewer = group.nodes.new('GeometryNodeViewer')
    viewer.viewer_items.new(socket_type='GEOMETRY', name='Geometry')
    input_node.location = (0, 100)
    output.location = (900, -700)
    if framed:
        frame = group.nodes.new('NodeFrame')
        frame.location = (200, 300)
        viewer.parent = frame
    viewer.location_absolute = (450, 100)
    group.links.new(input_node.outputs['Geometry'], output.inputs['Geometry'])
    group.links.new(input_node.outputs['Geometry'], viewer.inputs['Geometry'])
    source.modifiers.new('Viewer Edit Layout', 'NODES').node_group = group
    NODE_AREA.type = 'DOPESHEET_EDITOR'  # Refresh the editor's modifier context between fixtures.
    NODE_AREA.type = 'NODE_EDITOR'
    NODE_AREA.ui_type = 'GeometryNodeTree'
    NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
    group.nodes.active = viewer
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.node.activate_viewer()
        bpy.context.view_layer.update()
        assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
    select_face()

    operations = []
    # Two moves are the minimal repro: the old implementation places both at the same location.
    for i in range(2):
        with bpy.context.temp_override(**override(VIEW_AREA)):
            assert bpy.ops.transform.translate(value=(0.1, 0, 0), use_proportional_edit=False,
                                               mirror=False) == {'FINISHED'}
        operations.append(result(group, viewer))
        refresh()
    left, right = operations
    assert right.location_absolute.x >= left.location_absolute.x + left.width + 40, (
        'Consecutive edits overlap', tuple(left.location_absolute), tuple(right.location_absolute))
    for previous, operation in zip([input_node, left], operations):
        capture = next(link.from_node for link in group.links
                       if link.to_node == operation and link.to_socket.name == 'Selection')
        next_node = right if operation == left else viewer
        assert capture.location_absolute.x >= previous.location_absolute.x + previous.width + 40
        assert operation.location_absolute.x >= capture.location_absolute.x + capture.width + 40
        assert next_node.location_absolute.x >= operation.location_absolute.x + operation.width + 40
        assert capture.location_absolute.y == operation.location_absolute.y == previous.location_absolute.y
        assert capture.parent == operation.parent == viewer.parent
        checks += 1

    for action in ('SCALE', 'ROTATE', 'CUT', 'DISSOLVE', 'EXTRUDE', 'DELETE'):
        previous = result(group, viewer)
        before = set(group.nodes)
        if action not in {'CUT', 'DISSOLVE'}:
            select_face()
        with bpy.context.temp_override(**override(VIEW_AREA)):
            if action == 'SCALE':
                status = bpy.ops.transform.resize(value=(1.2,) * 3, use_proportional_edit=False, mirror=False)
            elif action == 'ROTATE':
                status = bpy.ops.transform.rotate(value=0.2, orient_type='GLOBAL', orient_axis='Z',
                                                  constraint_axis=(False, False, True),
                                                  use_proportional_edit=False, mirror=False)
            elif action == 'CUT':
                status = bpy.ops.node.geometry_viewer_edit_loop_cut(edge_index=0, cuts=2)
            elif action == 'DISSOLVE':
                status = bpy.ops.node.geometry_viewer_edit_delete(type='DISSOLVE_EDGE')
            elif action == 'EXTRUDE':
                status = bpy.ops.node.geometry_viewer_edit_extrude(offset=(0, 0, 0.2))
            else:
                status = bpy.ops.node.geometry_viewer_edit_delete(type='ONLY_FACE')
        assert status == {'FINISHED'}, (action, status)
        added = [node for node in group.nodes if node not in before]
        assert_stage(group, viewer, previous, added)
        refresh()

    previous = result(group, viewer)
    select_face()
    before = set(group.nodes)
    original_location = viewer.location_absolute.copy()
    with bpy.context.temp_override(**override(VIEW_AREA)):
        bpy.ops.transform.translate(value=(0, 0, 0), use_proportional_edit=False, mirror=False)
    assert viewer.location_absolute == original_location
    assert set(group.nodes) == before
    assert result(group, viewer) == previous
    checks += 1
    # A manually repositioned Viewer belongs to the user even for a discarded no-op gesture.
    viewer.location_absolute = (viewer.location_absolute.x + 120, viewer.location_absolute.y - 60)
    manual_location = viewer.location_absolute.copy()
    with bpy.context.temp_override(**override(VIEW_AREA)):
        bpy.ops.transform.resize(value=(1, 1, 1), use_proportional_edit=False, mirror=False)
    assert viewer.location_absolute == manual_location
    assert set(group.nodes) == before and result(group, viewer) == previous
    checks += 1
    assert output.location[:] == (900, -700)
    assert next(link.from_node for link in group.links if link.to_node == output) == input_node
    with bpy.context.temp_override(**override(NODE_AREA)):
        assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}

print('GEOMETRY_VIEWER_EDIT_LAYOUT_SMOKE_OK', checks)
