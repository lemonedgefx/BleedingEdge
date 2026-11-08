# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Repeated G/gizmo-path moves, borrowed-node cancellation, undo and session boundaries.

Run --factory-startup --enable-event-simulate --python this_file.py.
Requires GEOMETRY_VIEWER_MOVE_ACCUMULATION_UI_SMOKE_OK and a clean exit.
"""

from pathlib import Path
import runpy
import traceback

import bpy
from mathutils import Vector


fixture = runpy.run_path(str(Path(__file__).with_name('geometry_viewer_transform_ui_smoke.py')))
bpy.app.timers.unregister(fixture['run_step'])
tree, source, viewer = fixture['tree'], fixture['source'], fixture['viewer']
event, override = fixture['event'], fixture['override']
stage = 0
baseline = 0
operation_name = None
next_operation_name = None
first = total = next_offset = None


def result():
    return next(link.from_node for link in tree().links
                if link.to_node == viewer() and link.to_socket.type == 'GEOMETRY')


def close_offset(name, expected):
    actual = Vector(tree().nodes[name].inputs['Offset'].default_value)
    assert (actual - expected).length < 3e-5, (actual, expected)


def g_axis(axis, digit):
    event('MOUSEMOVE', 'NOTHING')
    event('G')
    event('G', 'RELEASE')
    event(axis)
    event({'1': 'ONE', '2': 'TWO'}[digit], unicode=digit)


def tick():
    global stage, baseline, operation_name, next_operation_name, first, total, next_offset
    try:
        if stage == 0:
            event('ESC')
        elif stage == 1:
            fixture['begin']('FACE')
            baseline = len(tree().nodes)
            local = source().matrix_world.inverted().to_3x3()
            first = local @ Vector((1, 0, 0))
            total = local @ Vector((1, 2, 0))
            g_axis('X', '1')
        elif stage == 2:
            assert len(tree().nodes) == baseline + 2
            operation_name = result().name
            close_offset(operation_name, first)
            event('RET')
        elif stage == 3:
            assert bpy.context.mode == 'EDIT_MESH'
            g_axis('Y', '2')
        elif stage == 4:
            assert len(tree().nodes) == baseline + 2, 'Second G created another node pair'
            assert result().name == operation_name
            close_offset(operation_name, total)
            event('RET')
        elif stage == 5:
            assert bpy.context.mode == 'EDIT_MESH'
            assert len(tree().nodes) == baseline + 2
            # Gizmos use this same native translate invocation seam.
            fixture['invoke']('MOVE')
        elif stage == 6:
            assert len(tree().nodes) == baseline + 2
            event('ESC')
        elif stage == 7:
            assert bpy.context.mode == 'EDIT_MESH'
            close_offset(operation_name, total)
            assert len(tree().nodes) == baseline + 2
            with bpy.context.temp_override(**override(fixture['VIEW_AREA'])):
                assert bpy.ops.transform.translate(value=(0, 0, 0)) == {'CANCELLED'}
            fixture['invoke']('MOVE')
        elif stage == 8:
            event('R')  # Native mode switching must not delete the borrowed move nodes.
        elif stage == 9:
            assert len(tree().nodes) == baseline + 9
            close_offset(operation_name, total)
            event('ESC')
        elif stage == 10:
            assert bpy.context.mode == 'EDIT_MESH'
            assert len(tree().nodes) == baseline + 2
            close_offset(operation_name, total)
            with bpy.context.temp_override(**override(fixture['NODE_AREA'])):
                assert bpy.ops.ed.undo() == {'FINISHED'}
            assert bpy.context.mode == 'OBJECT'
            assert len(tree().nodes) == baseline + 2
            close_offset(operation_name, first)
        elif stage == 11:
            with bpy.context.temp_override(**override(fixture['NODE_AREA'])):
                assert bpy.ops.ed.redo() == {'FINISHED'}
            assert bpy.context.mode == 'OBJECT'
            close_offset(operation_name, total)
            fixture['begin']('FACE')
            g_axis('Z', '1')
        elif stage == 12:
            assert len(tree().nodes) == baseline + 4
            next_operation_name = result().name
            assert next_operation_name != operation_name
            event('RET')
        elif stage == 13:
            assert bpy.context.mode == 'EDIT_MESH'
            assert len(tree().nodes) == baseline + 4
            fixture['invoke']('MOVE')
        elif stage == 14:
            event('MOUSEMOVE', 'NOTHING', dx=30)
            event('RET')
        elif stage == 15:
            assert bpy.context.mode == 'EDIT_MESH'
            assert len(tree().nodes) == baseline + 4
            assert result().name == next_operation_name
            next_offset = Vector(result().inputs['Offset'].default_value)
            g_axis('X', '1')
        elif stage == 16:
            assert len(tree().nodes) == baseline + 4
            bpy.context.scene.frame_set(bpy.context.scene.frame_current + 1)
        elif stage == 17:
            assert bpy.context.mode == 'OBJECT'
            assert len(tree().nodes) == baseline + 4
            close_offset(next_operation_name, next_offset)
            close_offset(operation_name, total)
            assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)
            print('GEOMETRY_VIEWER_MOVE_ACCUMULATION_UI_SMOKE_OK', flush=True)
            bpy.ops.wm.quit_blender()
            return None
        stage += 1
        return 0.5
    except Exception:
        traceback.print_exc()
        print('GEOMETRY_VIEWER_MOVE_ACCUMULATION_UI_SMOKE_FAILED', stage, flush=True)
        bpy.ops.wm.quit_blender()
        return None


bpy.app.timers.register(tick, first_interval=1)
