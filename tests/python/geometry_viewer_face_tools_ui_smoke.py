# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Real face shortcuts/menu: mapped triangle selection, continuation and one-step undo.

Run --factory-startup --enable-event-simulate --python this_file.py.
Require GEOMETRY_VIEWER_FACE_TOOLS_UI_SMOKE_OK and a clean process exit.
"""

import os
import tempfile
import traceback

import bmesh
import bpy


NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
SOURCE = TREE = 'Viewer Face Tools UI Test'
test_dir = tempfile.TemporaryDirectory(prefix='viewer-face-tools-ui-')
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


def viewer():
    return next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeViewer')


def result_node():
    return next(l.from_node for l in tree().links if l.to_node == viewer()
                and l.to_socket.type == 'GEOMETRY')


def event(kind, value='PRESS', **kwargs):
    region = next(r for r in VIEW_AREA.regions if r.type == 'WINDOW')
    bpy.context.window.event_simulate(type=kind, value=value, x=region.x + region.width // 2,
                                     y=region.y + region.height // 2, **kwargs)


def key(kind, **kwargs):
    event('MOUSEMOVE', 'NOTHING')
    event(kind, **kwargs)
    kwargs.pop('unicode', None)
    event(kind, 'RELEASE', **kwargs)


def begin():
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
    for index in (0, 2):
        bm.faces[index].select_set(True)
    bm.select_history.add(bm.faces[2])
    bm.faces.active = bm.faces[2]
    bm.select_flush_mode()
    bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)


def assert_picking(count, smooth=None, hidden=False):
    assert len(tree().nodes) == count and bpy.context.mode == 'EDIT_MESH', (count, bpy.context.mode)
    assert len(source().data.polygons) == 3
    assert not any(n.bl_idname in ('GeometryNodeStoreNamedAttribute', 'GeometryNodeInputIndex')
                   for n in tree().nodes)
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    layer = bm.faces.layers.int['source_face']
    selected = [f for f in bm.faces if f.select]
    assert len(bm.faces) == 5 and len(selected) == 4
    assert sorted(f[layer] for f in selected) == [20, 20, 22, 22]
    assert bm.faces.active is not None and bm.faces.active[layer] == 22
    assert bm.select_history.active is not None and bm.select_history.active[layer] == 22
    assert all(len(f.verts) == 3 for f in selected)
    if smooth is not None:
        assert all(f.smooth == smooth for f in selected)
    if hidden:
        assert [f[layer] for f in bm.faces if f.hide] == [21]
        assert not any(f.hide for f in selected)


def assert_clean(count):
    assert bpy.context.mode == 'OBJECT' and bpy.context.object == source()
    assert len(tree().nodes) == count
    assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)


def tick():
    global stage
    try:
        if stage == -2:
            key('ESC')
        elif stage == -1:
            begin()
        elif stage == 0:
            key('F', ctrl=True)
        elif stage == 1:
            assert len(tree().nodes) == 3
            key('ESC')
        elif stage == 2:
            assert len(tree().nodes) == 3 and bpy.context.mode == 'EDIT_MESH'
            key('T', ctrl=True)
        elif stage == 3:
            assert_picking(5, smooth=False)
            assert result_node().inputs['Quad Method'].default_value == 'Beauty'
            key('T', ctrl=True)  # Already-triangular selected faces must not add a no-op node/undo.
        elif stage == 4:
            assert_picking(5)
            key('F', ctrl=True)
        elif stage == 5:
            key('TWO', unicode='2')  # Flip Faces.
        elif stage == 6:
            assert_picking(7)
            assert result_node().bl_idname == 'GeometryNodeFlipFaces'
            bm = bmesh.from_edit_mesh(bpy.context.object.data)
            layer = bm.faces.layers.int['source_face']
            next(f for f in bm.faces if f[layer] == 21).hide_set(True)
            bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)
            bm = layer = None
            key('F', ctrl=True)
        elif stage == 7:
            key('THREE', unicode='3')  # Shade Smooth.
        elif stage == 8:
            assert_picking(9, smooth=True, hidden=True)
            assert result_node().domain == 'FACE'
            key('F', ctrl=True)
        elif stage == 9:
            key('THREE', unicode='3')  # Already-smooth shading is also a no-op.
        elif stage == 10:
            assert_picking(9, smooth=True, hidden=True)
            key('F', ctrl=True)
        elif stage == 11:
            key('FOUR', unicode='4')  # Shade Flat.
        elif stage == 12:
            assert_picking(11, smooth=False, hidden=True)
            key('G')
        elif stage == 13:
            key('Z')
            key('ONE', unicode='1')
            key('RET')
        elif stage == 14:
            assert_picking(13, smooth=False, hidden=True)
            for count in (11, 9, 7, 5, 3):
                with bpy.context.temp_override(**override(VIEW_AREA)):
                    assert bpy.ops.ed.undo() == {'FINISHED'}
                assert_clean(count)
            for count in (5, 7, 9, 11, 13):
                with bpy.context.temp_override(**override(VIEW_AREA)):
                    assert bpy.ops.ed.redo() == {'FINISHED'}
                assert_clean(count)
        elif stage == 15:
            with bpy.context.temp_override(**override(NODE_AREA)):
                bpy.ops.wm.save_as_mainfile(filepath=os.path.join(test_dir.name, 'faces-ui.blend'))
            assert_clean(13)
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert not bpy.ops.node.geometry_viewer_edit_face.poll()
                assert bpy.ops.object.mode_set(mode='EDIT') == {'FINISHED'}
                assert bpy.ops.mesh.quads_convert_to_tris.poll()
                assert not bpy.ops.node.geometry_viewer_edit_face.poll()
                bpy.ops.object.mode_set(mode='OBJECT')
            print('GEOMETRY_VIEWER_FACE_TOOLS_UI_SMOKE_OK', flush=True)
            bpy.ops.wm.quit_blender()
            return None
        stage += 1
        return 0.5
    except Exception:
        traceback.print_exc()
        print('GEOMETRY_VIEWER_FACE_TOOLS_UI_SMOKE_FAILED', stage, flush=True)
        bpy.ops.wm.quit_blender()
        return None


mesh = bpy.data.meshes.new(SOURCE)
mesh.from_pydata([(x, y, 0) for y in range(2) for x in range(4)], [],
                 [(x, x + 1, x + 5, x + 4) for x in range(3)])
attribute = mesh.attributes.new('source_face', 'INT', 'FACE')
for i, value in enumerate(attribute.data):
    value.value = 20 + i
for face in mesh.polygons:
    face.use_smooth = False
obj = bpy.data.objects.new(SOURCE, mesh)
bpy.context.collection.objects.link(obj)
bpy.context.view_layer.objects.active = obj
obj.select_set(True)
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
obj = mesh = group = input_node = output = node = attribute = None
with bpy.context.temp_override(**override(NODE_AREA)):
    bpy.ops.ed.undo_push(message='Viewer Face Tools UI Baseline')
bpy.app.timers.register(tick, first_interval=1)
