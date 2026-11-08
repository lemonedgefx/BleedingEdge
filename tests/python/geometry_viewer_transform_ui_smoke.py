# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Phase 1 real modal input and notifier lifecycle. Requires a Phase 1 GUI build.

Run --factory-startup --enable-event-simulate --python this_file.py.
Require GEOMETRY_VIEWER_TRANSFORM_UI_SMOKE_OK and a clean process exit.
This test uses a private temporary directory for file and quit-recovery writes.
"""

import tempfile
import traceback

import bmesh
import bpy
from mathutils import Vector


NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
SOURCE = 'Viewer Modal Transform Test'
TREE = 'Viewer Modal Transform Test'
stage = 0
baseline = 0
test_dir = tempfile.TemporaryDirectory(prefix='viewer-transform-ui-')
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.filepaths.temporary_directory = test_dir.name


def override(area):
    return dict(window=bpy.context.window, screen=bpy.context.screen, area=area,
                region=next(r for r in area.regions if r.type == 'WINDOW'))


def source():
    return bpy.data.objects[SOURCE]


def tree():
    return bpy.data.node_groups[TREE]


def viewer():
    return next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeViewer')


def event(kind, value='PRESS', dx=0, unicode=''):
    region = next(r for r in VIEW_AREA.regions if r.type == 'WINDOW')
    x, y = region.x + region.width // 2 + dx, region.y + region.height // 2
    args = dict(type=kind, value=value, x=x, y=y)
    if unicode:
        args['unicode'] = unicode
    bpy.context.window.event_simulate(**args)


def select(domain):
    with bpy.context.temp_override(**override(VIEW_AREA)):
        bpy.ops.mesh.select_all(action='DESELECT')
        bpy.ops.mesh.select_mode(type=domain)
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    for elements in (bm.verts, bm.edges, bm.faces):
        elements.ensure_lookup_table()
    element = {'VERT': bm.verts, 'EDGE': bm.edges, 'FACE': bm.faces}[domain][0]
    element.select_set(True)
    bm.select_history.add(element)
    if domain == 'FACE':
        bm.faces.active = element
    bm.select_flush_mode()
    bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)


def begin(domain='EDGE'):
    bpy.context.view_layer.objects.active = source()
    source().select_set(True)
    tree().nodes.active = viewer()
    with bpy.context.temp_override(**override(NODE_AREA)):
        bpy.ops.node.activate_viewer()
        assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
    select(domain)


def invoke(kind):
    with bpy.context.temp_override(**override(VIEW_AREA)):
        common = dict(use_proportional_edit=False, mirror=False, release_confirm=False)
        if kind == 'MOVE':
            status = bpy.ops.transform.translate('INVOKE_DEFAULT', value=(0.25, 0, 0),
                                                orient_type='GLOBAL', **common)
        elif kind == 'SCALE':
            status = bpy.ops.transform.resize('INVOKE_DEFAULT', value=(1.4,) * 3, **common)
        else:
            status = bpy.ops.transform.rotate('INVOKE_DEFAULT', value=0.5,
                                             orient_type='GLOBAL', orient_axis='Z',
                                             constraint_axis=(False, False, True), **common)
    assert status == {'RUNNING_MODAL'}, (kind, status)


def assert_clean():
    assert bpy.context.mode == 'OBJECT'
    assert bpy.context.object == source()
    assert len(tree().nodes) == baseline
    assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)
    assert not any(m.name.startswith('Viewer Editing') for m in bpy.data.meshes)


def assert_quit(*args):
    assert_clean()
    print('GEOMETRY_VIEWER_TRANSFORM_UI_SMOKE_OK', flush=True)


def run_step():
    global stage, baseline
    try:
        if stage == 0:
            event('ESC')  # Dismiss the startup splash and allow editor evaluation to settle.
        elif stage == 1:
            begin()
            baseline = len(tree().nodes)
            event('MOUSEMOVE', 'NOTHING')
        elif stage == 2:
            invoke('MOVE')
        elif stage == 3:
            assert bpy.context.mode == 'EDIT_MESH' and len(tree().nodes) == baseline + 2
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.node.geometry_viewer_import_selection() == {'CANCELLED'}
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert (not bpy.ops.node.geometry_viewer_edit_delete.poll() or
                        bpy.ops.node.geometry_viewer_edit_delete(type='FACE') == {'CANCELLED'})
            assert len(tree().nodes) == baseline + 2
            event('ESC')
        elif stage == 4:
            assert bpy.context.mode == 'EDIT_MESH' and len(tree().nodes) == baseline
            invoke('MOVE')
        elif stage == 5:
            event('MOUSEMOVE', 'NOTHING', dx=40)
            event('X')
            event('TWO', unicode='2')  # Native G, X, 2 replaces Preview Offset.
        elif stage == 6:
            assert len(tree().nodes) == baseline + 2  # Repeated preview does not accumulate nodes.
            operation = next(l.from_node for l in tree().links if l.to_node == viewer())
            expected = source().matrix_world.inverted().to_3x3() @ Vector((2, 0, 0))
            actual = Vector(operation.inputs['Offset'].default_value)
            assert (actual - expected).length < 3e-5, (actual, expected)
            event('RET')
        elif stage == 7:
            assert bpy.context.mode == 'EDIT_MESH' and len(tree().nodes) == baseline + 2
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.ed.undo() == {'FINISHED'}
            assert_clean()
        elif stage == 8:
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.ed.redo() == {'FINISHED'}
            baseline += 2
            assert_clean()
            begin('FACE')
            bpy.context.scene.tool_settings.transform_pivot_point = 'CURSOR'
            bpy.context.scene.cursor.location = (0, 0, 2)
            event('MOUSEMOVE', 'NOTHING')
        elif stage == 9:
            invoke('SCALE')
        elif stage == 10:
            assert len(tree().nodes) == baseline + 2
            assert any(n.bl_idname == 'GeometryNodeScaleElements' for n in tree().nodes)
            event('ESC')
        elif stage == 11:
            assert bpy.context.mode == 'EDIT_MESH' and len(tree().nodes) == baseline
            invoke('ROTATE')
        elif stage == 12:
            assert len(tree().nodes) == baseline + 7
            assert any(n.bl_idname == 'FunctionNodeCombineMatrix' for n in tree().nodes)
            # Cancelling a modal transform through invalidation must remove its handler before
            # deleting the BMesh. No explicit Viewer validation operator is used in this test.
            bpy.context.scene.frame_set(bpy.context.scene.frame_current + 1)
        elif stage == 13:
            assert_clean()
            begin('FACE')
            event('MOUSEMOVE', 'NOTHING')
        elif stage == 14:
            invoke('MOVE')
        elif stage == 15:
            assert len(tree().nodes) == baseline + 2
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.wm.save_as_mainfile(filepath=test_dir.name + '/modal-save.blend') == {'FINISHED'}
            assert_clean()
        elif stage == 16:
            begin('FACE')
            event('MOUSEMOVE', 'NOTHING')
        elif stage == 17:
            invoke('ROTATE')
        elif stage == 18:
            assert len(tree().nodes) == baseline + 7
            bpy.app.handlers.exit_pre.append(assert_quit)
            bpy.ops.wm.quit_blender()
            return None
        stage += 1
        return 0.5
    except Exception:
        traceback.print_exc()
        print('GEOMETRY_VIEWER_TRANSFORM_UI_SMOKE_FAILED', stage, flush=True)
        bpy.ops.wm.quit_blender()
        return None


# Initialize the fixture before the timer, so the first step can only wait for a refresh.
bpy.ops.mesh.primitive_cube_add()
obj = bpy.context.object
obj.name = SOURCE
obj.rotation_euler = (0.2, 0.4, -0.3)
obj.scale = (1.5, 0.7, 2)
node_tree = bpy.data.node_groups.new(TREE, 'GeometryNodeTree')
node_tree.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
node_tree.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
input_node = node_tree.nodes.new('NodeGroupInput')
output_node = node_tree.nodes.new('NodeGroupOutput')
viewer_node = node_tree.nodes.new('GeometryNodeViewer')
viewer_node.viewer_items.new(socket_type='GEOMETRY', name='Geometry')
node_tree.links.new(input_node.outputs['Geometry'], output_node.inputs['Geometry'])
node_tree.links.new(input_node.outputs['Geometry'], viewer_node.inputs['Geometry'])
modifier = obj.modifiers.new('Viewer Modal Transform Test', 'NODES')
modifier.node_group = node_tree
NODE_AREA.type = 'NODE_EDITOR'
NODE_AREA.ui_type = 'GeometryNodeTree'
NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
bpy.context.scene.tool_settings.transform_pivot_point = 'MEDIAN_POINT'
bpy.context.view_layer.update()
with bpy.context.temp_override(**override(NODE_AREA)):
    bpy.ops.ed.undo_push(message='Viewer modal transform baseline')
bpy.app.timers.register(run_step, first_interval=0.5)
