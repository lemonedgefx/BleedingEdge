# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Run without -b. Require FLOW_WORKFLOW_UI_SMOKE_OK, not merely exit code zero."""

from array import array
import tempfile
import time
import traceback

import bpy

temporary = tempfile.TemporaryDirectory(prefix='flow_workflow_ui_')
bpy.context.preferences.use_preferences_save = False
bpy.context.preferences.edit.use_global_undo = True
bpy.context.preferences.filepaths.temporary_directory = temporary.name
scene = bpy.context.scene
scene.render.compositor_device = 'CPU'
scene.render.resolution_x, scene.render.resolution_y = 97, 65
scene.render.resolution_percentage = 100
tree = bpy.data.node_groups.new('FlowWorkflowUISmoke', 'CompositorNodeTree')
tree.interface.new_socket('Image', in_out='OUTPUT', socket_type='NodeSocketColor')
scene.compositing_node_group = tree
image = bpy.data.images.new('FlowUIFixture', width=97, height=65, float_buffer=True)
image.pixels.foreach_set(array('f', [0.25, 0.4, 0.6, 1]) * (97 * 65))
image.update()
source = tree.nodes.new('CompositorNodeImage')
source.image = image
source.location = (-500, 150)
output = tree.nodes.new('NodeGroupOutput')
output.location = (500, 150)
tree.links.new(source.outputs['Image'], output.inputs['Image'])
tree.nodes.active = source
tree_name, source_name = tree.name, source.name
editor = next(a for a in bpy.context.screen.areas if a.type == 'VIEW_3D')
editor.type = 'NODE_EDITOR'
editor.ui_type = 'CompositorNodeTree'
editor.spaces.active.pin = False
editor.spaces.active.node_tree = tree
stage = 0
started = time.monotonic()
motion_name = None
interpolate_name = None


def context():
    area = next(a for a in bpy.context.screen.areas if a.type == 'NODE_EDITOR')
    return dict(window=bpy.context.window, screen=bpy.context.screen, area=area,
                region=next(r for r in area.regions if r.type == 'WINDOW'))


def current_tree():
    return bpy.data.node_groups[tree_name]


def tick():
    global stage, motion_name, interpolate_name
    quit_now = False
    try:
        assert time.monotonic() - started < 120, 'UI test timed out'
        if bpy.app.is_job_running('COMPOSITE') or bpy.app.is_job_running('RENDER'):
            return 0.2
        tree = current_tree()
        source = tree.nodes[source_name]
        with bpy.context.temp_override(**context()):
            if stage == 0:
                bpy.ops.ed.undo_push(message='Before optical flow workflow')
                tree.nodes.active = source
                assert bpy.ops.node.add_optical_flow_workflow('EXEC_DEFAULT', True, mode='MOTION_BLUR') == {'FINISHED'}
                node = tree.nodes.active
                motion_name = node.name
                assert node.node_tree.name.startswith('Footage Motion Blur')
                for input_name, source_socket in (('Previous Image', 'Previous Frame'),
                                                  ('Image', 'Image'), ('Next Image', 'Next Frame')):
                    link = node.inputs[input_name].links[0]
                    assert link.from_node == source and link.from_socket.name == source_socket
                assert node.location.x > source.location.x + source.width
                bpy.ops.node.view_all()
            elif stage == 1:
                assert bpy.ops.ed.undo() == {'FINISHED'}
                assert motion_name not in current_tree().nodes
            elif stage == 2:
                assert bpy.ops.ed.redo() == {'FINISHED'}
                assert motion_name in current_tree().nodes
            elif stage == 3:
                tree.nodes.active = source
                assert bpy.ops.node.add_optical_flow_workflow('EXEC_DEFAULT', True, mode='INTERPOLATE') == {'FINISHED'}
                node = tree.nodes.active
                interpolate_name = node.name
                assert node.location != tree.nodes[motion_name].location, 'Workflow groups overlap'
                assert node.inputs['Image'].is_linked and node.inputs['Next Image'].is_linked
                assert node.inputs['Fallback'].default_value == 'Crossfade'
                assert len([n for n in node.node_tree.nodes if n.bl_idname == 'CompositorNodeOpticalFlow']) == 2
            elif stage == 4:
                assert bpy.ops.ed.undo() == {'FINISHED'}
                assert interpolate_name not in current_tree().nodes
            elif stage == 5:
                assert bpy.ops.ed.redo() == {'FINISHED'}
                assert interpolate_name in current_tree().nodes
                # Draw the changed Image node and both group interfaces through the real editor.
                bpy.ops.node.view_all()
                bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=1)
            else:
                print('FLOW_WORKFLOW_UI_SMOKE_OK', flush=True)
                quit_now = True
        stage += 1
    except Exception:
        traceback.print_exc()
        print('FLOW_WORKFLOW_UI_SMOKE_FAILED', flush=True)
        quit_now = True
    finally:
        if quit_now:
            bpy.ops.wm.quit_blender()
    return None if quit_now else 0.3


bpy.app.timers.register(tick, first_interval=0.5)
