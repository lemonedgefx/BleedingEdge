# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Create with the old executable, verify with the new executable. No fixture is overwritten."""

from array import array
from pathlib import Path
import argparse
import sys

import bpy


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--create', action='store_true')
    parser.add_argument('--fixture', required=True)
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    path = Path(args.fixture)
    if args.create:
        assert not path.exists(), 'Do not overwrite an existing fixture'
        scene = bpy.context.scene
        scene.render.engine = 'BLENDER_WORKBENCH'
        scene.render.resolution_x, scene.render.resolution_y = 97, 65
        scene.render.resolution_percentage = 100
        tree = bpy.data.node_groups.new('LegacyImageWorkflow', 'CompositorNodeTree')
        tree.interface.new_socket('Image', in_out='OUTPUT', socket_type='NodeSocketColor')
        scene.compositing_node_group = tree
        image = bpy.data.images.new('LegacyImageFixture', 97, 65, float_buffer=True)
        image.colorspace_settings.name = 'Non-Color'
        image.pixels.foreach_set(array('f', [0.2, 0.4, 0.6, 1]) * (97 * 65))
        image.update()
        image.pack()
        node = tree.nodes.new('CompositorNodeImage')
        node.name = 'LegacyImageNode'
        node.image = image
        node.frame_start = 17
        node.frame_duration = 5
        node.frame_offset = 3
        output = tree.nodes.new('NodeGroupOutput')
        tree.links.new(node.outputs['Image'], output.inputs['Image'])
        assert 'Previous Frame' not in node.outputs, 'Create this fixture using the old build'
        bpy.ops.wm.save_as_mainfile(filepath=str(path))
        print('FLOW_LEGACY_FIXTURE_CREATED', bpy.app.build_hash, flush=True)
        return
    bpy.ops.wm.open_mainfile(filepath=str(path), load_ui=False)
    tree = bpy.data.node_groups['LegacyImageWorkflow']
    node = tree.nodes['LegacyImageNode']
    assert [s.identifier for s in node.outputs][:2] == ['Image', 'Alpha']
    assert node.frame_start == 17 and node.frame_duration == 5 and node.frame_offset == 3
    assert 'Previous Frame' in node.outputs and 'Next Frame' in node.outputs
    output = next(n for n in tree.nodes if n.bl_idname == 'NodeGroupOutput')
    assert output.inputs['Image'].links[0].from_socket == node.outputs['Image']
    scene = bpy.context.scene
    scene.render.compositor_device = 'CPU'
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_depth = '32'
    scene.view_settings.view_transform = 'Raw'
    for socket in ('Image', 'Previous Frame', 'Next Frame'):
        tree.links.new(node.outputs[socket], output.inputs['Image'])
        target = path.parent / ('legacy_' + socket.replace(' ', '_') + '.exr')
        scene.render.filepath = str(target)
        bpy.ops.render.render(write_still=True)
        rendered = bpy.data.images.load(str(target), check_existing=False)
        data = array('f', [0]) * (97 * 65 * 4)
        rendered.pixels.foreach_get(data)
        assert max(abs(v - (0.2, 0.4, 0.6, 1)[i % 4]) for i, v in enumerate(data)) < 0.0001
        bpy.data.images.remove(rendered)
    print('FLOW_LEGACY_IMAGE_COMPATIBILITY_OK', flush=True)


if __name__ == '__main__':
    main()
