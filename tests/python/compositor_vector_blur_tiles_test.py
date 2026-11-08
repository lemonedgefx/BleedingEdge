"""A smooth image and affine motion must not acquire the guidance tile grid."""

import os
import tempfile

import bpy
import numpy as np

folder = os.environ.get('VB_OUTPUT') or tempfile.mkdtemp(prefix='vector_tiles_')
os.makedirs(folder, exist_ok=True)
scene = bpy.context.scene
scene.render.engine = 'BLENDER_WORKBENCH'
scene.render.compositor_device = os.environ.get('VB_DEVICE', 'CPU')
scene.render.resolution_x, scene.render.resolution_y = 257, 193
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.image_settings.color_depth = '32'
scene.render.image_settings.color_mode = 'RGBA'
scene.render.dither_intensity = 0
scene.view_settings.view_transform = 'Raw'

y, x = np.indices((193, 257))
color = np.ones((193, 257, 4), np.float32)
color[:, :, 0] = 0.5 + 0.2 * np.sin(x / 25) + 0.15 * np.cos(y / 30)
color[:, :, 1] = 0.5 + 0.2 * np.cos(x / 40) + 0.15 * np.sin(y / 23)
color[:, :, 2] = 0.5 + 0.2 * np.sin((x + y) / 35)
speed = np.empty_like(color)
speed[:, :, 0] = 150 - (y - 96) * 0.5
speed[:, :, 1] = 70 + (x - 128) * 0.5
speed[:, :, 2:] = speed[:, :, :2]

tree = bpy.data.node_groups.new('tile_continuity', 'CompositorNodeTree')
tree.interface.new_socket('Image', in_out='OUTPUT', socket_type='NodeSocketColor')
scene.compositing_node_group = tree
node = tree.nodes.new('CompositorNodeVecBlur')
node.inputs['Samples'].default_value = 256
node.inputs['Shutter'].default_value = 2
if 'Curved' in node.inputs:
    node.inputs['Curved'].default_value = False
for label, pixels in [('Image', color), ('Speed', speed)]:
    image = bpy.data.images.new(label, 257, 193, alpha=True, float_buffer=True)
    image.alpha_mode = 'PREMUL'
    image.colorspace_settings.name = 'Non-Color'
    image.pixels.foreach_set(pixels.ravel())
    image.update()
    upstream = tree.nodes.new('CompositorNodeImage')
    upstream.image = image
    tree.links.new(upstream.outputs['Image'], node.inputs[label])
out = tree.nodes.new('NodeGroupOutput')
tree.links.new(node.outputs['Image'], out.inputs['Image'])

for falloff in (['None', 'Smooth'] if 'Falloff' in node.inputs else ['None']):
    if 'Falloff' in node.inputs:
        node.inputs['Falloff'].default_value = falloff
    scene.render.filepath = os.path.join(folder, falloff + '.exr')
    bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(scene.render.filepath)
    image.colorspace_settings.name = 'Non-Color'
    values = np.array(image.pixels[:], np.float32).reshape(color.shape)
    np.save(os.path.join(folder, falloff + '.npy'), values)
    interior = values[40:-40, 40:-40, :3]
    curvature = max(float(np.max(np.abs(np.diff(interior, 2, axis=0)))),
                    float(np.max(np.abs(np.diff(interior, 2, axis=1)))))
    print('TILE_DISCONTINUITY', falloff, curvature, flush=True)
    assert curvature < 0.01, f'Block discontinuities on smooth input: {curvature}'
print('VECTOR_BLUR_TILES_OK', scene.render.compositor_device, flush=True)
