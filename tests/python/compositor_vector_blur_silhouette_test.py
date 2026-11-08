"""Antialiased source vectors must not imprint the current silhouette in a streak."""

import os
import tempfile

import bpy
import numpy as np

device = os.environ.get('VB_DEVICE', 'CPU')
folder = os.environ.get('VB_OUTPUT') or tempfile.mkdtemp(prefix='vector_silhouette_')
os.makedirs(folder, exist_ok=True)
scene = bpy.context.scene
scene.render.engine = 'BLENDER_WORKBENCH'
scene.render.compositor_device = device
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.image_settings.color_depth = '32'
scene.render.image_settings.color_mode = 'RGBA'
scene.render.dither_intensity = 0
scene.view_settings.view_transform = 'Raw'


def render(color, speed, name, samples, falloff):
    height, width = color.shape[:2]
    scene.render.resolution_x, scene.render.resolution_y = width, height
    scene.render.resolution_percentage = 100
    tree = bpy.data.node_groups.new(name, 'CompositorNodeTree')
    tree.interface.new_socket('Image', in_out='OUTPUT', socket_type='NodeSocketColor')
    scene.compositing_node_group = tree
    node = tree.nodes.new('CompositorNodeVecBlur')
    node.inputs['Samples'].default_value = samples
    node.inputs['Shutter'].default_value = 1.47
    node.inputs['Curved'].default_value = False
    node.inputs['Falloff'].default_value = falloff
    for socket, values in [('Image', color), ('Speed', speed)]:
        image = bpy.data.images.new(name + socket, width, height, alpha=True, float_buffer=True)
        image.alpha_mode = 'PREMUL'
        image.colorspace_settings.name = 'Non-Color'
        image.pixels.foreach_set(values.ravel())
        image.update()
        upstream = tree.nodes.new('CompositorNodeImage')
        upstream.image = image
        tree.links.new(upstream.outputs['Image'], node.inputs[socket])
    out = tree.nodes.new('NodeGroupOutput')
    tree.links.new(node.outputs['Image'], out.inputs['Image'])
    scene.render.filepath = os.path.join(folder, name + '.exr')
    bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(scene.render.filepath)
    image.colorspace_settings.name = 'Non-Color'
    result = np.array(image.pixels[:], np.float32).reshape(color.shape)
    bpy.data.images.remove(image)
    np.save(os.path.join(folder, name + '.npy'), result)
    assert np.isfinite(result).all(), f'{name}: non-finite output'
    return result


# Like a render with an opaque world: source alpha is one even where the object
# covers only 4% of a pixel. The vector pass is coverage-mixed with the still world.
color = np.ones((97, 257, 4), np.float32)
color[:, :, :3] = 0.05
coverage = np.zeros((97, 257), np.float32)
coverage[24:73, 117:141] = 1
coverage[24:73, 116] = 0.04
color[:, :, :3] += coverage[:, :, None] * 0.95
speed = np.zeros_like(color)
speed[:, :, 0] = coverage * -70
speed[:, :, 2] = speed[:, :, 0]
for falloff in ['None', 'Smooth']:
    for samples in [32, 256]:
        name = f'antialiased_{falloff}_{samples}'
        result = render(color, speed, name, samples, falloff)
        edge = result[48, 115:118, 0]
        curvature = abs(float(edge[0] - 2 * edge[1] + edge[2]))
        print(f'SILHOUETTE_CONTOUR {device} {falloff} {samples}: {curvature:.6f}', flush=True)
        assert curvature < 0.06, f'{name}: current silhouette leaves a narrow contour: {edge}'
        assert np.max(np.abs(result[:, :, 3] - 1)) < 1e-5, f'{name}: world loses alpha'

# A genuine slow foreground in the same guidance tiles as a fast foreground must
# keep its short extent. Repairing edge vectors by substituting tile maxima fails this.
color[:] = 0
speed[:] = 0
color[40:45, 126:131] = 1
color[52:57, 126:131] = 1
speed[40:45, 126:131] = (-70, 0, -70, 0)
speed[52:57, 126:131] = (-0.9, 0, -0.9, 0)
result = render(color, speed, 'true_slow_motion', 256, 'Smooth')
# The entire short path lies in the solid source, so its Smooth window averages
# one half. The preceding build measures 0.4925 here; density correction must
# not introduce extra attenuation.
assert abs(float(result[54, 128, 3]) - 0.5) < 0.06, 'A genuine slow window was diluted'
assert result[54, 136, 3] < 1e-5, 'A genuine slow source was extended to another source motion'
result = render(color, speed, 'true_slow_uniform', 256, 'None')
assert result[54, 128, 3] > 0.9, 'Uniform exposure dilutes a fully covered slow source'
assert result[54, 136, 3] < 1e-5, 'Uniform exposure extends a slow source to tile motion'
color *= 0.5
result = render(color, speed, 'true_slow_translucent', 256, 'None')
assert abs(float(result[54, 128, 3]) - 0.5) < 0.06, 'Uniform exposure changes intrinsic alpha'

# Entirely subpixel motion stays unchanged, including texture and source alpha.
color = np.zeros((33, 65, 4), np.float32)
color[10:23, 22:43] = 1
color[10:23, 22:43:2, :3] = 0
speed = np.zeros_like(color)
speed[10:23, 22:43] = (-0.2, 0, -0.2, 0)
result = render(color, speed, 'subpixel_motion', 256, 'Smooth')
assert np.max(np.abs(result - color)) < 1e-6, 'Subpixel motion was replaced or sharpened'
print(f'VECTOR_BLUR_SILHOUETTE_OK {device}', flush=True)
