"""Stock directional filtering, direct sample control, and curve approximation quality."""

import os
import tempfile

import bpy
import numpy as np


DEVICE = os.environ.get('VB_DEVICE', 'CPU')
OUTPUT = os.environ.get('VB_OUTPUT') or tempfile.mkdtemp(prefix='vector_directional_')
os.makedirs(OUTPUT, exist_ok=True)
scene = bpy.context.scene
scene.render.engine = 'BLENDER_WORKBENCH'
scene.render.compositor_device = DEVICE
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.image_settings.color_depth = '32'
scene.render.image_settings.color_mode = 'RGBA'
scene.render.dither_intensity = 0
scene.view_settings.view_transform = 'Raw'


def image(name, pixels):
    height, width = pixels.shape[:2]
    result = bpy.data.images.new(name, width, height, alpha=True, float_buffer=True)
    result.alpha_mode = 'PREMUL'
    result.colorspace_settings.name = 'Non-Color'
    result.pixels.foreach_set(pixels.astype(np.float32).ravel())
    result.update()
    return result


def render(source, speed, name, samples, curved=False, falloff='None', single_speed=None):
    width, height = source.size
    scene.render.resolution_x, scene.render.resolution_y = width, height
    scene.render.resolution_percentage = 100
    tree = bpy.data.node_groups.new(name, 'CompositorNodeTree')
    tree.interface.new_socket('Image', in_out='OUTPUT', socket_type='NodeSocketColor')
    scene.compositing_node_group = tree
    node = tree.nodes.new('CompositorNodeVecBlur')
    node.inputs['Samples'].default_value = samples
    node.inputs['Shutter'].default_value = 2
    node.inputs['Curved'].default_value = curved
    node.inputs['Falloff'].default_value = falloff
    for socket, value in [('Image', source), ('Speed', speed)]:
        if value is None:
            continue
        upstream = tree.nodes.new('CompositorNodeImage')
        upstream.image = value
        tree.links.new(upstream.outputs['Image'], node.inputs[socket])
    if single_speed is not None:
        node.inputs['Speed'].default_value = single_speed
    output = tree.nodes.new('NodeGroupOutput')
    tree.links.new(node.outputs['Image'], output.inputs['Image'])
    path = os.path.join(OUTPUT, name + '.exr')
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    result = bpy.data.images.load(path)
    result.colorspace_settings.name = 'Non-Color'
    values = np.array(result.pixels[:], np.float32).reshape(height, width, 4)
    bpy.data.images.remove(result)
    assert np.isfinite(values).all(), f'{name}: non-finite result'
    return values


w, h = 129, 97
y, x = np.indices((h, w))
colors = np.empty((h, w, 4), np.float32)
colors[:, :, 0] = (x % 7) / 6
colors[:, :, 1] = ((x + y) % 5) / 4
colors[:, :, 2] = (y % 3) / 2
colors[:, :, 3] = 1
vectors = np.empty_like(colors)
vectors[:, :, :] = (-50, -14, -50, -14)
source, speed = image('pattern', colors), image('uniform_motion', vectors)
a = render(source, speed, 'linear_none_1', 1)
b = render(source, speed, 'linear_none_64', 64)
delta = float(np.mean(np.abs(a - b)))
assert delta > 0.01, f'Samples is ineffective in directional mode: {delta}'
# Independent snapshots from stock Blender 5.2 CPU at asymmetric image positions.
expected = np.array([[0.2961741, 0.4141237, 0.4276198],
                     [0.4950998, 0.4999998, 0.5089453],
                     [0.4250422, 0.5999685, 0.4182705]])
error = float(np.max(np.abs(b[[10, 48, 87], [10, 64, 119], :3] - expected)))
assert error < 0.003, f'Directional mode differs from stock filtering: {error}'
c = render(source, speed, 'straight_paths_with_curved_on', 64, curved=True)
assert np.max(np.abs(b - c)) < 1e-5, 'Curved changes filtering when trajectories are straight'
d = render(source, None, 'constant_velocity_socket', 64, single_speed=(-50, -14, -50, -14))
assert np.max(np.abs(b - d)) < 1e-5, 'Constant and image velocity inputs differ'
e = render(source, speed, 'linear_smooth', 64, falloff='Smooth')
assert np.max(np.abs(e[:, :, 3] - 1)) < 1e-5, 'Falloff makes opaque camera motion transparent'
assert np.mean(np.abs(e - b)) > 0.005, 'Falloff is ineffective in directional mode'

# A genuinely curved path must respond to the approximation quality too. Its chords
# keep spatial coverage filled even when there are few curve segments.
colors[:] = 0
colors[46:51, 62:67] = 1
vectors[:] = 0
vectors[46:51, 62:67] = (-36, -20, -12, -20)
source, speed = image('curved_marker', colors), image('curved_motion', vectors)
a = render(source, speed, 'curved_quality_1', 1, curved=True, falloff='Smooth')
b = render(source, speed, 'curved_quality_64', 64, curved=True, falloff='Smooth')
relative = float(np.abs(a - b).sum() / max(np.abs(b).sum(), 1e-6))
assert relative > 0.05, f'Samples is ineffective for curved approximation: {relative}'
print(f'VECTOR_BLUR_DIRECTIONAL_OK {DEVICE} sample_delta={delta:.6f} '
      f'stock_error={error:.6g} curve_delta={relative:.6f}', flush=True)
