# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Repeated confirmed moves share a session-owned Set Position node.

Run --background --factory-startup --python-exit-code 1 --python this_file.py.
Requires the move-accumulation adapter; an older executable must fail the second move.
"""

from pathlib import Path
import runpy

import bmesh
import bpy
from mathutils import Vector


# Reuse the native-transform fixture without running its independent modal test.
fixture = runpy.run_path(str(Path(__file__).with_name('geometry_viewer_transform_ui_smoke.py')))
bpy.app.timers.unregister(fixture['run_step'])
tree, source, viewer = fixture['tree'], fixture['source'], fixture['viewer']
override, begin, select = fixture['override'], fixture['begin'], fixture['select']
NODE_AREA, VIEW_AREA = fixture['NODE_AREA'], fixture['VIEW_AREA']
checks = 0


def refresh():
    with bpy.context.temp_override(**override(NODE_AREA)):
        assert bpy.ops.node.geometry_viewer_edit_transaction(action='VALIDATE') == {'FINISHED'}
    assert bpy.context.mode == 'EDIT_MESH'


def finish():
    with bpy.context.temp_override(**override(NODE_AREA)):
        assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
    assert bpy.context.mode == 'OBJECT'


def result():
    return next(link.from_node for link in tree().links
                if link.to_node == viewer() and link.to_socket.type == 'GEOMETRY')


def positions():
    bm = bmesh.from_edit_mesh(bpy.context.object.data)
    bm.verts.ensure_lookup_table()
    return [vertex.co.copy() for vertex in bm.verts]


def move(delta):
    with bpy.context.temp_override(**override(VIEW_AREA)):
        assert bpy.ops.transform.translate(value=delta, orient_type='GLOBAL',
                                            use_proportional_edit=False, mirror=False) == {'FINISHED'}
    refresh()


def check_positions(expected):
    actual = positions()
    assert len(actual) == len(expected)
    assert all((a - b).length < 2e-4 for a, b in zip(actual, expected))


for parented in (False, True):
    if parented:
        parent = bpy.data.objects.new('Move Accumulation Sheared Parent', None)
        bpy.context.collection.objects.link(parent)
        parent.rotation_euler = (-0.3, 0.2, 0.1)
        parent.scale = (2, 0.5, 1.25)
        source().parent = parent
        bpy.context.view_layer.update()
    for domain in ('VERT', 'EDGE', 'FACE'):
        begin(domain)
        initial = positions()
        bm = bmesh.from_edit_mesh(bpy.context.object.data)
        bm.verts.index_update()
        affected = {v.index for v in bm.verts if v.select}
        del bm
        count = len(tree().nodes)
        delta_a, delta_b = Vector((0.2, -0.3, 0.4)), Vector((-0.05, 0.15, -0.1))
        local = source().matrix_world.inverted().to_3x3()
        move(delta_a)
        operation_name = result().name
        capture_name = next(link.from_node.name for link in tree().links
                            if link.to_node == result() and link.to_socket.name == 'Selection')
        assert len(tree().nodes) == count + 2
        move(delta_b)
        assert len(tree().nodes) == count + 2, 'A repeated move created another node pair'
        assert result().name == operation_name
        assert tree().nodes[capture_name].selected_count > 0
        total = local @ (delta_a + delta_b)
        assert (Vector(result().inputs['Offset'].default_value) - total).length < 2e-4
        check_positions([p + total if i in affected else p for i, p in enumerate(initial)])
        with bpy.context.temp_override(**override(VIEW_AREA)):
            assert bpy.ops.transform.translate(value=(0, 0, 0)) == {'CANCELLED'}
        assert len(tree().nodes) == count + 2
        assert result().name == operation_name

        # A new explicitly entered Edit Mode must not reuse a previous session's node.
        finish()
        begin(domain)
        move(delta_a)
        assert len(tree().nodes) == count + 4
        assert result().name != operation_name
        finish()
        checks += 1

# Selection changes and a non-translation operation end a move run.
begin('FACE')
count = len(tree().nodes)
move((0.1, 0, 0))
first_name = result().name
select('EDGE')
move((0, 0.1, 0))
assert len(tree().nodes) == count + 4 and result().name != first_name
edge_name = result().name
move((0, 0, 0.1))
assert len(tree().nodes) == count + 4 and result().name == edge_name
select('FACE')
with bpy.context.temp_override(**override(VIEW_AREA)):
    assert bpy.ops.transform.resize(value=(1.1,) * 3, use_proportional_edit=False,
                                    mirror=False) == {'FINISHED'}
refresh()
count = len(tree().nodes)
move((0.1, 0, 0))
assert len(tree().nodes) == count + 2
finish()
checks += 1

# Different indices in the same domain must not be added to the old selection's Offset.
begin('VERT')
count = len(tree().nodes)
move((0.1, 0, 0))
first_name = result().name
with bpy.context.temp_override(**override(VIEW_AREA)):
    bpy.ops.mesh.select_all(action='DESELECT')
bm = bmesh.from_edit_mesh(bpy.context.object.data)
bm.verts.ensure_lookup_table()
bm.verts[1].select_set(True)
bm.select_history.add(bm.verts[1])
bm.select_flush_mode()
bmesh.update_edit_mesh(bpy.context.object.data, loop_triangles=False, destructive=False)
del bm
move((0, 0.1, 0))
assert len(tree().nodes) == count + 4 and result().name != first_name
second_name = result().name
move((0, 0, 0.1))
assert len(tree().nodes) == count + 4 and result().name == second_name
finish()
checks += 1

# Sharing the previous operation's output must keep that other branch unchanged.
begin('FACE')
count = len(tree().nodes)
move((0.1, 0, 0))
first_name = result().name
first_offset = Vector(result().inputs['Offset'].default_value)
output = next(n for n in tree().nodes if n.bl_idname == 'NodeGroupOutput')
tree().links.new(result().outputs['Geometry'], output.inputs['Geometry'])
bpy.context.view_layer.update()
move((0, 0.1, 0))
assert len(tree().nodes) == count + 4 and result().name != first_name
assert (Vector(tree().nodes[first_name].inputs['Offset'].default_value) - first_offset).length < 1e-6
input_node = next(n for n in tree().nodes if n.bl_idname == 'NodeGroupInput')
tree().links.new(input_node.outputs['Geometry'], output.inputs['Geometry'])
finish()
checks += 1
print('GEOMETRY_VIEWER_MOVE_ACCUMULATION_OK', checks, flush=True)
