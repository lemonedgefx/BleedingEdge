# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Load a real pre-consolidation fixture with both legacy selection node types.

Run --background --factory-startup --python-exit-code 1 --python this_file.py
    -- --fixture /path/to/legacy_capture.blend
The fixture is written by selection_replay_smoke on the previous executable.
"""

import argparse
import os
import sys
import tempfile

import bpy


parser = argparse.ArgumentParser()
parser.add_argument('--fixture', required=True)
args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
bpy.ops.wm.open_mainfile(filepath=args.fixture)


def check(group):
    obj = bpy.data.objects['Selection Replay Source']
    manual = group.nodes['Legacy Manual Selection']
    assert manual.bl_idname == 'GeometryNodeSelectByIndex'
    assert manual.mode == 'MANUAL' and manual.domain == 'EDGE'
    assert manual.inputs['Indices'].default_value == '0, 2-4'
    assert manual.inputs[1].identifier == 'Geometry' and manual.inputs[1].is_unavailable
    captures = [n for n in group.nodes if n.bl_idname == 'GeometryNodeCapturedSelection']
    assert len(captures) == 2 and all(n.domain == 'FACE' and n.selected_count == 1 for n in captures)
    scale = next(n for n in group.nodes if n.bl_idname == 'GeometryNodeScaleElements')
    captured = next(l.from_node for l in group.links if l.to_node == scale and l.to_socket.name == 'Selection')
    assert captured.bl_idname == 'GeometryNodeCapturedSelection'
    assert scale.inputs['Center'].is_linked  # Preserve the existing legacy frozen pivot link.
    for node in (manual, *captures):
        input_node = next(n for n in group.nodes if n.bl_idname == 'NodeGroupInput')
        geometry = (input_node.outputs['Geometry'] if node == manual else
                    next(l.from_socket for l in group.links if l.to_node == node))
        output = next(n for n in group.nodes if n.bl_idname == 'NodeGroupOutput')
        old = next(l.from_socket for l in group.links if l.to_node == output)
        store = group.nodes.new('GeometryNodeStoreNamedAttribute')
        store.data_type = 'BOOLEAN'
        store.domain = 'POINT'
        store.inputs['Name'].default_value = 'legacy_valid'
        group.links.new(geometry, store.inputs['Geometry'])
        group.links.new(node.outputs['Valid'], store.inputs['Value'])
        group.links.new(store.outputs['Geometry'], output.inputs['Geometry'])
        try:
            bpy.context.view_layer.update()
            depsgraph = bpy.context.evaluated_depsgraph_get()
            evaluated = obj.evaluated_get(depsgraph)
            mesh = evaluated.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph)
            assert all(v.value for v in mesh.attributes['legacy_valid'].data), node.name
            evaluated.to_mesh_clear()
        finally:
            group.links.new(old, output.inputs['Geometry'])
            group.nodes.remove(store)
    # Old manual nodes acquire safe storage and can change mode without a null dereference.
    manual.mode = 'CAPTURED'
    manual.mode = 'MANUAL'
    assert manual.inputs['Indices'].default_value == '0, 2-4'


group = bpy.data.node_groups['Selection Replay Tree']
check(group)
clone = group.copy()
obj = bpy.data.objects['Selection Replay Source']
obj.modifiers[0].node_group = clone
check(clone)
obj.modifiers[0].node_group = group
bpy.data.node_groups.remove(clone)
group = clone = obj = None
with tempfile.TemporaryDirectory(prefix='viewer-legacy-') as directory:
    path = os.path.join(directory, 'legacy-roundtrip.blend')
    bpy.ops.wm.save_as_mainfile(filepath=path)
    bpy.ops.wm.open_mainfile(filepath=path)
    check(bpy.data.node_groups['Selection Replay Tree'])
print('GEOMETRY_VIEWER_SELECTION_COMPATIBILITY_SMOKE_OK', flush=True)
