# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Real Shift+D, connected face selection, repeat/cancel, undo and file lifecycle.

Requires the new adapter. Run --factory-startup --enable-event-simulate --python this_file.py.
Require GEOMETRY_VIEWER_DUPLICATE_UI_SMOKE_OK and a clean process exit.
"""

import os
import tempfile
import traceback

import bmesh
import bpy


NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
SOURCE = TREE = 'Viewer Duplicate Modal Test'
test_dir = tempfile.TemporaryDirectory(prefix='viewer-duplicate-ui-')
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


def event(kind, value='PRESS', dx=0, shift=False, unicode=''):
    region = next(r for r in VIEW_AREA.regions if r.type == 'WINDOW')
    args = dict(type=kind, value=value, x=region.x + region.width // 2 + dx,
                y=region.y + region.height // 2, shift=shift)
    if unicode:
        args['unicode'] = unicode
    bpy.context.window.event_simulate(**args)


def begin():
    bpy.context.view_layer.objects.active = source()
    source().select_set(True)
    viewer = next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeViewer')
    tree().nodes.active = viewer
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.node.activate_viewer()
        bpy.context.view_layer.update()
        assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
    with bpy.context.temp_override(**override(VIEW_AREA)):
        bpy.ops.mesh.select_all(action='DESELECT')
        bpy.ops.mesh.select_mode(type='FACE')
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    bm.faces.ensure_lookup_table()
    bm.faces[0].select_set(True)
    bm.faces[1].select_set(True)
    bm.faces.active = bm.faces[1]
    bm.select_history.add(bm.faces[1])
    bm.select_flush_mode()
    assert len({v for f in bm.faces if f.select for v in f.verts}) == 6
    bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)


def duplicate_key():
    event('MOUSEMOVE', 'NOTHING')
    event('D', shift=True)
    event('D', 'RELEASE', shift=True)


def assert_clean(count):
    assert bpy.context.mode == 'OBJECT' and bpy.context.object == source()
    assert len(tree().nodes) == count, len(tree().nodes)
    assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)
    assert not any(n.bl_idname == 'GeometryNodeStoreNamedAttribute' for n in tree().nodes)


def tick():
    global stage
    try:
        if stage == -2:
            event('ESC')
        elif stage == -1:
            begin()
        elif stage == 0:
            duplicate_key()
        elif stage == 1:
            assert len(tree().nodes) == 8 and bpy.context.mode == 'EDIT_MESH'
            bm = bmesh.from_edit_mesh(bpy.context.object.data)
            assert (len(bm.verts), len(bm.faces)) == (14, 8), 'Adjacent copies must share their boundary'
            assert len([f for f in bm.faces if f.select]) == 2
            event('MOUSEMOVE', 'NOTHING', dx=24)
        elif stage == 2:
            assert len(tree().nodes) == 8
            event('MOUSEMOVE', 'NOTHING', dx=45)
        elif stage == 3:
            assert len(tree().nodes) == 8
            event('ESC')
        elif stage == 4:
            assert len(tree().nodes) == 7 and bpy.context.mode == 'EDIT_MESH'
            move = next(n for n in tree().nodes if n.label == 'Duplicate Offset')
            assert not any(move.inputs['Offset'].default_value)
            duplicate_key()
        elif stage == 5:
            assert len(tree().nodes) == 12
            event('X')
            event('X', 'RELEASE')
            event('TWO', unicode='2')
            event('TWO', 'RELEASE')
        elif stage == 6:
            assert len(tree().nodes) == 12
            event('RET')
        elif stage == 7:
            assert len(tree().nodes) == 11 and bpy.context.mode == 'EDIT_MESH'
            offsets = [n.inputs['Offset'].default_value for n in tree().nodes if n.label == 'Duplicate Offset']
            assert any(abs(v[0] - 2) < 1e-5 and abs(v[1]) < 1e-5 and abs(v[2]) < 1e-5 for v in offsets), offsets
            bm = bmesh.from_edit_mesh(bpy.context.object.data)
            assert (len(bm.verts), len(bm.faces)) == (20, 10)
            assert len([f for f in bm.faces if f.select]) == 2
            assert not any(n.bl_idname == 'GeometryNodeStoreNamedAttribute' for n in tree().nodes)
            duplicate_key()
        elif stage == 8:
            assert len(tree().nodes) == 16
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.node.geometry_viewer_edit_transaction(action='ABORT_DUPLICATION') == {'FINISHED'}
            assert_clean(11)
        elif stage == 9:
            begin()
            duplicate_key()
        elif stage == 10:
            assert len(tree().nodes) == 16
            event('ESC', shift=True)
        elif stage == 11:
            assert_clean(11)
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert bpy.ops.ed.undo() == {'FINISHED'}
            assert_clean(7)
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert bpy.ops.ed.undo() == {'FINISHED'}
            assert_clean(3)  # Esc's zero-offset copy owns exactly one undo step.
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert bpy.ops.ed.redo() == {'FINISHED'}
            assert_clean(7)
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert bpy.ops.ed.redo() == {'FINISHED'}
            assert_clean(11)
        elif stage == 12:
            begin()
            duplicate_key()
        elif stage == 13:
            assert len(tree().nodes) == 16
            bpy.context.scene.frame_set(bpy.context.scene.frame_current + 1)
        elif stage == 14:
            assert_clean(11)
            begin()
            duplicate_key()
        elif stage == 15:
            assert len(tree().nodes) == 16
            with bpy.context.temp_override(**override(NODE_AREA)):
                bpy.ops.wm.save_as_mainfile(filepath=os.path.join(test_dir.name, 'duplicate.blend'))
            assert_clean(11)
            print('GEOMETRY_VIEWER_DUPLICATE_UI_SMOKE_OK', flush=True)
            bpy.ops.wm.quit_blender()
            return None
        stage += 1
        return 0.5
    except Exception:
        traceback.print_exc()
        print('GEOMETRY_VIEWER_DUPLICATE_UI_SMOKE_FAILED', stage, flush=True)
        bpy.ops.wm.quit_blender()
        return None


bpy.ops.mesh.primitive_cube_add()
obj = bpy.context.object
obj.name = SOURCE
group = bpy.data.node_groups.new(TREE, 'GeometryNodeTree')
group.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
input_node, output_node = group.nodes.new('NodeGroupInput'), group.nodes.new('NodeGroupOutput')
viewer = group.nodes.new('GeometryNodeViewer')
viewer.viewer_items.new(socket_type='GEOMETRY', name='Geometry')
group.links.new(input_node.outputs['Geometry'], output_node.inputs['Geometry'])
group.links.new(input_node.outputs['Geometry'], viewer.inputs['Geometry'])
obj.modifiers.new(TREE, 'NODES').node_group = group
NODE_AREA.type = 'NODE_EDITOR'
NODE_AREA.ui_type = 'GeometryNodeTree'
NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
obj = group = input_node = output_node = viewer = None
with bpy.context.temp_override(**override(NODE_AREA)):
    bpy.ops.ed.undo_push(message='Viewer Duplicate Modal Baseline')
bpy.app.timers.register(tick, first_interval=1)
