# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Run --factory-startup --enable-event-simulate --python this_file.py in a GUI build.

Timers let the real notifier loop validate native modal sessions and cleanup.
"""

import tempfile
import traceback

import bmesh
import bpy


def override(area):
    return dict(window=bpy.context.window, screen=bpy.context.screen, area=area,
                region=next(r for r in area.regions if r.type == 'WINDOW'))


node_area = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
view_area = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
stage = 0
test_temp_dir = tempfile.TemporaryDirectory(prefix='viewer-ui-smoke-')
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.filepaths.temporary_directory = test_temp_dir.name


def pair_count():
    return sum(n.bl_idname in {'GeometryNodeSelectByIndex', 'GeometryNodeSetPosition'}
               for n in tree.nodes)


def begin(check_exit_undo=False):
    bpy.context.view_layer.objects.active = source
    source.select_set(True)
    tree.nodes.active = viewer
    with bpy.context.temp_override(**override(node_area)):
        assert bpy.ops.node.activate_viewer() == {'FINISHED'}
    bpy.context.view_layer.update()
    if check_exit_undo:
        with bpy.context.temp_override(**override(node_area)):
            bpy.context.scene['viewer_exit_undo_marker'] = 0
            bpy.ops.ed.undo_push(message='Before Viewer exit test')
            bpy.context.scene['viewer_exit_undo_marker'] = 1
            bpy.ops.ed.undo_push(message='Viewer exit test baseline')
    with bpy.context.temp_override(**override(node_area)):
        assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
    with bpy.context.temp_override(**override(view_area)):
        bpy.ops.mesh.select_all(action='DESELECT')
        bpy.ops.mesh.select_mode(type='VERT')
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    bm.verts.ensure_lookup_table()
    bm.verts[0].select_set(True)
    bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)


def preview(offset):
    region = next(r for r in view_area.regions if r.type == 'WINDOW')
    x, y = region.x + region.width // 2, region.y + region.height // 2
    if pair_count() == 0:
        with bpy.context.temp_override(**override(view_area)):
            assert bpy.ops.transform.translate('INVOKE_DEFAULT', value=offset,
                use_proportional_edit=False, mirror=False, release_confirm=False) == {'RUNNING_MODAL'}
    bpy.context.window.event_simulate(type='MOUSEMOVE', value='NOTHING',
        x=x + 20 + int(offset[0] * 30), y=y + 20 + int(offset[1] * 30))


def assert_clean():
    assert bpy.context.mode == 'OBJECT'
    assert bpy.context.view_layer.objects.active == source
    assert pair_count() == 0
    assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)
    assert not any(m.name.startswith('Viewer Editing') for m in bpy.data.meshes)


def assert_quit_cleanup(*args):
    # Called before quit recovery is written, after the native session cleanup hook.
    assert_clean()
    print('GEOMETRY_VIEWER_EDITING_UI_SMOKE_OK', flush=True)


def run_step():
    global stage, source, tree, viewer
    try:
        if stage == 0:
            bpy.ops.mesh.primitive_cube_add()
            source = bpy.context.object
            source.name = 'Viewer Event Loop Source'
            tree = bpy.data.node_groups.new('Viewer Event Loop Smoke', 'GeometryNodeTree')
            tree.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
            tree.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
            input_node = tree.nodes.new('NodeGroupInput')
            output_node = tree.nodes.new('NodeGroupOutput')
            viewer = tree.nodes.new('GeometryNodeViewer')
            viewer.viewer_items.new(socket_type='GEOMETRY', name='Geometry')
            tree.links.new(input_node.outputs['Geometry'], output_node.inputs['Geometry'])
            tree.links.new(input_node.outputs['Geometry'], viewer.inputs['Geometry'])
            modifier = source.modifiers.new('Viewer Event Loop Smoke', 'NODES')
            modifier.node_group = tree
            node_area.type = 'NODE_EDITOR'
            node_area.ui_type = 'GeometryNodeTree'
            node_area.spaces.active.node_tree_sub_type = 'MODIFIER'
        elif stage == 1:
            begin()
        elif stage == 2:
            assert bpy.context.mode == 'EDIT_MESH'
            preview((0.25, 0.5, 0.75))
        elif stage == 3:
            # The preview must survive the normal reevaluation/notifier cycle.
            assert bpy.context.mode == 'EDIT_MESH' and pair_count() == 2
            preview((0.5, 0.25, -0.5))
        elif stage == 4:
            assert bpy.context.mode == 'EDIT_MESH' and pair_count() == 2
            bpy.context.scene.frame_set(bpy.context.scene.frame_current + 1)
        elif stage == 5:
            # No explicit validation operator: the real event loop must do the cleanup.
            assert_clean()
            begin()
        elif stage == 6:
            preview((0, 0, 0.5))
        elif stage == 7:
            assert bpy.context.mode == 'EDIT_MESH' and pair_count() == 2
            node_area.type = 'TEXT_EDITOR'
        elif stage == 8:
            assert_clean()
            node_area.type = 'NODE_EDITOR'
            node_area.ui_type = 'GeometryNodeTree'
        elif stage == 9:
            begin(check_exit_undo=True)
        elif stage == 10:
            preview((0, 0.25, 0))
        elif stage == 11:
            # A native modal transform owns the EditMesh until it is cancelled.
            region = next(r for r in view_area.regions if r.type == 'WINDOW')
            bpy.context.window.event_simulate(type='ESC', value='PRESS',
                x=region.x + region.width // 2, y=region.y + region.height // 2)
        elif stage == 12:
            assert bpy.context.mode == 'EDIT_MESH' and pair_count() == 0
            with bpy.context.temp_override(**override(view_area)):
                assert bpy.ops.object.mode_set(mode='OBJECT') == {'FINISHED'}
        elif stage == 13:
            assert_clean()
            with bpy.context.temp_override(**override(node_area)):
                assert bpy.ops.ed.undo() == {'FINISHED'}
            # Native mode exit must not add a proxy-only undo step.
            assert bpy.context.scene['viewer_exit_undo_marker'] == 0
            source = bpy.data.objects['Viewer Event Loop Source']
            tree = bpy.data.node_groups['Viewer Event Loop Smoke']
            viewer = next(n for n in tree.nodes if n.bl_idname == 'GeometryNodeViewer')
            assert_clean()
            begin()
        elif stage == 14:
            preview((0.25, 0, 0))
        elif stage == 15:
            bpy.data.objects.remove(source, do_unlink=True)
            source = None
        elif stage == 16:
            assert bpy.context.mode == 'OBJECT'
            assert pair_count() == 0
            assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)
            assert not any(m.name.startswith('Viewer Editing') for m in bpy.data.meshes)
            bpy.ops.mesh.primitive_cube_add()
            source = bpy.context.object
            source.name = 'Viewer Event Loop Source'
            source.modifiers.new('Viewer Event Loop Smoke', 'NODES').node_group = tree
        elif stage == 17:
            begin()
            preview((0, 0, 0.5))
        elif stage == 18:
            assert pair_count() == 2
            bpy.app.handlers.exit_pre.append(assert_quit_cleanup)
            bpy.ops.wm.quit_blender()
            return None
        stage += 1
        return 0.5
    except Exception:
        traceback.print_exc()
        print('GEOMETRY_VIEWER_EDITING_UI_SMOKE_FAILED', stage, flush=True)
        bpy.ops.wm.quit_blender()
        return None


bpy.app.timers.register(run_step, first_interval=0.5)
