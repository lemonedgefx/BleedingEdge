# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Real Ctrl+R, hover, wheel/numeric input and both cancellation stages. Requires Phase 3.

Run --factory-startup --enable-event-simulate --python this_file.py.
Require GEOMETRY_VIEWER_LOOP_CUT_UI_SMOKE_OK and a clean process exit.
"""

import os
import tempfile
import traceback

import bmesh
import bpy
from bpy_extras.view3d_utils import location_3d_to_region_2d
from mathutils import Quaternion, Vector


NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
SOURCE = TREE = 'Viewer Loop Cut Modal Test'
test_dir = tempfile.TemporaryDirectory(prefix='viewer-loop-cut-ui-')
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.filepaths.temporary_directory = test_dir.name
stage = -2
pending_name = None
pending_pointer = None


def override(area):
    return dict(window=bpy.context.window, screen=bpy.context.screen, area=area,
                region=next(r for r in area.regions if r.type == 'WINDOW'))


def tree():
    return bpy.data.node_groups[TREE]


def source():
    return bpy.data.objects[SOURCE]


def cut():
    return tree().nodes[pending_name]


def remember_cut():
    global pending_name, pending_pointer
    store = next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeStoreNamedAttribute')
    node = next(l.from_node for l in tree().links
                if l.to_node == store and l.to_socket.name == 'Geometry')
    pending_name, pending_pointer = node.name, node.as_pointer()


def event(kind, value='PRESS', dy=0, ctrl=False, shift=False, unicode=''):
    region = next(r for r in VIEW_AREA.regions if r.type == 'WINDOW')
    point = location_3d_to_region_2d(region, VIEW_AREA.spaces.active.region_3d, Vector((1, -0.7, 0)))
    assert point is not None
    args = dict(type=kind, value=value,
                x=region.x + round(point.x), y=region.y + round(point.y) + dy,
                ctrl=ctrl, shift=shift)
    if unicode:
        args['unicode'] = unicode
    bpy.context.window.event_simulate(**args)


def ctrl_r():
    event('MOUSEMOVE', 'NOTHING')
    event('R', ctrl=True)
    event('R', 'RELEASE', ctrl=True)


def begin():
    bpy.context.view_layer.objects.active = source()
    source().select_set(True)
    node = next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeViewer')
    tree().nodes.active = node
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.node.activate_viewer()
        bpy.context.view_layer.update()
        assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
    with bpy.context.temp_override(**override(VIEW_AREA)):
        bpy.ops.mesh.select_all(action='DESELECT')
        bpy.ops.mesh.select_mode(type='VERT')
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    bm.verts.ensure_lookup_table()
    bm.verts[0].select_set(True)  # Seed hover must not use this existing vertex selection.
    bm.select_history.add(bm.verts[0])
    bm.select_flush_mode()
    bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)


def assert_clean(count):
    assert bpy.context.mode == 'OBJECT' and bpy.context.object == source()
    assert len(tree().nodes) == count
    assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)
    assert not any(n.bl_idname == 'GeometryNodeStoreNamedAttribute' for n in tree().nodes)


def tick():
    global stage
    try:
        if stage == -2:
            event('ESC')  # Dismiss the startup splash before testing viewport keymaps.
        elif stage == -1:
            begin()
        elif stage == 0:
            ctrl_r()
        elif stage == 1:
            assert len(tree().nodes) == 6
            remember_cut()
            assert cut().inputs['Cuts'].default_value == 1
            bm = bmesh.from_edit_mesh(bpy.context.object.data)
            assert len(bm.verts) == 4 and len(bm.faces) == 1  # Picking BMesh is never rewritten mid-tool.
            event('WHEELUPMOUSE')
            event('WHEELUPMOUSE')
        elif stage == 2:
            assert len(tree().nodes) == 6 and cut().inputs['Cuts'].default_value == 3
            assert cut().as_pointer() == pending_pointer
            assert cut().inputs['Factor'].default_value == 0
            event('ESC')  # Placement cancel removes the complete pending cut.
        elif stage == 3:
            assert len(tree().nodes) == 3 and bpy.context.mode == 'EDIT_MESH'
            bm = bmesh.from_edit_mesh(bpy.context.object.data)
            assert [v.index for v in bm.verts if v.select] == [0]
            ctrl_r()
        elif stage == 4:
            remember_cut()
            event('THREE', unicode='3')
        elif stage == 5:
            assert len(tree().nodes) == 6 and cut().inputs['Cuts'].default_value == 3
            event('LEFTMOUSE')  # Placement enters slide, without committing or adding another node.
        elif stage == 6:
            assert len(tree().nodes) == 6 and cut().inputs['Factor'].default_value == 0
            event('MOUSEMOVE', 'NOTHING', dy=25)
        elif stage == 7:
            assert len(tree().nodes) == 6 and abs(cut().inputs['Factor'].default_value) > 1e-5
            assert cut().as_pointer() == pending_pointer
            event('MOUSEMOVE', 'NOTHING', dy=45, shift=True)
        elif stage == 8:
            assert len(tree().nodes) == 6
            event('ESC')  # Slide cancel commits centered cuts as a single graph undo step.
        elif stage == 9:
            assert len(tree().nodes) == 5 and bpy.context.mode == 'EDIT_MESH'
            assert cut().inputs['Factor'].default_value == 0
            bm = bmesh.from_edit_mesh(bpy.context.object.data)
            assert len(bm.verts) == 10 and len(bm.faces) == 4
            assert len([e for e in bm.edges if e.select]) == 3
            assert bpy.context.tool_settings.mesh_select_mode[:] == (False, True, False)
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert bpy.ops.ed.undo() == {'FINISHED'}
            assert_clean(3)
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert bpy.ops.ed.redo() == {'FINISHED'}
            assert_clean(5)
            begin()
            ctrl_r()
        elif stage == 10:
            assert len(tree().nodes) == 8
            remember_cut()
            event('LEFTMOUSE')
        elif stage == 11:
            event('MOUSEMOVE', 'NOTHING', dy=20)
            event('MINUS', unicode='-')
            event('ZERO', unicode='0')
            event('PERIOD', unicode='.')
            event('FOUR', unicode='4')
        elif stage == 12:
            assert abs(cut().inputs['Factor'].default_value + 0.4) < 1e-5
            event('LEFTMOUSE')
        elif stage == 13:
            assert len(tree().nodes) == 7 and bpy.context.mode == 'EDIT_MESH'
            assert abs(cut().inputs['Factor'].default_value + 0.4) < 1e-5
            ctrl_r()
        elif stage == 14:
            assert len(tree().nodes) == 10
            event('LEFTMOUSE')
        elif stage == 15:
            event('ESC', shift=True)  # Abort both stages, not a centered third cut.
        elif stage == 16:
            assert_clean(7)
            begin()
            ctrl_r()
        elif stage == 17:
            assert len(tree().nodes) == 10
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.node.geometry_viewer_edit_transaction(action='ABORT_LOOP_CUT') == {'FINISHED'}
            assert_clean(7)
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert bpy.ops.ed.undo() == {'FINISHED'}
            assert_clean(5)
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert bpy.ops.ed.undo() == {'FINISHED'}
            assert_clean(3)
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert bpy.ops.ed.redo() == {'FINISHED'}
                assert bpy.ops.ed.redo() == {'FINISHED'}
            assert_clean(7)
            begin()
            ctrl_r()
        elif stage == 18:
            assert len(tree().nodes) == 10
            event('LEFTMOUSE')
        elif stage == 19:
            bpy.context.scene.frame_set(bpy.context.scene.frame_current + 1)
        elif stage == 20:
            assert_clean(7)
            begin()
            ctrl_r()
        elif stage == 21:
            assert len(tree().nodes) == 10
            event('LEFTMOUSE')
        elif stage == 22:
            with bpy.context.temp_override(**override(NODE_AREA)):
                bpy.ops.wm.save_as_mainfile(filepath=os.path.join(test_dir.name, 'loop-cut.blend'))
            assert_clean(7)  # Forced cancellation during slide must not keep centered topology.
            print('GEOMETRY_VIEWER_LOOP_CUT_UI_SMOKE_OK', flush=True)
            bpy.ops.wm.quit_blender()
            return None
        stage += 1
        return 0.5
    except Exception:
        traceback.print_exc()
        print('GEOMETRY_VIEWER_LOOP_CUT_UI_SMOKE_FAILED', stage, flush=True)
        bpy.ops.wm.quit_blender()
        return None


mesh = bpy.data.meshes.new(SOURCE)
if bpy.data.objects.get('Cube'):
    bpy.data.objects['Cube'].hide_set(True)  # Do not let the factory cube occlude the hovered test edge.
mesh.from_pydata([(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)], [], [(0, 1, 2, 3)])
obj = bpy.data.objects.new(SOURCE, mesh)
bpy.context.collection.objects.link(obj)
bpy.context.view_layer.objects.active = obj
obj.select_set(True)
group = bpy.data.node_groups.new(TREE, 'GeometryNodeTree')
group.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
input_node = group.nodes.new('NodeGroupInput')
output_node = group.nodes.new('NodeGroupOutput')
node = group.nodes.new('GeometryNodeViewer')
node.viewer_items.new(socket_type='GEOMETRY', name='Geometry')
group.links.new(input_node.outputs['Geometry'], output_node.inputs['Geometry'])
group.links.new(input_node.outputs['Geometry'], node.inputs['Geometry'])
obj.modifiers.new(TREE, 'NODES').node_group = group
NODE_AREA.type = 'NODE_EDITOR'
NODE_AREA.ui_type = 'GeometryNodeTree'
NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
view = VIEW_AREA.spaces.active.region_3d
view.view_rotation = Quaternion()
view.view_location = (0, 0, 0)
view.view_distance = 5
view.view_perspective = 'ORTHO'
obj = group = mesh = node = input_node = output_node = view = None
with bpy.context.temp_override(**override(NODE_AREA)):
    bpy.ops.ed.undo_push(message='Viewer Loop Cut Modal Baseline')
bpy.app.timers.register(tick, first_interval=1)
