# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Validate Viewer modeling routing and native fallback without running Blender."""

import itertools
import runpy
from pathlib import Path


module = runpy.run_path(str(Path(__file__).resolve().parents[2] /
                           'scripts/presets/keyconfig/keymap_data/blender_default.py'))
checks = 0
for select_mouse, fallback, legacy, activate_tools in itertools.product(
        ('LEFT', 'RIGHT'), (False, True), (False, True), (False, True)):
    params = module['Params'](select_mouse=select_mouse, use_fallback_tool=fallback,
                              legacy=legacy, use_key_activate_tools=activate_tools)
    mesh = next(k[2]['items'] for k in module['generate_keymaps'](params) if k[0] == 'Mesh')
    plain_e = [i for i in mesh if i[1].get('type') == 'E' and i[1].get('value') == 'PRESS'
               and not any(i[1].get(k) for k in ('ctrl', 'shift', 'alt', 'oskey'))]
    assert plain_e[0][0] == 'node.geometry_viewer_edit_extrude'
    assert plain_e[1][0] == ('wm.tool_set_by_id' if activate_tools else
                            'view3d.edit_mesh_extrude_move_normal'), plain_e
    assert any(i[0] == 'wm.call_menu' and i[1].get('type') == 'E' and i[1].get('alt')
               for i in mesh)  # Alt+E and the ordinary native tool remain available outside authoring.
    checks += 1
    for shift, quad, ngon in ((False, 'BEAUTY', 'BEAUTY'), (True, 'FIXED', 'CLIP')):
        triangulate = [i for i in mesh if i[1].get('type') == 'T' and i[1].get('value') == 'PRESS'
                       and i[1].get('ctrl') and bool(i[1].get('shift')) == shift
                       and not any(i[1].get(k) for k in ('alt', 'oskey'))]
        assert triangulate[0][0] == 'node.geometry_viewer_edit_face', triangulate
        assert dict(triangulate[0][2]['properties']) == dict(type='TRIANGULATE', quad_method=quad,
                                                          ngon_method=ngon)
        assert triangulate[1][0] == 'mesh.quads_convert_to_tris', triangulate
        checks += 1
    face_menu = [i for i in mesh if i[1].get('type') == 'F' and i[1].get('value') == 'PRESS'
                 and i[1].get('ctrl') and not any(i[1].get(k) for k in ('shift', 'alt', 'oskey'))]
    assert face_menu[0][0] == 'node.geometry_viewer_edit_face', face_menu
    assert dict(face_menu[1][2]['properties'])['name'] == 'VIEW3D_MT_edit_mesh_faces'
    assert any(i[1].get('type') == 'N' and i[1].get('alt') for i in mesh)
    checks += 1
    ctrl_r = [i for i in mesh if i[1].get('type') == 'R' and i[1].get('value') == 'PRESS'
              and i[1].get('ctrl') and not any(i[1].get(k) for k in ('shift', 'alt', 'oskey'))]
    assert ctrl_r[0][0] == 'node.geometry_viewer_edit_loop_cut', ctrl_r
    assert ctrl_r[1][0] == ('wm.tool_set_by_id' if activate_tools else 'mesh.loopcut_slide'), ctrl_r
    assert any(i[1].get('type') == 'R' and i[1].get('ctrl') and i[1].get('shift')
               for i in mesh)  # Ctrl+Shift+R remains the separate ordinary offset-loop tool.
    checks += 1
    merge = [i for i in mesh if i[1].get('type') == 'M' and i[1].get('value') == 'PRESS'
             and not any(i[1].get(k) for k in ('ctrl', 'shift', 'alt', 'oskey'))]
    assert merge[0][0] == 'node.geometry_viewer_edit_merge', merge
    assert merge[1][0] == 'wm.call_menu', merge
    assert dict(merge[1][2]['properties'])['name'] == 'VIEW3D_MT_edit_mesh_merge'
    checks += 1
    duplicate = [i for i in mesh if i[1].get('type') == 'D' and i[1].get('value') == 'PRESS'
                 and i[1].get('shift') and not any(i[1].get(k) for k in ('ctrl', 'alt', 'oskey'))]
    assert duplicate[0][0] == 'node.geometry_viewer_edit_duplicate', duplicate
    assert duplicate[1][0] == 'mesh.duplicate_move', duplicate
    assert any(i[0] == 'mesh.rip_edge_move' and i[1].get('type') == 'D' and i[1].get('alt') for i in mesh)
    checks += 1
    for key in ('X', 'DEL'):
        delete = [i for i in mesh if i[1].get('type') == key and i[1].get('value') == 'PRESS'
                  and not any(i[1].get(k) for k in ('ctrl', 'shift', 'alt', 'oskey'))]
        assert delete[0][0] == 'node.geometry_viewer_edit_delete', delete
        assert delete[1][0] == 'wm.call_menu', delete
        assert dict(delete[1][2]['properties'])['name'] == 'VIEW3D_MT_edit_mesh_delete'
        # Ctrl+X/Delete retain their ordinary bindings; unadapted proxy mutations remain guarded.
        assert any(i[0] == 'mesh.dissolve_mode' and i[1].get('type') == key and i[1].get('ctrl')
                   for i in mesh)
        checks += 1
print('VIEWER_EDIT_KEYMAP_OK', checks)
