# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Node layout must remain editable, undoable and independent of Viewer mesh editing.

Run --factory-startup --enable-event-simulate --python this_file.py in a GUI build.
Require GEOMETRY_VIEWER_NODE_LAYOUT_UI_SMOKE_OK and a clean process exit.
"""

import tempfile
import traceback

import bmesh
import bpy


NODE_AREA = next(a for a in bpy.context.screen.areas if a.type == 'PROPERTIES')
VIEW_AREA = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
TREE_NAME = 'Viewer Node Layout Test'
stage = 0
test_dir = tempfile.TemporaryDirectory(prefix='viewer-node-layout-')
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.filepaths.temporary_directory = test_dir.name


def override(area):
    return dict(window=bpy.context.window, screen=bpy.context.screen, area=area,
                region=next(r for r in area.regions if r.type == 'WINDOW'))


def tree():
    return bpy.data.node_groups[TREE_NAME]


def input_node():
    return next(n for n in tree().nodes if n.bl_idname == 'NodeGroupInput')


def viewer():
    return next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeViewer')


def event(kind, value='PRESS', dx=0):
    bpy.context.window.event_simulate(type=kind, value=value, x=drag_x + dx, y=drag_y)


def point_at(node):
    global drag_x, drag_y
    region = next(r for r in NODE_AREA.regions if r.type == 'WINDOW')
    scale = bpy.context.preferences.system.ui_scale
    location = region.view2d.view_to_region(
        (node.location_absolute.x + node.width * 0.5) * scale,
        (node.location_absolute.y - 10) * scale, clip=False)
    drag_x, drag_y = region.x + location[0], region.y + location[1]
    event('MOUSEMOVE', 'NOTHING')


def select_node(node):
    for item in tree().nodes:
        item.select = item == node
    tree().nodes.active = node
    point_at(node)


def assert_editing():
    assert bpy.context.mode == 'EDIT_MESH', 'Node layout ended Viewer Edit Mode'
    proxy = bpy.context.object
    assert proxy.name.startswith('Viewer Editing')
    bm = bmesh.from_edit_mesh(proxy.data)
    bm.verts.ensure_lookup_table()
    assert [v.index for v in bm.verts if v.select] == [0], 'Node layout changed mesh selection'
    assert bm.verts[1].hide, 'Node layout lost hidden elements'
    assert bm.select_history.active == bm.verts[0], 'Node layout lost the active element'
    assert len(tree().nodes) == 4, 'Node layout authored geometry-operation nodes'
    assert len(tree().links) == 2, 'Node layout changed geometry links'
    assert len([o for o in bpy.data.objects if o.name.startswith('Viewer Editing')]) == 1


def mesh_state():
    assert bpy.context.mode == 'EDIT_MESH'
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    for elements in (bm.verts, bm.edges, bm.faces):
        elements.ensure_lookup_table()
        elements.index_update()
    active = bm.select_history.active
    if active is None and bpy.context.tool_settings.mesh_select_mode[2]:
        active = bm.faces.active if bm.faces.active and bm.faces.active.select else None
    return (tuple(bpy.context.tool_settings.mesh_select_mode),
            tuple(tuple(e.index for e in elements if e.select) for elements in (bm.verts, bm.edges, bm.faces)),
            tuple(tuple(e.index for e in elements if e.hide) for elements in (bm.verts, bm.edges, bm.faces)),
            (type(active).__name__, active.index) if active else None,
            tuple(tuple(v.co) for v in bm.verts))


def select_domain(domain):
    with bpy.context.temp_override(**override(VIEW_AREA)):
        bpy.ops.mesh.select_all(action='DESELECT')
        bpy.ops.mesh.select_mode(type=domain)
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    bm.select_history.clear()
    if domain == 'EDGE':
        element = next(e for e in bm.edges if not e.hide)
        element.select_set(True)
        bm.select_history.add(element)
    elif domain == 'FACE':
        element = next(f for f in bm.faces if not f.hide)
        element.select_set(True)
        bm.faces.active = element  # Exercise active-face fallback without selection history.
    bm.select_flush_mode()
    bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)


def assert_layout_state():
    assert mesh_state() == selection_state, 'Layout changed mesh selection, active element, hiding or positions'
    assert len(tree().nodes) == 6, 'Layout created or removed operation nodes'
    assert len([o for o in bpy.data.objects if o.name.startswith('Viewer Editing')]) == 1


def move_viewer():
    global layout_before
    select_node(viewer())
    layout_before = viewer().location_absolute.copy()
    with bpy.context.temp_override(**override(NODE_AREA)):
        assert bpy.ops.transform.translate('EXEC_DEFAULT', True, value=(15, 0, 0)) == {'FINISHED'}


def run_step():
    global stage, initial, moved, keyboard, viewer_initial, frame_initial, child_initial
    global capture_initial, capture_moved, selection_state, layout_before
    try:
        if stage == 0:
            bpy.context.window.event_simulate(type='ESC', value='PRESS', x=100, y=100)
            bpy.ops.mesh.primitive_cube_add()
            source = bpy.context.object
            group = bpy.data.node_groups.new(TREE_NAME, 'GeometryNodeTree')
            group.interface.new_socket(name='Geometry', in_out='INPUT', socket_type='NodeSocketGeometry')
            group.interface.new_socket(name='Geometry', in_out='OUTPUT', socket_type='NodeSocketGeometry')
            entry = group.nodes.new('NodeGroupInput')
            output = group.nodes.new('NodeGroupOutput')
            preview = group.nodes.new('GeometryNodeViewer')
            preview.viewer_items.new(socket_type='GEOMETRY', name='Geometry')
            frame = group.nodes.new('NodeFrame')
            frame.name = 'Layout Frame'
            entry.location = (0, 0)
            output.location = (350, -250)
            preview.location = (350, 0)
            frame.location = (-250, 120)
            frame.shrink = False
            entry.parent = frame
            entry.location = (25, -50)
            group.links.new(entry.outputs['Geometry'], output.inputs['Geometry'])
            group.links.new(entry.outputs['Geometry'], preview.inputs['Geometry'])
            source.modifiers.new(TREE_NAME, 'NODES').node_group = group
            NODE_AREA.type = 'NODE_EDITOR'
            NODE_AREA.ui_type = 'GeometryNodeTree'
            NODE_AREA.spaces.active.node_tree_sub_type = 'MODIFIER'
            NODE_AREA.spaces.active.show_region_ui = False
            NODE_AREA.spaces.active.show_region_toolbar = False
        elif stage == 1:
            tree().nodes.active = viewer()
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.node.activate_viewer() == {'FINISHED'}
                bpy.context.view_layer.update()
                bpy.ops.node.view_all()
                bpy.ops.ed.undo_push(message='Before Viewer node layout')
                assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
            with bpy.context.temp_override(**override(VIEW_AREA)):
                bpy.ops.mesh.select_all(action='DESELECT')
            bm = bmesh.from_edit_mesh(bpy.context.object.data)
            bm.verts.ensure_lookup_table()
            bm.verts[0].select_set(True)
            bm.select_history.add(bm.verts[0])
            bm.verts[1].hide_set(True)
            bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)
            initial = input_node().location_absolute.copy()
        elif stage == 2:
            assert_editing()
            select_node(input_node())
            event('LEFTMOUSE')
        elif stage == 3:
            event('MOUSEMOVE', 'NOTHING', dx=10)
        elif stage == 4:
            event('MOUSEMOVE', 'NOTHING', dx=80)
        elif stage == 5:
            event('LEFTMOUSE', 'RELEASE', dx=80)
        elif stage == 6:
            moved = input_node().location_absolute.copy()
            assert (moved - initial).length > 1, 'Viewer Edit Mode blocked node-header dragging'
            assert_editing()
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.ed.undo() == {'FINISHED'}
        elif stage == 7:
            assert (input_node().location_absolute - initial).length < 1e-4, 'Layout undo lost its step'
            assert_editing()
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.ed.redo() == {'FINISHED'}
        elif stage == 8:
            assert (input_node().location_absolute - moved).length < 1e-4, 'Layout redo did not restore movement'
            assert_editing()
            select_node(input_node())
            event('G')
        elif stage == 9:
            event('MOUSEMOVE', 'NOTHING', dx=35)
        elif stage == 10:
            event('RET')
        elif stage == 11:
            keyboard = input_node().location_absolute.copy()
            assert (keyboard - moved).length > 1, 'Viewer Edit Mode blocked node-editor G'
            assert_editing()
            select_node(input_node())
            event('G')
        elif stage == 12:
            event('MOUSEMOVE', 'NOTHING', dx=35)
        elif stage == 13:
            event('ESC')
        elif stage == 14:
            assert (input_node().location_absolute - keyboard).length < 1e-4, 'Cancelled layout move was retained'
            assert_editing()
            select_node(viewer())
            viewer_initial = viewer().location.copy()
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.transform.translate('EXEC_DEFAULT', True, value=(30, 0, 0)) == {'FINISHED'}
        elif stage == 15:
            assert (viewer().location - viewer_initial).length > 1
            assert_editing()
            frame = tree().nodes['Layout Frame']
            select_node(frame)
            frame_initial = frame.location.copy()
            child_initial = input_node().location_absolute.copy()
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.transform.translate('EXEC_DEFAULT', True, value=(20, 0, 0)) == {'FINISHED'}
        elif stage == 16:
            assert (tree().nodes['Layout Frame'].location - frame_initial).length > 1
            assert (input_node().location_absolute - child_initial).length > 1
            assert_editing()
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.ed.undo() == {'FINISHED'}
        elif stage == 17:
            assert (tree().nodes['Layout Frame'].location - frame_initial).length < 1e-4
            assert_editing()
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.ed.redo() == {'FINISHED'}
        elif stage == 18:
            assert (tree().nodes['Layout Frame'].location - frame_initial).length > 1
            assert_editing()
            # Node layout must not disable the subsequent viewport transform adapter.
            with bpy.context.temp_override(**override(VIEW_AREA)):
                assert bpy.ops.transform.translate('EXEC_DEFAULT', True, value=(0.2, 0, 0),
                    use_proportional_edit=False, mirror=False) == {'FINISHED'}
        elif stage == 19:
            assert bpy.context.mode == 'EDIT_MESH'
            assert len(tree().nodes) == 6
            assert len([n for n in tree().nodes if n.bl_idname == 'GeometryNodeSetPosition']) == 1
            assert len([n for n in tree().nodes if n.bl_idname == 'GeometryNodeSelectByIndex']) == 1
            selection_state = mesh_state()
            capture = next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeSelectByIndex')
            capture_initial = capture.location_absolute.copy()
            select_node(capture)
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.transform.translate('EXEC_DEFAULT', True, value=(20, 0, 0)) == {'FINISHED'}
        elif stage == 20:
            capture = next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeSelectByIndex')
            capture_moved = capture.location_absolute.copy()
            assert (capture_moved - capture_initial).length > 1
            assert_layout_state()
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.ed.undo() == {'FINISHED'}
        elif stage == 21:
            capture = next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeSelectByIndex')
            assert (capture.location_absolute - capture_initial).length < 1e-4
            assert_layout_state()
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.ed.redo() == {'FINISHED'}
        elif stage == 22:
            capture = next(n for n in tree().nodes if n.bl_idname == 'GeometryNodeSelectByIndex')
            assert (capture.location_absolute - capture_moved).length < 1e-4
            assert_layout_state()
            select_domain('EDGE')
            selection_state = mesh_state()
            move_viewer()
        elif stage in (23, 26, 29):
            assert (viewer().location_absolute - layout_before).length > 1
            assert_layout_state()
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.ed.undo() == {'FINISHED'}
        elif stage in (24, 27, 30):
            assert (viewer().location_absolute - layout_before).length < 1e-4
            assert_layout_state()
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.ed.redo() == {'FINISHED'}
        elif stage == 25:
            assert (viewer().location_absolute - layout_before).length > 1
            assert_layout_state()
            select_domain('FACE')
            selection_state = mesh_state()
            move_viewer()
        elif stage == 28:
            assert (viewer().location_absolute - layout_before).length > 1
            assert_layout_state()
            with bpy.context.temp_override(**override(VIEW_AREA)):
                bpy.ops.mesh.select_all(action='DESELECT')
            selection_state = mesh_state()
            assert not any(selection_state[1])
            move_viewer()
        elif stage == 31:
            assert (viewer().location_absolute - layout_before).length > 1
            assert_layout_state()
            tree().nodes.active = viewer()
            with bpy.context.temp_override(**override(NODE_AREA)):
                assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
            assert bpy.context.mode == 'OBJECT'
            assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)
            print('GEOMETRY_VIEWER_NODE_LAYOUT_UI_SMOKE_OK', flush=True)
            bpy.ops.wm.quit_blender()
            return None
        stage += 1
        return 0.5
    except Exception:
        traceback.print_exc()
        print('GEOMETRY_VIEWER_NODE_LAYOUT_UI_SMOKE_FAILED', stage, flush=True)
        bpy.ops.wm.quit_blender()
        return None


bpy.app.timers.register(run_step, first_interval=0.5)
