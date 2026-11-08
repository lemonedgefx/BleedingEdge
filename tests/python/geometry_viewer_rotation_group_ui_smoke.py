# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""New adapter only: real R/Z/45, compact group, editable angles, cancel and undo.

Requires a future build containing Set Rotation authoring and the Select Elements rename.
Run --factory-startup --enable-event-simulate --python this_file.py.
Require GEOMETRY_VIEWER_ROTATION_GROUP_UI_SMOKE_OK and a clean exit.
"""

import math
import os
import tempfile
import traceback

import bmesh
import bpy
from mathutils import Matrix, Vector


NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
SOURCE = TREE = 'Viewer Set Rotation UI Test'
test_dir = tempfile.TemporaryDirectory(prefix='viewer-set-rotation-ui-')
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.filepaths.temporary_directory = test_dir.name
stage = -2
original = []
selected = []


def override(area):
    return dict(window=bpy.context.window, screen=bpy.context.screen, area=area,
                region=next(r for r in area.regions if r.type == 'WINDOW'))


def source():
    return bpy.data.objects[SOURCE]


def tree():
    return bpy.data.node_groups[TREE]


def viewer():
    return next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeViewer')


def result_node():
    return next(l.from_node for l in tree().links if l.to_node == viewer()
                and l.to_socket.type == 'GEOMETRY')


def event(kind, value='PRESS', unicode=''):
    region = next(r for r in VIEW_AREA.regions if r.type == 'WINDOW')
    args = dict(type=kind, value=value, x=region.x + region.width // 2,
                y=region.y + region.height // 2)
    if unicode:
        args['unicode'] = unicode
    bpy.context.window.event_simulate(**args)


def begin():
    global selected
    bpy.context.view_layer.objects.active = source()
    source().select_set(True)
    tree().nodes.active = viewer()
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.node.activate_viewer()
        bpy.context.view_layer.update()
        assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
    with bpy.context.temp_override(**override(VIEW_AREA)):
        bpy.ops.mesh.select_all(action='DESELECT')
        bpy.ops.mesh.select_mode(type='FACE')
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    bm.faces.ensure_lookup_table()
    bm.verts.index_update()
    bm.faces[0].select_set(True)
    bm.select_history.add(bm.faces[0])
    bm.faces.active = bm.faces[0]
    bm.select_flush_mode()
    selected = [v.index for v in bm.verts if v.select]
    bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)


def assert_clean(count):
    assert bpy.context.mode == 'OBJECT' and bpy.context.object == source()
    assert len(tree().nodes) == count
    assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)


def evaluate_angle(angle):
    group = result_node()
    output = next(n for n in tree().nodes if n.bl_idname == 'NodeGroupOutput')
    input_node = next(n for n in tree().nodes if n.bl_idname == 'NodeGroupInput')
    tree().links.new(group.outputs['Mesh'], output.inputs['Geometry'])
    try:
        bpy.context.view_layer.update()
        obj = source().evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = obj.to_mesh()
        world = source().matrix_world.to_3x3()
        linear = world.inverted() @ Matrix.Rotation(angle, 3, 'Z') @ world
        pivot = sum((original[i] for i in selected), Vector()) / len(selected)
        for i, vertex in enumerate(mesh.vertices):
            expected = pivot + linear @ (original[i] - pivot) if i in selected else original[i]
            assert (vertex.co - expected).length < 8e-5, (i, vertex.co, expected)
        obj.to_mesh_clear()
    finally:
        tree().links.new(input_node.outputs['Geometry'], output.inputs['Geometry'])


def tick():
    global stage
    try:
        if stage == -2:
            event('ESC')
        elif stage == -1:
            begin()
        elif stage == 0:
            event('MOUSEMOVE', 'NOTHING')
            event('R')
            event('R', 'RELEASE')
        elif stage == 1:
            event('Z')
            event('FOUR', unicode='4')
            event('FIVE', unicode='5')
        elif stage == 2:
            assert len(tree().nodes) == 10  # The unconfirmed safety recipe is still ungrouped.
            assert not any(n.bl_idname == 'GeometryNodeGroup' for n in tree().nodes)
            event('RET')
        elif stage == 3:
            assert len(tree().nodes) == 4 and bpy.context.mode == 'EDIT_MESH'
            group = result_node()
            assert group.bl_idname == 'GeometryNodeGroup' and group.label == 'Set Rotation'
            assert [s.name for s in group.inputs] == ['Mesh', 'Rotation']
            assert [s.name for s in group.outputs] == ['Mesh']
            assert abs(group.inputs['Rotation'].default_value[2] - math.pi / 4) < 1e-5
            capture = next(n for n in group.node_tree.nodes if n.bl_idname == 'GeometryNodeSelectByIndex')
            assert capture.bl_rna.name == 'Select Elements' and capture.selected_count == 1
            bm = bmesh.from_edit_mesh(bpy.context.object.data)
            bm.verts.index_update()
            assert [v.index for v in bm.verts if v.select] == selected
            bm = None
            assert group.location_absolute.x + group.width < viewer().location_absolute.x
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert bpy.ops.ed.undo() == {'FINISHED'}
            assert_clean(3)
            assert not any(n.name.startswith('Set Rotation') for n in bpy.data.node_groups)
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert bpy.ops.ed.redo() == {'FINISHED'}
            assert_clean(4)
            evaluate_angle(math.pi / 4)
            result_node().inputs['Rotation'].default_value = (0, 0, math.pi / 2)
            evaluate_angle(math.pi / 2)
        elif stage == 4:
            begin()
            event('MOUSEMOVE', 'NOTHING')
            event('R')
        elif stage == 5:
            assert len(tree().nodes) == 11
            event('ESC')
        elif stage == 6:
            assert len(tree().nodes) == 4 and bpy.context.mode == 'EDIT_MESH'
            assert len([n for n in bpy.data.node_groups if n.name.startswith('Set Rotation')]) == 1
            event('R')
        elif stage == 7:
            event('Z')
            event('ZERO', unicode='0')
            event('RET')
        elif stage == 8:
            assert len(tree().nodes) == 4 and bpy.context.mode == 'EDIT_MESH'
            with bpy.context.temp_override(**override(NODE_AREA)):
                bpy.ops.wm.save_as_mainfile(filepath=os.path.join(test_dir.name, 'set-rotation.blend'))
            assert_clean(4)
            evaluate_angle(math.pi / 2)
            print('GEOMETRY_VIEWER_ROTATION_GROUP_UI_SMOKE_OK', flush=True)
            bpy.ops.wm.quit_blender()
            return None
        stage += 1
        return 0.5
    except Exception:
        traceback.print_exc()
        print('GEOMETRY_VIEWER_ROTATION_GROUP_UI_SMOKE_FAILED', stage, flush=True)
        bpy.ops.wm.quit_blender()
        return None


bpy.ops.mesh.primitive_cube_add()
obj = bpy.context.object
obj.name, obj.rotation_euler, obj.scale = SOURCE, (0.2, -0.4, 0.3), (1.5, 0.7, 2)
original = [v.co.copy() for v in obj.data.vertices]
group = bpy.data.node_groups.new(TREE, 'GeometryNodeTree')
group.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
input_node, output = group.nodes.new('NodeGroupInput'), group.nodes.new('NodeGroupOutput')
node = group.nodes.new('GeometryNodeViewer')
node.viewer_items.new(socket_type='GEOMETRY', name='Geometry')
group.links.new(input_node.outputs['Geometry'], output.inputs['Geometry'])
group.links.new(input_node.outputs['Geometry'], node.inputs['Geometry'])
obj.modifiers.new(TREE, 'NODES').node_group = group
NODE_AREA.type = 'NODE_EDITOR'
NODE_AREA.ui_type = 'GeometryNodeTree'
NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
obj = group = input_node = output = node = None
with bpy.context.temp_override(**override(NODE_AREA)):
    bpy.ops.ed.undo_push(message='Viewer Set Rotation UI Baseline')
bpy.app.timers.register(tick, first_interval=1)
