# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Analytical fixtures for Color Match, Film Grain, and Relight.

Native tests (requires an executable containing these nodes):
  blender -b --factory-startup --python-exit-code 1 --python compositor_artist_nodes_test.py
  blender -b --factory-startup --python-exit-code 1 --python compositor_artist_nodes_test.py -- --device BOTH

Source contracts and independent mathematical reference checks only (does not test native nodes):
  python compositor_artist_nodes_test.py --reference-only
"""

import argparse
from array import array
import math
import os
from pathlib import Path
import re
import sys
import tempfile


WIDTH, HEIGHT = 37, 23
TOLERANCE = 0.005
LUMA = (0.2126, 0.7152, 0.0722)
UINT_MASK = 0xffffffff


def near(actual, expected, label, tolerance=TOLERANCE):
    assert math.isfinite(actual) and abs(actual - expected) <= tolerance, (
        f'{label}: expected {expected:.8f}, got {actual:.8f}')


def near_pixels(actual, expected, label, tolerance=TOLERANCE):
    assert len(actual) == len(expected), label + ': size differs'
    for index, (value, wanted) in enumerate(zip(actual, expected)):
        near(value, wanted, f'{label} pixel {index // 4}, channel {index % 4}', tolerance)


def flatten(pixels):
    return array('f', (value for pixel in pixels for value in pixel))


def statistics(pixels, masks, luminance=False):
    usable = []
    for color, mask in zip(pixels, masks):
        if not (color[3] > 1e-6 and math.isfinite(color[3]) and math.isfinite(mask) and mask > 0):
            continue
        value = tuple(channel / color[3] for channel in color[:3])
        if not all(abs(channel) < 1e10 for channel in value):
            continue
        if luminance:
            value = (sum(channel * coefficient for channel, coefficient in zip(value, LUMA)),) * 3
        usable.append((value, min(1, mask) * min(1, color[3])))
    total = sum(weight for _value, weight in usable)
    if total == 0:
        return None
    mean = tuple(sum(value[c] * weight for value, weight in usable) / total for c in range(3))
    deviation = tuple(math.sqrt(sum((value[c] - mean[c]) ** 2 * weight
                                    for value, weight in usable) / total) for c in range(3))
    return mean, deviation


def match_correction(source, reference, source_mask, reference_mask, mode='Color', contrast=1):
    source_stats = statistics(source, source_mask, mode != 'Color')
    reference_stats = statistics(reference, reference_mask, mode != 'Color')
    if source_stats is None or reference_stats is None:
        return (1, 1, 1), (0, 0, 0), False
    mean, deviation = source_stats
    target_mean, target_deviation = reference_stats
    if mode == 'Exposure':
        if mean[0] <= 1e-6 or target_mean[0] < 0:
            return (1, 1, 1), (0, 0, 0), False
        return (min(10000, target_mean[0] / mean[0]),) * 3, (0, 0, 0), True
    gain = tuple(1 + contrast * ((min(10000, target_deviation[c] / deviation[c])
                                 if deviation[c] > 1e-6 * max(1, abs(mean[c])) else 1) - 1)
                 for c in range(3))
    offset = tuple(target_mean[c] - mean[c] * gain[c] for c in range(3))
    return gain, offset, True


def matched(color, gain, offset, factor=1):
    return (*(color[c] + factor * (color[c] * (gain[c] - 1) + offset[c] * color[3])
              for c in range(3)), color[3])


def grain_hash(value):
    value &= UINT_MASK
    value = ((value ^ (value >> 16)) * 0x7feb352d) & UINT_MASK
    value = ((value ^ (value >> 15)) * 0x846ca68b) & UINT_MASK
    return value ^ (value >> 16)


def grain_cell(x, y, seed):
    base = grain_hash((x & UINT_MASK) ^ grain_hash((y + 0x9e3779b9) & UINT_MASK) ^ seed)
    return (sum((grain_hash((base + salt) & UINT_MASK) >> 8) / 16777216
                for salt in (0, 0x9e3779b9, 0x3c6ef372, 0xdaa66d2b)) - 2) * math.sqrt(3)


def grain_noise(x, y, size, seed):
    x, y = x / size - 0.5, y / size - 0.5
    ix, iy = math.floor(x), math.floor(y)
    x, y = x - ix, y - iy
    x, y = x * x * (3 - 2 * x), y * y * (3 - 2 * y)
    weights = ((1 - x) * (1 - y), x * (1 - y), (1 - x) * y, x * y)
    cells = ((ix, iy), (ix + 1, iy), (ix, iy + 1), (ix + 1, iy + 1))
    return sum(weight * grain_cell(cx, cy, seed) for weight, (cx, cy) in zip(weights, cells)) / math.sqrt(
        sum(weight * weight for weight in weights))


def grained(color, x, y, amount=0.02, size=(1, 1, 1), chroma=1, response=(1, 1, 0.25),
            pivot=0.18, seed=0, animate=True, frame=1, mask=1):
    if color[3] <= 1e-6 or amount == 0 or mask <= 0:
        return color
    seed = (seed ^ (grain_hash(frame) if animate else 0)) & 0x7fffffff
    luminance = max(0, sum(color[c] / color[3] * LUMA[c] for c in range(3)))
    t = luminance / (luminance + pivot)
    envelope = sum(weight * value for weight, value in zip(((1 - t)**2, 2 * t * (1 - t), t*t), response))
    mono = grain_noise(x + 0.5, y + 0.5, size[0], seed)
    noise = (mono, grain_noise(x + 0.5, y + 0.5, size[1], grain_hash(seed + 1)),
             grain_noise(x + 0.5, y + 0.5, size[2], grain_hash(seed + 2)))
    return (*(color[c] + (mono + chroma * (noise[c] - mono)) * amount * envelope * color[3] * mask
              for c in range(3)), color[3])


def lighting(color, normal, position=(0, 0, 0), diffuse=(1, 1, 1, 1), mask=1,
             light_type='Directional', light_color=(1, 1, 1), strength=1, direction=(0, 0, 1),
             light_position=(0, 0, 5), falloff=True, minimum_distance=0.1):
    zero = (0, 0, 0, color[3])
    length = math.sqrt(sum(value * value for value in normal))
    if color[3] <= 1e-6 or mask <= 0 or not math.isfinite(length) or length <= 1e-6:
        return zero
    attenuation = 1
    if light_type == 'Point':
        direction = tuple(light_position[c] - position[c] for c in range(3))
        if falloff:
            attenuation = 1 / max(sum(value * value for value in direction), minimum_distance ** 2)
    direction_length = math.sqrt(sum(value * value for value in direction))
    if not math.isfinite(direction_length) or direction_length <= 1e-6:
        return zero
    lambert = max(0, sum(normal[c] * direction[c] for c in range(3)) / (length * direction_length))
    return (*(diffuse[c] / diffuse[3] * light_color[c] * strength * lambert * attenuation * mask * color[3]
              if diffuse[3] > 1e-6 else 0 for c in range(3)), color[3])


def test_reference_contracts():
    """Validate the oracle and wiring contracts, not the unbuilt C++ implementation."""
    root = Path(__file__).resolve().parents[2]
    node_dir = root / 'source/blender/nodes/composite'
    compositor = root / 'source/blender/compositor'
    rna = (root / 'source/blender/makesrna/intern/rna_nodetree.cc').read_text(encoding='utf-8')
    menu = (root / 'scripts/startup/bl_ui/node_add_menu_compositor.py').read_text(encoding='utf-8')
    cmake = (node_dir / 'CMakeLists.txt').read_text(encoding='utf-8')
    shader_cmake = (compositor / 'CMakeLists.txt').read_text(encoding='utf-8')
    shader_cpp_cmake = (compositor / 'shaders/CMakeLists.txt').read_text(encoding='utf-8')
    for filename, name in (('color_match', 'ColorMatch'), ('film_grain', 'FilmGrain'), ('relight', 'Relight')):
        node_source = (node_dir / f'nodes/node_composite_{filename}.cc').read_text(encoding='utf-8')
        assert f'nodes/node_composite_{filename}.cc' in cmake
        assert f'nodes/node_composite_{filename}.cc' in cmake
        assert f'"CompositorNode{name}"' in node_source
        assert f'define("CompositorNode", "CompositorNode{name}")' in rna
        assert f'"CompositorNode{name}"' in menu
        info_name = f'compositor_{filename}_infos.hh'
        info = (compositor / 'shaders/infos' / info_name).read_text(encoding='utf-8')
        assert f'shaders/infos/{info_name}' in shader_cmake
        for shader in re.findall(r'COMPUTE_SOURCE\("([^"]+)"\)', info):
            assert (compositor / 'shaders' / shader).is_file(), shader
            assert shader in shader_cmake and shader in shader_cpp_cmake, shader
        declared = set(re.findall(r'GPU_SHADER_CREATE_INFO\((\w+)\)', info))
        requested = set(re.findall(r'"(compositor_' + filename + r'\w*)"', node_source))
        assert requested <= declared, (requested, declared)
    dependencies = (root / 'source/blender/nodes/intern/dependencies.cc').read_text(encoding='utf-8')
    assert 'has_enabled_nodes_of_type(ntree, "CompositorNodeFilmGrain"_ustr)' in dependencies
    match_source = (node_dir / 'nodes/node_composite_color_match.cc').read_text(encoding='utf-8')
    assert 'WM_jobs_has_running' in match_source and 'G.is_rendering' in match_source
    assert match_source.index('WM_jobs_has_running') < match_source.index('get_contextual_tree_logs')
    cpu_grain = (node_dir / 'nodes/node_composite_film_grain.cc').read_text(encoding='utf-8')
    gpu_grain = (compositor / 'shaders/library/gpu_shader_compositor_film_grain.glsl').read_text(encoding='utf-8')
    for constant in ('0x7feb352d', '0x846ca68b', '0x9e3779b9', '0x3c6ef372', '0xdaa66d2b', '16777216.0f'):
        assert constant in cpu_grain and constant in gpu_grain, constant

    source = [(0.1, 0.2, 0.3, 1), (0.3, 0.5, 0.8, 1), (0.8, 0.7, 1.2, 1)]
    known_gain, known_offset = (2, 0.5, 1.2), (-0.1, 0.2, 0.3)
    reference = [matched(pixel, known_gain, known_offset) for pixel in source]
    gain, offset, valid = match_correction(source, reference, [1] * 3, [1] * 3)
    assert valid
    for actual, wanted in zip(gain + offset, known_gain + known_offset):
        near(actual, wanted, 'reference affine recovery', 1e-10)
    assert not match_correction(source, reference, [0] * 3, [1] * 3)[2]
    flat = statistics([(0.1, 0.2, 0.3, 0.5)] * 3, [1] * 3)
    assert all(value < 1e-15 for value in flat[1])
    ignored = statistics(source + [(float('nan'), 0, 0, 1), (1, 1, 1, 0)], [1] * 5)
    assert ignored == statistics(source, [1] * 3)

    samples = [grain_cell(x, y, 123) for y in range(128) for x in range(128)]
    mean = sum(samples) / len(samples)
    variance = sum((value - mean)**2 for value in samples) / len(samples)
    assert abs(mean) < 0.04 and abs(variance - 1) < 0.06, (mean, variance)
    for size in (0.5, 1, 2.5, 9):
        assert math.isfinite(grain_noise(10.2, -3.1, size, 12))
    assert grained((0.2, 0.2, 0.2, 1), 5, 3, seed=1) != grained((0.2, 0.2, 0.2, 1), 5, 3, seed=2)
    assert grained((0, 0, 0, 0), 5, 3) == (0, 0, 0, 0)
    near(lighting((0, 0, 0, 1), (0, 0, 5))[0], 1, 'reference normalized normal')
    near(lighting((0, 0, 0, 1), (0, 0, -1))[0], 0, 'reference back face')
    near(lighting((0, 0, 0, 1), (0, 0, 1), light_type='Point', light_position=(0, 0, 2))[0],
         0.25, 'reference inverse-square falloff')
    assert lighting((0, 0, 0, 1), (0, 0, 0)) == (0, 0, 0, 1)
    print('ARTIST_NODE_SOURCE_AND_REFERENCE_CONTRACTS_OK (not native runtime verification)')


def make_image(bpy, name, pixels, width=WIDTH, height=HEIGHT):
    image = bpy.data.images.new(name, width, height, alpha=True, float_buffer=True)
    image.colorspace_settings.name = 'Non-Color'
    image.alpha_mode = 'PREMUL'
    image.pixels.foreach_set(flatten(pixels))
    image.update()
    return image


class Harness:
    def __init__(self, bpy, device, directory):
        self.bpy, self.device, self.directory = bpy, device, directory
        self.scene = bpy.context.scene
        self.scene.render.engine = 'BLENDER_WORKBENCH'
        self.scene.render.compositor_device = device
        self.scene.render.resolution_x, self.scene.render.resolution_y = WIDTH, HEIGHT
        self.scene.render.resolution_percentage = 100
        self.scene.render.image_settings.file_format = 'OPEN_EXR'
        self.scene.render.image_settings.color_depth = '32'
        self.scene.render.image_settings.color_mode = 'RGBA'
        self.scene.view_settings.view_transform = 'Raw'
        self.scene.render.film_transparent = True
        self.scene.frame_set(1)
        self.results = {}

    def setup(self, node_type, inputs, output='Image', locked=False):
        bpy = self.bpy
        self.tree = bpy.data.node_groups.new('artist_fixture_' + node_type, 'CompositorNodeTree')
        self.tree.interface.new_socket('Image', in_out='OUTPUT', socket_type='NodeSocketColor')
        self.scene.compositing_node_group = self.tree
        self.node = self.tree.nodes.new(node_type)
        if node_type == 'CompositorNodeColorMatch':
            self.node.correction_locked = locked
        for name, value in inputs.items():
            if isinstance(value, bpy.types.Image):
                image = self.tree.nodes.new('CompositorNodeImage')
                image.image = value
                self.tree.links.new(image.outputs['Image'], self.node.inputs[name])
            else:
                self.node.inputs[name].default_value = value
        result = self.tree.nodes.new('NodeGroupOutput')
        self.tree.links.new(self.node.outputs[output], result.inputs['Image'])
        return self.node

    def render(self, tag, frame=1):
        self.scene.frame_set(frame)
        self.tree.update_tag()
        path = os.path.join(self.directory, self.device + '_' + tag + '.exr')
        self.scene.render.filepath = path
        self.bpy.ops.render.render(write_still=True)
        result = self.bpy.data.images.load(path, check_existing=False)
        result.alpha_mode = 'PREMUL'
        assert tuple(result.size) == (WIDTH, HEIGHT), (tag, tuple(result.size))
        pixels = array('f', [0]) * (WIDTH * HEIGHT * 4)
        result.pixels.foreach_get(pixels)
        assert all(math.isfinite(value) for value in pixels), tag + ': non-finite result'
        self.bpy.data.images.remove(result)
        self.results[tag] = pixels
        print(f'ARTIST_NODE_CASE_OK {self.device} {tag}')
        return pixels


def test_color_match(h):
    source = []
    for y in range(HEIGHT):
        for x in range(WIDTH):
            alpha = 0.4 if x else 0
            source.append(((0.1 + x / WIDTH) * alpha, (0.15 + y / HEIGHT) * alpha,
                           (0.2 + (x + y) / (WIDTH + HEIGHT)) * alpha, alpha))
    known_gain, known_offset = (0.8, 1.25, 1.6), (0.08, -0.03, 0.12)
    target = [matched(pixel, known_gain, known_offset) for pixel in source]
    image = make_image(h.bpy, 'match_source', source)
    reference = make_image(h.bpy, 'match_target', target)
    h.setup('CompositorNodeColorMatch', {'Image': image})
    near_pixels(h.render('match_missing_reference'), flatten(source), 'missing reference preserves image')
    h.setup('CompositorNodeColorMatch', {'Image': image, 'Reference': reference})
    near_pixels(h.render('match_rgb'), flatten(target), 'RGB affine match')
    h.node.inputs['Factor'].default_value = 0
    near_pixels(h.render('match_factor_zero'), flatten(source), 'zero factor')
    h.node.inputs['Factor'].default_value = 0.25
    near_pixels(h.render('match_factor_partial'), flatten([matched(pixel, known_gain, known_offset, 0.25)
                                                         for pixel in source]), 'partial factor')

    source_mask = [float((i % WIDTH) < WIDTH // 2) for i in range(WIDTH * HEIGHT)]
    reference_mask = [float((i // WIDTH) > HEIGHT // 2) for i in range(WIDTH * HEIGHT)]
    application_mask = [float((i % WIDTH) >= WIDTH // 2) for i in range(WIDTH * HEIGHT)]
    gain, offset, valid = match_correction(source, target, source_mask, reference_mask)
    assert valid
    masks = {name: make_image(h.bpy, name, [(value, value, value, 1) for value in values])
             for name, values in (('Source Mask', source_mask), ('Reference Mask', reference_mask),
                                  ('Factor', application_mask))}
    h.setup('CompositorNodeColorMatch', {'Image': image, 'Reference': reference, **masks})
    near_pixels(h.render('match_independent_masks'), flatten([matched(pixel, gain, offset, factor)
                                                            for pixel, factor in zip(source, application_mask)]),
                'analysis masks independent of application')
    for name in ('Source Mask', 'Reference Mask'):
        h.setup('CompositorNodeColorMatch', {'Image': image, 'Reference': reference, name: 0})
        near_pixels(h.render('match_empty_' + name.replace(' ', '_')), flatten(source), 'empty analysis')
        h.setup('CompositorNodeColorMatch', {'Image': image, 'Reference': reference, name: 0}, output='Valid')
        near(h.render('match_invalid_' + name.replace(' ', '_'))[0], 0, 'empty region validity')

    gain, offset, _valid = match_correction(source, target, [1] * len(source), [1] * len(source), contrast=0)
    h.setup('CompositorNodeColorMatch', {'Image': image, 'Reference': reference, 'Contrast': 0})
    near_pixels(h.render('match_mean_only'), flatten([matched(pixel, gain, offset) for pixel in source]),
                'mean-only correction')
    flat = [(0.3, 0.4, 0.5, 1)] * (19 * 11)
    flat_image = make_image(h.bpy, 'different_reference_size', flat, 19, 11)
    h.setup('CompositorNodeColorMatch', {'Image': image, 'Reference': flat_image})
    near_pixels(h.render('match_different_resolution'),
                flatten([(*(channel * pixel[3] for channel in flat[0][:3]), pixel[3]) for pixel in source]),
                'independent reference resolution')
    h.setup('CompositorNodeColorMatch', {'Image': (0.1, 0.1, 0.1, 0.5), 'Reference': flat_image})
    near_pixels(h.render('match_flat_source'), flatten([(0.15, 0.2, 0.25, 0.5)] * len(source)),
                'flat source does not divide by zero')
    hdr_image = make_image(h.bpy, 'flat_hdr_source', [(10000.1, 5000.2, 20000.3, 1)] * len(source))
    target_mean = statistics(target, [1] * len(target))[0]
    h.setup('CompositorNodeColorMatch', {'Image': hdr_image, 'Reference': reference})
    near_pixels(h.render('match_flat_hdr_source'), flatten([(*target_mean, 1)] * len(source)),
                'flat HDR source must not amplify reduction round-off')

    grey = [(0.1 + (i % WIDTH) / WIDTH,) * 3 + (1,) for i in range(len(source))]
    grey_image = make_image(h.bpy, 'grey_source', grey)
    for mode, off in (('Luminance', 0.1), ('Exposure', 0)):
        expected = [matched(pixel, (2, 2, 2), (off, off, off)) for pixel in grey]
        ref = make_image(h.bpy, 'grey_' + mode, expected)
        h.setup('CompositorNodeColorMatch', {'Image': grey_image, 'Reference': ref, 'Mode': mode})
        near_pixels(h.render('match_' + mode.lower()), flatten(expected), mode + ' correction')
    h.setup('CompositorNodeColorMatch', {'Image': image, 'Stored Gain': (2, 3, 4),
                                       'Stored Offset': (-0.1, 0.2, 0.3)}, locked=True)
    locked = flatten([matched(pixel, (2, 3, 4), (-0.1, 0.2, 0.3)) for pixel in source])
    near_pixels(h.render('match_locked_frame_1'), locked, 'stored correction')
    near_pixels(h.render('match_locked_frame_17', frame=17), locked, 'locked correction across frames')


def test_grain(h):
    source = [(0.18, 0.18, 0.18, 1)] * (WIDTH * HEIGHT)
    image = make_image(h.bpy, 'grain_source', source)
    h.setup('CompositorNodeFilmGrain', {'Image': image, 'Seed': 13})
    first = h.render('grain_seed_13')
    expected = [grained(pixel, i % WIDTH, i // WIDTH, seed=13) for i, pixel in enumerate(source)]
    near_pixels(first, flatten(expected), 'deterministic grain')
    near_pixels(h.render('grain_repeat'), first, 'repeat evaluation')
    second = h.render('grain_frame_2', frame=2)
    assert max(abs(a - b) for a, b in zip(first, second)) > 0.01, 'animation did not change grain'
    h.node.inputs['Animate'].default_value = False
    static = h.render('grain_static_1')
    near_pixels(h.render('grain_static_19', frame=19), static, 'static grain across frames')
    h.node.inputs['Seed'].default_value = 14
    assert max(abs(a - b) for a, b in zip(h.render('grain_seed_14'), static)) > 0.01

    h.setup('CompositorNodeFilmGrain', {'Image': image, 'Amount': 0})
    near_pixels(h.render('grain_amount_zero'), flatten(source), 'zero grain amount')
    h.setup('CompositorNodeFilmGrain', {'Image': image, 'Mask': 0})
    near_pixels(h.render('grain_mask_zero'), flatten(source), 'zero grain mask')
    h.setup('CompositorNodeFilmGrain', {'Image': image, 'Shadows': 0, 'Midtones': 0, 'Highlights': 0})
    near_pixels(h.render('grain_response_zero'), flatten(source), 'zero tonal response')
    h.setup('CompositorNodeFilmGrain', {'Image': (0.18, 0.18, 0.18, 1), 'Animate': False,
                                      'Red Size': 1, 'Green Size': 2.5, 'Blue Size': 4, 'Color': 0})
    mono = h.render('grain_constant_image')
    assert max(mono[0::4]) - min(mono[0::4]) > 0.02, 'constant image grain was broadcast'
    for r, g, b in zip(mono[0::4], mono[1::4], mono[2::4]):
        near(r, g, 'monochrome green', 0.0001)
        near(r, b, 'monochrome blue', 0.0001)
    sizes = (1, 2.5, 4)
    h.setup('CompositorNodeFilmGrain', {'Image': image, 'Red Size': 1, 'Green Size': 2.5,
                                      'Blue Size': 4, 'Animate': False,
                                      'Shadows': 1, 'Midtones': 1, 'Highlights': 1})
    near_pixels(h.render('grain_rgb_sizes'), flatten([grained(pixel, i % WIDTH, i // WIDTH, size=sizes,
                                                            animate=False, response=(1, 1, 1))
                                                    for i, pixel in enumerate(source)]), 'RGB grain sizes')
    alpha_source = [(0.09, 0.09, 0.09, 0.5) if i % WIDTH else (0, 0, 0, 0) for i in range(len(source))]
    alpha_image = make_image(h.bpy, 'grain_alpha', alpha_source)
    h.setup('CompositorNodeFilmGrain', {'Image': alpha_image, 'Seed': 13})
    near_pixels(h.render('grain_alpha'), flatten([grained(pixel, i % WIDTH, i // WIDTH, seed=13)
                                                for i, pixel in enumerate(alpha_source)]), 'grain premultiplication')


def test_relight(h):
    source = [(0.1, 0.2, 0.3, 1)] * (WIDTH * HEIGHT)
    image = make_image(h.bpy, 'relight_source', source)
    normals = [(0, 0, (2 if i % WIDTH < WIDTH // 2 else -2), 1) for i in range(len(source))]
    normal_image = make_image(h.bpy, 'relight_normals', normals)
    h.setup('CompositorNodeRelight', {'Image': image, 'Normal': normal_image})
    expected_light = [lighting(pixel, normal[:3]) for pixel, normal in zip(source, normals)]
    expected = [(*(pixel[c] + light[c] for c in range(3)), pixel[3])
                for pixel, light in zip(source, expected_light)]
    near_pixels(h.render('relight_directional'), flatten(expected), 'normalized directional light')
    h.setup('CompositorNodeRelight', {'Image': image, 'Normal': normal_image}, output='Lighting')
    near_pixels(h.render('relight_lighting_output'), flatten(expected_light), 'Lighting output')
    h.setup('CompositorNodeRelight', {'Image': image})
    near_pixels(h.render('relight_zero_normals'), flatten(source), 'missing normals preserve image')
    for name, value in (('Strength', 0), ('Mask', 0), ('Direction', (0, 0, 0))):
        h.setup('CompositorNodeRelight', {'Image': image, 'Normal': normal_image, name: value})
        near_pixels(h.render('relight_zero_' + name.lower()), flatten(source), name + ' zero')

    positions = [((i % WIDTH) / WIDTH, (i // WIDTH) / HEIGHT, 0, 1) for i in range(len(source))]
    position_image = make_image(h.bpy, 'relight_positions', positions)
    alpha_source = [(0.05, 0.1, 0.15, 0.5)] * len(source)
    alpha_image = make_image(h.bpy, 'relight_alpha', alpha_source)
    common = {'Image': alpha_image, 'Normal': (0, 0, 3), 'Position': position_image,
              'Type': 'Point', 'Light Position': (0, 0, 2), 'Light Color': (1, 0.5, 0.25, 1),
              'Diffuse Color': (0.2, 0.3, 0.4, 0.5), 'Mask': 0.5}
    for falloff in (False, True):
        h.setup('CompositorNodeRelight', {**common, 'Distance Falloff': falloff})
        expected = []
        for pixel, position in zip(alpha_source, positions):
            light = lighting(pixel, (0, 0, 3), position[:3], diffuse=(0.2, 0.3, 0.4, 0.5),
                             mask=0.5, light_type='Point', light_position=(0, 0, 2),
                             light_color=(1, 0.5, 0.25), falloff=falloff)
            expected.append((*(pixel[c] + light[c] for c in range(3)), pixel[3]))
        near_pixels(h.render('relight_point_' + str(falloff)), flatten(expected), 'point light')
    h.setup('CompositorNodeRelight', {'Image': image, 'Normal': (0, 0, 1), 'Type': 'Point',
                                    'Position': (0, 0, 2), 'Light Position': (0, 0, 2)})
    near_pixels(h.render('relight_point_coincident'), flatten(source), 'coincident point light')
    h.setup('CompositorNodeRelight', {'Image': image, 'Normal': (0, 0, 1), 'Type': 'Point',
                                    'Position': (0, 0, 0), 'Light Position': (0, 0, 0.01),
                                    'Minimum Distance': 0.5})
    near_pixels(h.render('relight_point_minimum_distance'),
                flatten([(4.1, 4.2, 4.3, 1)] * len(source)), 'minimum distance clamp')
    h.setup('CompositorNodeRelight', {'Image': (0.1, 0.2, 0.3, 0.5), 'Normal': (0, 0, 5),
                                    'Diffuse Color': (0.2, 0.3, 0.4, 1), 'Strength': 2, 'Mask': 0.5})
    near_pixels(h.render('relight_single_value'), flatten([(0.2, 0.35, 0.5, 0.5)] * len(source)),
                'constant value shading')
    h.setup('CompositorNodeRelight', {'Image': (0, 0, 0, 0), 'Normal': (0, 0, 1)})
    near_pixels(h.render('relight_transparent'), flatten([(0, 0, 0, 0)] * len(source)),
                'transparent surfaces stay transparent')


def test_persistence(bpy, directory):
    tree = bpy.data.node_groups.new('artist_nodes_persistence', 'CompositorNodeTree')
    tree.use_fake_user = True
    match = tree.nodes.new('CompositorNodeColorMatch')
    match.correction_locked = True
    match.inputs['Stored Gain'].default_value = (1.5, 2, 0.5)
    match.inputs['Stored Offset'].default_value = (-0.1, 0.2, 0.3)
    grain = tree.nodes.new('CompositorNodeFilmGrain')
    for name, value in zip(('Red Size', 'Green Size', 'Blue Size'), (1.2, 2.3, 3.4)):
        grain.inputs[name].default_value = value
    grain.inputs['Animate'].default_value = False
    grain.inputs['Seed'].default_value = 42
    relight = tree.nodes.new('CompositorNodeRelight')
    relight.inputs['Type'].default_value = 'Point'
    relight.inputs['Light Position'].default_value = (2, 3, 4)
    copied = tree.copy()
    next(node for node in copied.nodes if node.bl_idname == match.bl_idname).inputs['Stored Gain'].default_value = (3, 3, 3)
    near(match.inputs['Stored Gain'].default_value[0], 1.5, 'independent node copy')
    bpy.data.node_groups.remove(copied)
    path = os.path.join(directory, 'artist_nodes_persistence.blend')
    bpy.ops.wm.save_as_mainfile(filepath=path)
    bpy.ops.wm.open_mainfile(filepath=path, load_ui=False)
    saved = bpy.data.node_groups['artist_nodes_persistence']
    nodes = {node.bl_idname: node for node in saved.nodes}
    assert nodes['CompositorNodeColorMatch'].correction_locked
    for actual, wanted in zip(nodes['CompositorNodeColorMatch'].inputs['Stored Gain'].default_value, (1.5, 2, 0.5)):
        near(actual, wanted, 'saved locked gain')
    for actual, wanted in zip(nodes['CompositorNodeColorMatch'].inputs['Stored Offset'].default_value, (-0.1, 0.2, 0.3)):
        near(actual, wanted, 'saved locked offset')
    assert nodes['CompositorNodeFilmGrain'].inputs['Seed'].default_value == 42
    assert not nodes['CompositorNodeFilmGrain'].inputs['Animate'].default_value
    for name, wanted in zip(('Red Size', 'Green Size', 'Blue Size'), (1.2, 2.3, 3.4)):
        near(nodes['CompositorNodeFilmGrain'].inputs[name].default_value, wanted, 'saved RGB sizes')
    assert nodes['CompositorNodeRelight'].inputs['Type'].default_value == 'Point'
    assert tuple(nodes['CompositorNodeRelight'].inputs['Light Position'].default_value) == (2, 3, 4)
    print('ARTIST_NODE_PERSISTENCE_OK')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--reference-only', action='store_true')
    parser.add_argument('--device', choices=('CPU', 'GPU', 'BOTH'), default='CPU')
    arguments = (sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else
                 (sys.argv[1:] if '--reference-only' in sys.argv else []))
    args = parser.parse_args(arguments)
    if args.reference_only:
        test_reference_contracts()
        return

    import bpy
    for name in ('CompositorNodeColorMatch', 'CompositorNodeFilmGrain', 'CompositorNodeRelight'):
        assert hasattr(bpy.types, name), name + ' is not registered; use a rebuilt executable'
    with tempfile.TemporaryDirectory(prefix='blender_artist_nodes_') as directory:
        devices = ('CPU', 'GPU') if args.device == 'BOTH' else (args.device,)
        results = {}
        for device in devices:
            harness = Harness(bpy, device, directory)
            test_color_match(harness)
            test_grain(harness)
            test_relight(harness)
            results[device] = harness.results
        if args.device == 'BOTH':
            for tag, cpu in results['CPU'].items():
                near_pixels(results['GPU'][tag], cpu, tag + ' CPU/GPU parity')
            print(f'ARTIST_NODE_CPU_GPU_PARITY_OK {len(results["CPU"])} cases')
        test_persistence(bpy, directory)
    print('COMPOSITOR_ARTIST_NODES_TEST_OK')


if __name__ == '__main__':
    main()
