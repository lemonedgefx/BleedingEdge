# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Real M dialog confirm/cancel, merge continuation, no-op, undo and file lifecycle.

Requires the new adapter. Run --factory-startup --enable-event-simulate --python this_file.py.
Require GEOMETRY_VIEWER_MERGE_UI_SMOKE_OK and a clean process exit.
"""

import os
import tempfile
import traceback

import bmesh
import bpy


NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
SOURCE = TREE = 'Viewer Merge Dialog Test'
test_dir = tempfile.TemporaryDirectory(prefix='viewer-merge-ui-')
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.filepaths.temporary_directory = test_dir.name
stage = -2


def override(area):
    return dict(window=bpy.context.window, screen=bpy.context.screen, area=area,
                region=next(r for r in area.regions if r.type == 'WINDOW'))


def tree():
    return bpy.data.node_groups[TREE]


def source():
    return bpy.data.objects[SOURCE]


def event(kind, value='PRESS'):
    region = next(r for r in VIEW_AREA.regions if r.type == 'WINDOW')
    bpy.context.window.event_simulate(type=kind, value=value, x=region.x + region.width // 2,
                                     y=region.y + region.height // 2)


def begin():
    bpy.context.view_layer.objects.active = source()
    source().select_set(True)
    viewer = next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeViewer')
    tree().nodes.active = viewer
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.node.activate_viewer()
        bpy.context.view_layer.update()
        assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
    select()


def select():
    with bpy.context.temp_override(**override(VIEW_AREA)):
        bpy.ops.mesh.select_all(action='DESELECT')
        bpy.ops.mesh.select_mode(type='VERT')
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    bm.verts.ensure_lookup_table()
    for index in (0, 1):
        bm.verts[index].select_set(True)
    bm.select_history.add(bm.verts[1])
    bm.select_flush_mode()
    bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)


def merge_key():
    event('MOUSEMOVE', 'NOTHING')
    event('M')
    event('M', 'RELEASE')


def assert_clean(count):
    assert bpy.context.mode == 'OBJECT' and bpy.context.object == source()
    assert len(tree().nodes) == count
    assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)


def tick():
    global stage
    try:
        if stage == -2:
            event('ESC')
        elif stage == -1:
            begin()
        elif stage == 0:
            merge_key()
        elif stage == 1:
            assert len(tree().nodes) == 3 and bpy.context.mode == 'EDIT_MESH'
            event('ESC')
        elif stage == 2:
            assert len(tree().nodes) == 3 and bpy.context.mode == 'EDIT_MESH'
            bm = bmesh.from_edit_mesh(bpy.context.object.data)
            assert len([v for v in bm.verts if v.select]) == 2 and len(bm.verts) == 4
            merge_key()
        elif stage == 3:
            assert len(tree().nodes) == 3
            event('RET')
        elif stage == 4:
            assert len(tree().nodes) == 5 and bpy.context.mode == 'EDIT_MESH'
            operation = next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeMergeByDistance')
            assert operation.inputs['Mode'].default_value == 'All'
            bm = bmesh.from_edit_mesh(bpy.context.object.data)
            assert len(bm.verts) == 3 and not any(v.select for v in bm.verts)
            assert not bm.select_history
            assert len(source().data.vertices) == 4
            select()  # Remaining points are far apart: another M confirmation is a no-op.
            merge_key()
        elif stage == 5:
            assert len(tree().nodes) == 5
            event('RET')
        elif stage == 6:
            assert len(tree().nodes) == 5 and bpy.context.mode == 'EDIT_MESH'
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert bpy.ops.ed.undo() == {'FINISHED'}
            assert_clean(3)  # Cancelling/no-op did not add intermediate undo steps.
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert bpy.ops.ed.redo() == {'FINISHED'}
            assert_clean(5)
        elif stage == 7:
            begin()
            with bpy.context.temp_override(**override(NODE_AREA)):
                bpy.ops.wm.save_as_mainfile(filepath=os.path.join(test_dir.name, 'merge.blend'))
            assert_clean(5)
            print('GEOMETRY_VIEWER_MERGE_UI_SMOKE_OK', flush=True)
            bpy.ops.wm.quit_blender()
            return None
        stage += 1
        return 0.5
    except Exception:
        traceback.print_exc()
        print('GEOMETRY_VIEWER_MERGE_UI_SMOKE_FAILED', stage, flush=True)
        bpy.ops.wm.quit_blender()
        return None


mesh = bpy.data.meshes.new(SOURCE)
mesh.from_pydata([(0, 0, 0), (0.0005, 0, 0), (1, 0, 0), (2, 0, 0)], [(0, 1), (1, 2), (2, 3)], [])
obj = bpy.data.objects.new(SOURCE, mesh)
bpy.context.collection.objects.link(obj)
bpy.context.view_layer.objects.active = obj
obj.select_set(True)
group = bpy.data.node_groups.new(TREE, 'GeometryNodeTree')
group.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
input_node, output = group.nodes.new('NodeGroupInput'), group.nodes.new('NodeGroupOutput')
viewer = group.nodes.new('GeometryNodeViewer')
viewer.viewer_items.new(socket_type='GEOMETRY', name='Geometry')
group.links.new(input_node.outputs['Geometry'], output.inputs['Geometry'])
group.links.new(input_node.outputs['Geometry'], viewer.inputs['Geometry'])
obj.modifiers.new(TREE, 'NODES').node_group = group
NODE_AREA.type = 'NODE_EDITOR'
NODE_AREA.ui_type = 'GeometryNodeTree'
NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
obj = mesh = group = input_node = output = viewer = None
with bpy.context.temp_override(**override(NODE_AREA)):
    bpy.ops.ed.undo_push(message='Viewer Merge Dialog Baseline')
bpy.app.timers.register(tick, first_interval=1)
