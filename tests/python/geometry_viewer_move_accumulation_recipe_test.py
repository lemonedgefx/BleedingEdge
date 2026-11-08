# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Ordinary-node recipe proof, independent of the new C++ session adapter.

Run --background --factory-startup --python-exit-code 1 --python this_file.py.
An existing Viewer build can validate offset addition and restoration, not automatic batching.
"""

from pathlib import Path
import runpy

import bmesh
import bpy
from mathutils import Vector


fixture = runpy.run_path(str(Path(__file__).with_name('geometry_viewer_transform_ui_smoke.py')))
bpy.app.timers.unregister(fixture['run_step'])
tree, source, viewer = fixture['tree'], fixture['source'], fixture['viewer']
override = fixture['override']
checks = 0


def evaluate(operation):
    output = next(n for n in tree().nodes if n.bl_idname == 'NodeGroupOutput')
    input_node = next(n for n in tree().nodes if n.bl_idname == 'NodeGroupInput')
    tree().links.new(operation.outputs['Geometry'], output.inputs['Geometry'])
    try:
        bpy.context.view_layer.update()
        evaluated = source().evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = evaluated.to_mesh()
        result = [v.co.copy() for v in mesh.vertices]
        evaluated.to_mesh_clear()
        return result
    finally:
        tree().links.new(input_node.outputs['Geometry'], output.inputs['Geometry'])
        bpy.context.view_layer.update()


for parented in (False, True):
    if parented:
        parent = bpy.data.objects.new('Move Recipe Sheared Parent', None)
        bpy.context.collection.objects.link(parent)
        parent.rotation_euler = (-0.3, 0.2, 0.1)
        parent.scale = (2, 0.5, 1.25)
        source().parent = parent
        bpy.context.view_layer.update()
    for domain in ('VERT', 'EDGE', 'FACE'):
        fixture['begin'](domain)
        bm = bmesh.from_edit_mesh(bpy.context.object.data)
        bm.verts.index_update()
        affected = {v.index for v in bm.verts if v.select}
        initial = [v.co.copy() for v in bm.verts]
        del bm
        local = source().matrix_world.inverted().to_3x3()
        delta_a = local @ Vector((0.2, -0.3, 0.4))
        delta_b = local @ Vector((-0.05, 0.15, -0.1))
        with bpy.context.temp_override(**override(fixture['VIEW_AREA'])):
            assert bpy.ops.transform.translate(value=(0.2, -0.3, 0.4), orient_type='GLOBAL',
                                                use_proportional_edit=False, mirror=False) == {'FINISHED'}
        with bpy.context.temp_override(**override(fixture['NODE_AREA'])):
            bpy.ops.node.geometry_viewer_edit_transaction(action='VALIDATE')
            assert bpy.context.mode == 'EDIT_MESH'
            assert bpy.ops.node.geometry_viewer_editing_toggle() == {'FINISHED'}
        operation = next(link.from_node for link in tree().links
                         if link.to_node == viewer() and link.to_socket.type == 'GEOMETRY')
        assert operation.bl_idname == 'GeometryNodeSetPosition'
        capture = next(link.from_node for link in tree().links
                       if link.to_node == operation and link.to_socket.name == 'Selection')
        assert capture.mode == 'CAPTURED' and capture.selected_count > 0
        count = len(tree().nodes)
        # The native adapter will write these cumulative values; this test only proves replay.
        for offset in (delta_a, delta_a + delta_b, delta_a, Vector((0, 0, 0)), delta_a + delta_b):
            operation.inputs['Offset'].default_value = offset
            actual = evaluate(operation)
            expected = [p + offset if i in affected else p for i, p in enumerate(initial)]
            assert len(actual) == len(expected)
            assert all((a - b).length < 2e-4 for a, b in zip(actual, expected))
            assert len(tree().nodes) == count
            checks += 1
        # Leave a stable ordinary recipe for the next independent test case.
        operation.inputs['Offset'].default_value = delta_a + delta_b
        bpy.context.view_layer.update()

print('GEOMETRY_VIEWER_MOVE_ACCUMULATION_RECIPE_OK', checks, flush=True)
