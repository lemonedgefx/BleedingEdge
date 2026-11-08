# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Ordinary node selection must not end the active Viewer editing session.

Run --factory-startup --enable-event-simulate in a GUI build.
Require GEOMETRY_VIEWER_NODE_SELECTION_UI_SMOKE_OK
and a clean exit; timers exercise the real draw, evaluation and notifier loop.
"""

import tempfile
import traceback

import bmesh
import bpy


NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'DOPESHEET_EDITOR')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
stage = 0
test_temp_dir = tempfile.TemporaryDirectory(prefix='viewer-node-selection-')
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.filepaths.temporary_directory = test_temp_dir.name


def override(area):
    return dict(window=bpy.context.window, screen=bpy.context.screen, area=area,
                region=next(r for r in area.regions if r.type == 'WINDOW'))


def click_node(node):
    region = next(r for r in NODE_AREA.regions if r.type == 'WINDOW')
    scale = bpy.context.preferences.system.ui_scale
    location = region.view2d.view_to_region(
        (node.location_absolute.x + node.width * 0.5) * scale,
        (node.location_absolute.y - 10) * scale, clip=False)
    x, y = region.x + location[0], region.y + location[1]
    for kind, value in (('MOUSEMOVE', 'NOTHING'), ('LEFTMOUSE', 'PRESS'), ('LEFTMOUSE', 'RELEASE')):
        bpy.context.window.event_simulate(type=kind, value=value, x=x, y=y)


def select_all_nodes():
    region = next(r for r in NODE_AREA.regions if r.type == 'WINDOW')
    bpy.context.window.event_simulate(type='A', value='PRESS',
                                      x=region.x + region.width // 2,
                                      y=region.y + region.height // 2)


def assert_editing():
    assert bpy.context.mode == 'EDIT_MESH', 'Clicking a node ended Viewer Edit Mode'
    assert bpy.context.object == proxy, 'Node selection replaced the picking mesh'
    with bpy.context.temp_override(**override(VIEW_AREA)):
        assert bpy.ops.node.geometry_viewer_edit_delete.poll()


def run_step():
    global stage, source, tree, viewer, input_node, proxy
    try:
        if stage == 0:
            bpy.context.window.event_simulate(type='ESC', value='PRESS', x=100, y=100)
            bpy.ops.mesh.primitive_cube_add()
            source = bpy.context.object
            tree = bpy.data.node_groups.new('Viewer Node Selection', 'GeometryNodeTree')
            tree.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
            tree.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
            input_node = tree.nodes.new('NodeGroupInput')
            output = tree.nodes.new('NodeGroupOutput')
            viewer = tree.nodes.new('GeometryNodeViewer')
            viewer.viewer_items.new(socket_type='GEOMETRY', name='Geometry')
            input_node.location = (0, 0)
            output.location = (350, -250)
            viewer.location = (350, 0)
            tree.links.new(input_node.outputs['Geometry'], output.inputs['Geometry'])
            tree.links.new(input_node.outputs['Geometry'], viewer.inputs['Geometry'])
            source.modifiers.new('Viewer Node Selection', 'NODES').node_group = tree
            NODE_AREA.type = 'NODE_EDITOR'
            NODE_AREA.ui_type = 'GeometryNodeTree'
            NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
        elif stage == 1:
            bpy.context.scene['viewer_node_selection_marker'] = 0
            with bpy.context.temp_override(**override(NODE_AREA)):
                bpy.ops.ed.undo_push(message='Before Viewer node selection')
            tree.nodes.active = viewer
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.node.activate_viewer() == {'FINISHED'}
                bpy.context.view_layer.update()
                assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
                bpy.ops.node.view_all()
            proxy = bpy.context.object
            with bpy.context.temp_override(**override(VIEW_AREA)):
                bpy.ops.mesh.select_all(action='DESELECT')
            bm = bmesh.from_edit_mesh(proxy.data)
            bm.verts.ensure_lookup_table()
            bm.verts[0].select_set(True)
            bmesh.update_edit_mesh(proxy.data, loop_triangles=False, destructive=False)
        elif stage == 2:
            assert_editing()
            click_node(input_node)
        elif stage == 3:
            assert_editing()
            assert tree.nodes.active == input_node
            select_all_nodes()
        elif stage == 4:
            assert_editing()
            assert all(node.select for node in tree.nodes)
            tree.nodes.active = viewer
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert bpy.ops.transform.translate(value=(0.1, 0, 0),
                    use_proportional_edit=False, mirror=False) == {'FINISHED'}
            with bpy.context.temp_override(**override(NODE_AREA)):
                bpy.ops.node.geometry_viewer_edit_transaction(action='VALIDATE')
                bpy.ops.node.view_all()
            proxy = bpy.context.object
            tree.nodes.active = input_node
        elif stage == 5:
            assert_editing()
            operation = next(n for n in tree.nodes if n.bl_idname == 'GeometryNodeSetPosition')
            click_node(operation)
        elif stage == 6:
            assert_editing()
            assert tree.nodes.active.bl_idname == 'GeometryNodeSetPosition'
            capture = next(n for n in tree.nodes if n.bl_idname == 'GeometryNodeSelectByIndex')
            click_node(capture)
        elif stage == 7:
            assert_editing()
            assert tree.nodes.active.bl_idname == 'GeometryNodeSelectByIndex'
            assert_editing()
        elif stage == 8:
            # Unlike node selection, an unrelated property edit still needs global undo and
            # must retire the proxy before its snapshot. Do not swallow these undo steps.
            bpy.context.scene['viewer_node_selection_marker'] = 1
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.ed.undo_push(message='Viewer other editor property') == {'FINISHED'}
        elif stage == 9:
            assert bpy.context.mode == 'OBJECT'
            assert bpy.context.object == source
            assert len(tree.nodes) == 5  # The confirmed move is retained; only the session ends.
            assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.ed.undo() == {'FINISHED'}
            assert bpy.context.scene['viewer_node_selection_marker'] == 0
            assert bpy.context.mode == 'OBJECT'
            assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)
            print('GEOMETRY_VIEWER_NODE_SELECTION_UI_SMOKE_OK', flush=True)
            bpy.ops.wm.quit_blender()
            return None
        stage += 1
        return 0.5
    except Exception:
        traceback.print_exc()
        print('GEOMETRY_VIEWER_NODE_SELECTION_UI_SMOKE_FAILED', stage, flush=True)
        bpy.ops.wm.quit_blender()
        return None


bpy.app.timers.register(run_step, first_interval=0.5)
