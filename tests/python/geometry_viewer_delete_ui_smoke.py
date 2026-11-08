# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Real X/Delete popup routing, Escape, numeric choices and safe undo/redo.

Run --factory-startup --enable-event-simulate --python this_file.py.
Require GEOMETRY_VIEWER_DELETE_UI_SMOKE_OK and a clean process exit.
"""

import tempfile
import traceback

import bmesh
import bpy


NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
SOURCE = TREE = 'Viewer Delete Menu Test'
stage = -1
test_dir = tempfile.TemporaryDirectory(prefix='viewer-delete-ui-')
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.filepaths.temporary_directory = test_dir.name


def override(area):
    return dict(window=bpy.context.window, screen=bpy.context.screen, area=area,
                region=next(r for r in area.regions if r.type == 'WINDOW'))


def tree():
    return bpy.data.node_groups[TREE]


def source():
    return bpy.data.objects[SOURCE]


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


def menu(key='X'):
    event('MOUSEMOVE', 'NOTHING')
    event(key)
    event(key, 'RELEASE')


def select_edge():
    with bpy.context.temp_override(**override(VIEW_AREA)):
        bpy.ops.mesh.select_mode(type='EDGE')
        bpy.ops.mesh.select_all(action='DESELECT')
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    for elements in (bm.verts, bm.edges, bm.faces):
        elements.ensure_lookup_table()
        elements.index_update()
    edge = next(e for e in bm.edges if {v.index for v in e.verts} == {1, 4})
    edge.select_set(True)
    bm.select_history.add(edge)
    bm.select_flush_mode()
    bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)


def begin(select_seed=True):
    bpy.context.view_layer.objects.active = source()
    source().select_set(True)
    tree().nodes.active = viewer()
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.node.activate_viewer()
        bpy.context.view_layer.update()
        assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
    if select_seed:
        select_edge()


def assert_retired(count):
    assert bpy.context.mode == 'OBJECT' and bpy.context.object == source()
    assert len(tree().nodes) == count
    assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)


def tick():
    global stage
    try:
        if stage == -1:
            event('ESC')  # Dismiss the startup splash before testing viewport keymaps.
        elif stage == 0:
            begin()
            menu()
        elif stage == 1:
            assert len(tree().nodes) == 3  # Opening the popup creates no capture or operation.
            event('ESC')
        elif stage == 2:
            assert len(tree().nodes) == 3 and bpy.context.mode == 'EDIT_MESH'
            assert len([e for e in bmesh.from_edit_mesh(bpy.context.object.data).edges if e.select]) == 1
            menu('DEL')
        elif stage == 3:
            assert len(tree().nodes) == 3
            event('ESC')
        elif stage == 4:
            assert len(tree().nodes) == 3 and bpy.context.mode == 'EDIT_MESH'
            menu()
        elif stage == 5:
            # Standard enum popup numeric shortcuts count actionable items, not the separator.
            event('SEVEN', unicode='7')  # Dissolve Edges.
        elif stage == 6:
            assert len(tree().nodes) == 5, len(tree().nodes)
            assert result_node().bl_idname == 'GeometryNodeDissolveElements'
            assert result_node().domain == 'EDGE'
            assert bpy.context.mode == 'EDIT_MESH'
            bm = bmesh.from_edit_mesh(bpy.context.object.data)
            assert len(bm.faces) == 1
            assert not any(e.select for elements in (bm.verts, bm.edges, bm.faces) for e in elements)
            bm = None
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert bpy.ops.ed.undo() == {'FINISHED'}
            assert_retired(3)
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert bpy.ops.ed.redo() == {'FINISHED'}
            assert_retired(5)
            begin(select_seed=False)
            with bpy.context.temp_override(**override(VIEW_AREA)):
                bpy.ops.mesh.select_all(action='DESELECT')
            menu('DEL')
        elif stage == 7:
            event('ONE', unicode='1')  # Empty selection must not import an empty capture.
        elif stage == 8:
            assert len(tree().nodes) == 5 and bpy.context.mode == 'EDIT_MESH'
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
            assert_retired(5)
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert not bpy.ops.node.geometry_viewer_edit_delete.poll()
                assert bpy.ops.object.mode_set(mode='EDIT') == {'FINISHED'}
                assert not bpy.ops.node.geometry_viewer_edit_delete.poll()
                assert bpy.ops.mesh.delete.poll()
                bpy.ops.mesh.select_all(action='DESELECT')
            menu()  # The native mesh menu remains available outside Viewer editing.
        elif stage == 9:
            event('ESC')
        elif stage == 10:
            assert len(tree().nodes) == 5
            with bpy.context.temp_override(**override(VIEW_AREA)):
                bpy.ops.object.mode_set(mode='OBJECT')
            print('GEOMETRY_VIEWER_DELETE_UI_SMOKE_OK', flush=True)
            bpy.ops.wm.quit_blender()
            return None
        stage += 1
        return 0.5
    except Exception:
        traceback.print_exc()
        print('GEOMETRY_VIEWER_DELETE_UI_SMOKE_FAILED', stage, flush=True)
        bpy.ops.wm.quit_blender()
        return None


mesh = bpy.data.meshes.new(SOURCE)
mesh.from_pydata([(x, y, 0) for y in range(2) for x in range(3)], [],
                 [(0, 1, 4, 3), (1, 2, 5, 4)])
obj = bpy.data.objects.new(SOURCE, mesh)
bpy.context.collection.objects.link(obj)
bpy.context.view_layer.objects.active = obj
obj.select_set(True)
group = bpy.data.node_groups.new(TREE, 'GeometryNodeTree')
group.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
input_node = group.nodes.new('NodeGroupInput')
output = group.nodes.new('NodeGroupOutput')
node = group.nodes.new('GeometryNodeViewer')
node.viewer_items.new(socket_type='GEOMETRY', name='Geometry')
group.links.new(input_node.outputs['Geometry'], output.inputs['Geometry'])
group.links.new(input_node.outputs['Geometry'], node.inputs['Geometry'])
obj.modifiers.new(TREE, 'NODES').node_group = group
NODE_AREA.type = 'NODE_EDITOR'
NODE_AREA.ui_type = 'GeometryNodeTree'
NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
obj = mesh = group = input_node = output = node = None
with bpy.context.temp_override(**override(NODE_AREA)):
    bpy.ops.ed.undo_push(message='Viewer Delete Menu Baseline')
bpy.app.timers.register(tick, first_interval=1)
