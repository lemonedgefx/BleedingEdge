# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Losing a loop-cut hover target must not close Viewer Edit Mode.

Run --factory-startup --enable-event-simulate --python this_file.py.
Requires GEOMETRY_VIEWER_LOOP_CUT_HOVER_UI_SMOKE_OK and a clean exit.
"""

from pathlib import Path
import runpy
import traceback

import bpy


fixture = runpy.run_path(str(Path(__file__).with_name('geometry_viewer_loop_cut_ui_smoke.py')))
bpy.app.timers.unregister(fixture['tick'])
stage = -2
cut_name = None


def off_mesh(kind='MOUSEMOVE', value='NOTHING'):
    region = next(r for r in fixture['VIEW_AREA'].regions if r.type == 'WINDOW')
    bpy.context.window.event_simulate(type=kind, value=value,
                                    x=region.x + 35, y=region.y + 35)


def tick():
    global stage, cut_name
    try:
        if stage == -2:
            fixture['event']('ESC')
        elif stage == -1:
            fixture['begin']()
        elif stage == 0:
            fixture['ctrl_r']()
        elif stage == 1:
            assert bpy.context.mode == 'EDIT_MESH'
            assert len(fixture['tree']().nodes) == 6
            cut_name = next(n.name for n in fixture['tree']().nodes
                            if n.bl_idname == 'GeometryNodeLoopCutSlide')
            off_mesh()
        elif stage == 2:
            assert bpy.context.mode == 'EDIT_MESH', 'Lost hover closed Viewer Edit Mode'
            assert len(fixture['tree']().nodes) == 6
            # Scrolling and clicking empty space must neither commit nor discard the cut.
            off_mesh('WHEELUPMOUSE', 'PRESS')
            off_mesh('LEFTMOUSE', 'PRESS')
        elif stage == 3:
            assert bpy.context.mode == 'EDIT_MESH'
            assert len(fixture['tree']().nodes) == 6
            fixture['event']('MOUSEMOVE', 'NOTHING')
        elif stage == 4:
            assert bpy.context.mode == 'EDIT_MESH'
            assert len(fixture['tree']().nodes) == 6
            assert fixture['tree']().nodes[cut_name].inputs['Cuts'].default_value == 2
            fixture['event']('ESC')
        elif stage == 5:
            assert bpy.context.mode == 'EDIT_MESH'
            assert len(fixture['tree']().nodes) == 3
            fixture['ctrl_r']()
        elif stage == 6:
            off_mesh()
        elif stage == 7:
            assert bpy.context.mode == 'EDIT_MESH'
            # Real frame/context changes must still abort a pending tool.
            bpy.context.scene.frame_set(bpy.context.scene.frame_current + 1)
        elif stage == 8:
            assert bpy.context.mode == 'OBJECT'
            assert len(fixture['tree']().nodes) == 3
            assert not any(o.name.startswith('Viewer Editing') for o in bpy.data.objects)
            print('GEOMETRY_VIEWER_LOOP_CUT_HOVER_UI_SMOKE_OK', flush=True)
            bpy.ops.wm.quit_blender()
            return None
        stage += 1
        return 0.5
    except Exception:
        traceback.print_exc()
        print('GEOMETRY_VIEWER_LOOP_CUT_HOVER_UI_SMOKE_FAILED', stage, flush=True)
        bpy.ops.wm.quit_blender()
        return None


bpy.app.timers.register(tick, first_interval=1)
