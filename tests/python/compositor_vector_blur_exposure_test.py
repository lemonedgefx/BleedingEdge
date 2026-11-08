"""Exposure averaging, per-source falloff, and shutter continuity regressions."""

import os
import tempfile

import bpy
import numpy as np


DEVICE = os.environ.get('VB_DEVICE', 'CPU')
OUTPUT = os.environ.get('VB_OUTPUT') or tempfile.mkdtemp(prefix='vector_blur_exposure_')
os.makedirs(OUTPUT, exist_ok=True)
SIZE = 256
scene = bpy.context.scene
scene.render.compositor_device = DEVICE
scene.render.resolution_x = scene.render.resolution_y = SIZE
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.image_settings.color_depth = '32'
scene.render.image_settings.color_mode = 'RGBA'
scene.view_settings.view_transform = 'Raw'
failures = []


def check(name, condition, detail):
    print(f"EXPOSURE_{'OK' if condition else 'FAIL'} {DEVICE} {name}: {detail}", flush=True)
    if not condition:
        failures.append(name)


def image(name, array):
    h, w = array.shape[:2]
    result = bpy.data.images.new(name, w, h, alpha=True, float_buffer=True)
    result.alpha_mode = 'PREMUL'
    result.pixels.foreach_set(array.astype(np.float32).ravel())
    result.update()
    return result


def render(source, speed, tag, shutter=2, samples=32, curved=False, extended=None, falloff='Smooth', depth=None):
    tree = bpy.data.node_groups.new(tag, 'CompositorNodeTree')
    tree.interface.new_socket('Image', in_out='OUTPUT', socket_type='NodeSocketColor')
    scene.compositing_node_group = tree
    scene.render.resolution_x, scene.render.resolution_y = source.size
    node = tree.nodes.new('CompositorNodeVecBlur')
    node.inputs['Samples'].default_value = samples
    node.inputs['Shutter'].default_value = shutter
    node.inputs['Curved'].default_value = curved
    node.inputs['Falloff'].default_value = falloff
    for socket, value in [('Image', source), ('Speed', speed), ('Speed Extended', extended), ('Depth', depth)]:
        if value is None:
            continue
        upstream = tree.nodes.new('CompositorNodeImage')
        upstream.image = value
        tree.links.new(upstream.outputs['Image'], node.inputs[socket])
    output = tree.nodes.new('NodeGroupOutput')
    tree.links.new(node.outputs['Image'], output.inputs['Image'])
    path = os.path.join(OUTPUT, tag + '.exr')
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    result = bpy.data.images.load(path)
    array = np.empty(source.size[0] * source.size[1] * 4, dtype=np.float32)
    result.pixels.foreach_get(array)
    return array.reshape(source.size[1], source.size[0], 4)


source_values = np.zeros((SIZE, SIZE, 4), dtype=np.float32)
source_values[:, :, 3] = 1
source_values[126:131, 126:131, :3] = 1
speed_values = np.zeros_like(source_values)
speed_values[126:131, 126:131, 0] = -60
speed_values[126:131, 126:131, 2] = -60
source = image('opaque_foreground', source_values)
speed = image('foreground_speed', speed_values)
result = render(source, speed, 'softened_original')
check('directional envelope starts near source opacity', float(result[128, 128, 0]) > 0.8,
      f"center={result[128, 128, 0]:.6f}")
check('foreground has a visible tapered trail', float(result[128, 160, 0]) > 0.005,
      f"trail={result[128, 160, 0]:.6f}")
check('opaque background retains alpha', np.max(np.abs(result[:, :, 3] - 1)) < 1e-5,
      f"alpha error={np.max(np.abs(result[:, :, 3] - 1)):.6g}")

# Both sources are in the same 32-pixel tile, with a fourfold difference in speed.
source_values[:] = 0
source_values[140:145, 126:131, :] = 1
source_values[152:157, 126:131, :] = 1
speed_values[:] = 0
speed_values[140:145, 126:131, 0] = speed_values[140:145, 126:131, 2] = -20
speed_values[152:157, 126:131, 0] = speed_values[152:157, 126:131, 2] = -80
source = image('different_speed_markers', source_values)
speed = image('different_speeds', speed_values)
result = render(source, speed, 'per_source_falloff', samples=128)
slow = float(result[142, 143, 0] / max(result[142, 133, 0], 1e-6))
fast = float(result[154, 188, 0] / max(result[154, 148, 0], 1e-6))
check('short and long paths each have their own falloff', abs(slow - fast) < 0.12 and slow < 0.4,
      f"normalized far/near ratios={slow:.6f}, {fast:.6f}")

source_values[:] = 0
source_values[127:130, 127:130, :] = 1
speed_values[:] = 0
speed_values[127:130, 127:130, 0] = speed_values[127:130, 127:130, 2] = -60
source = image('thin_marker', source_values)
speed = image('thin_marker_speed', speed_values)
# Directional filtering now obeys the requested sample count. Use enough samples
# to cover this thin source; sparse sampling is deliberately a lower quality option.
a = render(source, speed, 'shutter_3', shutter=3, samples=64)
b = render(source, speed, 'shutter_3_01', shutter=3.01, samples=64)
trail = a[128, 139:208, 0]
check('long trails have no missing slices', float(trail.min()) > 1e-5,
      f"minimum={trail.min():.6g}, empty={int(np.count_nonzero(trail <= 1e-5))}")
relative_change = float(np.abs(a[:, :, :3] - b[:, :, :3]).sum() /
                        max(np.abs(a[:, :, :3]).sum(), 1e-6))
check('small shutter changes remain continuous', relative_change < 0.03,
      f"relative change={relative_change:.6f}")

# A future-side quadratic reverses after one frame and passes its starting position
# after two frames. Direction/chord tests must not erase the returning section.
speed_values[127:130, 127:130, 0] = -36
speed_values[127:130, 127:130, 2] = -12
returning = image('reversing_speed', speed_values)
a = render(source, returning, 'curved_shutter_4', shutter=4, curved=True, samples=8)
b = render(source, returning, 'curved_shutter_4_01', shutter=4.01, curved=True, samples=8)
check('curved trails continue above shutter two', float(a[128, 78:121, 0].min()) > 1e-5,
      f"minimum={a[128, 78:121, 0].min():.6g}")
relative_change = float(np.abs(a[:, :, :3] - b[:, :, :3]).sum() /
                        max(np.abs(a[:, :, :3]).sum(), 1e-6))
check('curved shutter changes remain continuous', relative_change < 0.04,
      f"relative change={relative_change:.6f}")

# A fully moving opaque image must blur its texture, including its sharp marker.
source_values[:] = 0
source_values[:, :, 3] = 1
source_values[127:130, 127:130, :3] = 1
speed_values[:] = (-48, 0, -48, 0)
source = image('camera_motion_image', source_values)
speed = image('camera_motion_field', speed_values)
a = render(source, speed, 'camera_motion', falloff='None')
check('camera motion blurs the entire image', float(a[128, 128, 0]) < 0.1 and a[128, 148, 0] > 0.005,
      f"center={a[128, 128, 0]:.6f}, trail={a[128, 148, 0]:.6f}")
check('camera motion stays opaque', np.max(np.abs(a[:, :, 3] - 1)) < 1e-5,
      f"alpha error={np.max(np.abs(a[:, :, 3] - 1)):.6g}")

# Uneven dimensions and an HDR, translucent source cover the border and alpha paths.
h, w = 97, 129
source_values = np.zeros((h, w, 4), dtype=np.float32)
source_values[46:51, 61:66] = (4, 1, 0.25, 0.5)
speed_values = np.zeros_like(source_values)
speed_values[46:51, 61:66, 0] = speed_values[46:51, 61:66, 2] = -35
source = image('hdr_translucent', source_values)
speed = image('hdr_speed', speed_values)
a = render(source, speed, 'hdr_odd_dimensions')
check('HDR exposure retains premultiplied ratios',
      np.max(np.abs(a[:, :, 0] - a[:, :, 3] * 8)) < 1e-4 and np.all(np.isfinite(a)),
      f"ratio error={np.max(np.abs(a[:, :, 0] - a[:, :, 3] * 8)):.6g}")

# A farther moving source must not draw through an opaque stationary foreground.
source_values = np.zeros((SIZE, SIZE, 4), dtype=np.float32)
source_values[:, :, 3] = 1
source_values[127:130, 127:130, :3] = 1
source_values[110:150, 155:175, 2] = 1
speed_values = np.zeros_like(source_values)
speed_values[127:130, 127:130, 0] = speed_values[127:130, 127:130, 2] = -60
depth_values = np.ones_like(source_values)
depth_values[:, :, :3] = 100
depth_values[127:130, 127:130, :3] = 10
depth_values[110:150, 155:175, :3] = 1
source = image('occluded_source', source_values)
speed = image('occluded_speed', speed_values)
depth = image('depth', depth_values)
a = render(source, speed, 'stationary_occlusion', depth=depth)
check('stationary foreground occludes farther trails',
      np.max(np.abs(a[110:150, 155:175] - source_values[110:150, 155:175])) < 1e-5,
      f"occluder error={np.max(np.abs(a[110:150, 155:175] - source_values[110:150, 155:175])):.6g}")

assert not failures, f"Exposure regressions: {', '.join(failures)}"
print(f"VECTOR_BLUR_EXPOSURE_OK {DEVICE}", flush=True)
