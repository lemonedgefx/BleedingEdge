# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""IBK compositor regression tests with analytic foreground-over-screen fixtures.

Run: blender -b --factory-startup --python-exit-code 1 --python compositor_ibk_test.py
     -- --device BOTH
"""

import argparse
from array import array
import math
import os
import sys
import tempfile

import bpy


WIDTH = 53
HEIGHT = 29
TOLERANCE = 0.004


def assert_near(actual, expected, label, tolerance=TOLERANCE):
    assert math.isfinite(actual) and abs(actual - expected) <= tolerance, (
        f'{label}: expected {expected:.6f}, got {actual:.6f}')


def make_image(name, color_at):
    image = bpy.data.images.new(name, WIDTH, HEIGHT, alpha=True, float_buffer=True)
    image.colorspace_settings.name = 'Non-Color'
    image.alpha_mode = 'PREMUL'
    pixels = array('f')
    for y in range(HEIGHT):
        for x in range(WIDTH):
            pixels.extend(color_at(x, y))
    image.pixels.foreach_set(pixels)
    image.update()
    return image


def pixel(pixels, x, y):
    index = (y * WIDTH + x) * 4
    return pixels[index:index + 4]


def coverage_at(x, _y):
    return (0.0, 0.25, 0.5, 0.75, 1.0)[min(4, x * 5 // WIDTH)]


def plate_color(x, y, channel=1):
    color = [0.05, 0.05, 0.05]
    color[channel] = 0.35 + 0.55 * x / (WIDTH - 1) + 0.15 * y / (HEIGHT - 1)
    return tuple(color)


def fixtures(tag, channel=1, opacity=1.0, plate_alpha=1.0, exposure=0.0,
             coverage=coverage_at, foreground=0.3):
    def clean(x, y):
        screen = plate_color(x, y, channel)
        return (*(value * plate_alpha / (2.0 ** exposure) for value in screen), plate_alpha)

    def source(x, y):
        screen = plate_color(x, y, channel)
        alpha = coverage(x, y)
        return (*(opacity * (alpha * foreground + (1.0 - alpha) * value)
                  for value in screen), opacity)

    return make_image(tag + '_source', source), make_image(tag + '_plate', clean)


class Harness:
    def __init__(self, device, directory):
        self.device = device
        self.directory = directory
        self.results = {}
        self.scene = bpy.context.scene
        self.scene.render.engine = 'BLENDER_WORKBENCH'
        self.scene.render.compositor_device = device
        self.scene.render.resolution_x = WIDTH
        self.scene.render.resolution_y = HEIGHT
        self.scene.render.resolution_percentage = 100
        self.scene.render.image_settings.file_format = 'OPEN_EXR'
        self.scene.render.image_settings.color_depth = '32'
        self.scene.render.image_settings.color_mode = 'RGBA'
        self.scene.view_settings.view_transform = 'Raw'
        self.scene.render.film_transparent = True

    def render(self, tag, source=None, plate=None, output='Matte', settings=None,
               garbage=None, core=None, node_type='CompositorNodeIBK'):
        tree = bpy.data.node_groups.new('IBK_' + tag, 'CompositorNodeTree')
        tree.interface.new_socket('Image', in_out='OUTPUT', socket_type='NodeSocketColor')
        self.scene.compositing_node_group = tree
        group_output = tree.nodes.new('NodeGroupOutput')
        keyer = tree.nodes.new(node_type)

        def connect(value, socket):
            if isinstance(value, bpy.types.Image):
                image_node = tree.nodes.new('CompositorNodeImage')
                image_node.image = value
                tree.links.new(image_node.outputs['Image'], keyer.inputs[socket])
            elif value is not None:
                keyer.inputs[socket].default_value = value

        connect(source, 'Mask' if node_type == 'CompositorNodeDilateErode' else 'Image')
        connect(plate, 'Clean Plate' if node_type == 'CompositorNodeIBK' else 'Key Color')
        connect(garbage, 'Garbage Matte')
        connect(core, 'Core Matte')
        for name, value in (settings or {}).items():
            keyer.inputs[name].default_value = value
        tree.links.new(keyer.outputs[output], group_output.inputs['Image'])

        path = os.path.join(self.directory, self.device + '_' + tag + '.exr')
        self.scene.render.filepath = path
        bpy.ops.render.render(write_still=True)
        result = bpy.data.images.load(path, check_existing=False)
        result.alpha_mode = 'PREMUL'
        assert tuple(result.size) == (WIDTH, HEIGHT), (tag, tuple(result.size))
        pixels = array('f', [0.0]) * (WIDTH * HEIGHT * 4)
        result.pixels.foreach_get(pixels)
        assert all(math.isfinite(value) for value in pixels), tag + ': non-finite output'
        self.results[tag] = pixels
        bpy.data.images.remove(result)
        print(f'IBK_CASE_OK {self.device} {tag}')
        return pixels


def test_device(harness):
    # Known alpha and foreground colors must survive a spatially varying screen, including HDR.
    for channel, mode in ((1, 'Auto'), (1, 'Green'), (2, 'Auto'), (2, 'Blue'), (0, 'Auto')):
        tag = f'gradient_{channel}_{mode}'
        source, plate = fixtures(tag, channel=channel)
        settings = {'Screen': mode}
        matte = harness.render(tag + '_matte', source, plate, settings=settings)
        image = harness.render(tag + '_image', source, plate, output='Image', settings=settings)
        for y in range(HEIGHT):
            for x in range(WIDTH):
                alpha = coverage_at(x, y)
                assert_near(pixel(matte, x, y)[0], alpha, tag + ': coverage')
                actual = pixel(image, x, y)
                assert_near(actual[3], alpha, tag + ': image alpha')
                for value in actual[:3]:
                    assert_near(value, alpha * 0.3, tag + ': recovered foreground')

    source, plate = fixtures('premultiplied', opacity=0.4, plate_alpha=0.5)
    matte = harness.render('premultiplied_matte', source, plate)
    image = harness.render('premultiplied_image', source, plate, output='Image')
    for x in range(WIDTH):
        alpha = coverage_at(x, 0) * 0.4
        assert_near(pixel(matte, x, 0)[0], alpha, 'existing alpha is respected')
        actual = pixel(image, x, 0)
        assert_near(actual[3], alpha, 'image alpha matches Matte')
        for value in actual[:3]:
            assert_near(value, alpha * 0.3, 'premultiplied foreground recovery')

    source, plate = fixtures('exposure', exposure=1.0)
    matte = harness.render('plate_exposure', source, plate, settings={'Plate Exposure': 1.0})
    for x in range(WIDTH):
        assert_near(pixel(matte, x, 0)[0], coverage_at(x, 0), 'plate exposure match')

    # Constant inputs must also key; constant footage with image inputs must use the image domain.
    screen = (0.05, 0.85, 0.05, 1.0)
    mixed = (0.175, 0.575, 0.175, 1.0)
    matte = harness.render('constant_matte', mixed, screen)
    assert_near(pixel(matte, 0, 0)[0], 0.5, 'constant key')
    assert_near(pixel(matte, WIDTH - 1, HEIGHT - 1)[0], 0.5, 'constant broadcast')
    _, plate = fixtures('constant_against_image')
    matte = harness.render('constant_against_image', (0.3, 0.3, 0.3, 1.0), plate)
    assert_near(min(matte[0::4]), 1.0, 'constant foreground / varying plate')

    garbage = make_image('garbage', lambda x, y: (float(x < WIDTH // 2),) * 3 + (1.0,))
    core = make_image('core', lambda x, y: (float(x < 5),) * 3 + (1.0,))
    matte = harness.render('image_masks', mixed, screen, garbage=garbage, core=core)
    for x in range(WIDTH):
        expected = 1.0 if x < 5 else (0.0 if x < WIDTH // 2 else 0.5)
        assert_near(pixel(matte, x, HEIGHT // 2)[0], expected, 'image masks and precedence')
    matte = harness.render('scalar_mask', mixed, screen, garbage=1.0)
    assert_near(max(matte[0::4]), 0.0, 'scalar garbage mask')

    # Levels must read independent controls, and equal levels must remain finite.
    matte = harness.render('white_level', mixed, screen,
                           settings={'Black Level': 0.1, 'White Level': 0.9})
    assert_near(pixel(matte, 0, 0)[0], 0.5, 'independent black and white levels')
    matte = harness.render('white_clipping', mixed, screen, settings={'White Level': 0.5})
    assert_near(pixel(matte, 0, 0)[0], 1.0, 'white clipping')
    matte = harness.render('equal_levels', mixed, screen,
                           settings={'Black Level': 0.5, 'White Level': 0.5})
    assert_near(pixel(matte, 0, 0)[0], 0.5, 'equal levels do not divide by zero')

    for tag, plate in (('black_plate', (0.0, 0.0, 0.0, 1.0)),
                       ('neutral_plate', (0.3, 0.3, 0.3, 1.0)),
                       ('transparent_plate', (0.0, 0.0, 0.0, 0.0)),
                       ('missing_plate', None)):
        matte = harness.render(tag, (0.08, 0.12, 0.16, 0.4), plate)
        assert_near(pixel(matte, 0, 0)[0], 0.4, tag + ': safe foreground fallback')
    matte = harness.render('transparent_source', (0.0, 0.0, 0.0, 0.0), screen, core=1.0)
    assert_near(max(matte[0::4]), 0.0, 'core cannot revive transparent input')

    source, plate = fixtures('edge', coverage=lambda x, y: float(x >= WIDTH // 2))
    edges = harness.render('edges_only', source, plate, output='Edges')
    assert_near(pixel(edges, WIDTH // 2, HEIGHT // 2)[0], 1.0, 'edge detection')
    assert_near(pixel(edges, 3, HEIGHT // 2)[0], 0.0, 'flat background is not an edge')
    assert_near(pixel(edges, WIDTH - 4, HEIGHT // 2)[0], 0.0, 'flat foreground is not an edge')
    dilated = harness.render('dilate', source, plate, settings={'Postprocess Dilate Size': 3})
    eroded = harness.render('erode', source, plate, settings={'Postprocess Dilate Size': -3})
    assert_near(pixel(dilated, WIDTH // 2 - 2, HEIGHT // 2)[0], 1.0, 'positive dilation expands')
    assert_near(pixel(eroded, WIDTH // 2 + 1, HEIGHT // 2)[0], 0.0, 'negative dilation erodes')
    blurred = harness.render('blur', source, plate, settings={'Postprocess Blur Size': 4})
    feathered = harness.render('feather', source, plate, settings={'Postprocess Feather Size': 4})
    assert 0.0 < pixel(blurred, WIDTH // 2, HEIGHT // 2)[0] < 1.0, 'blur softens the edge'
    assert 0.0 < pixel(feathered, WIDTH // 2 - 1, HEIGHT // 2)[0] < 1.0, 'feather softens the edge'
    harness.render('plate_and_input_blur', source, plate,
                   settings={'Plate Blur Size': 3, 'Input Blur Size': 3})

    source, plate = fixtures('despill', coverage=lambda x, y: 0.5)
    image = harness.render('despill_disabled', source, plate, output='Image',
                           settings={'Despill Strength': 0.0})
    x = WIDTH // 2
    original = tuple(0.5 * 0.3 + 0.5 * value for value in plate_color(x, 0))
    for actual, value in zip(pixel(image, x, 0)[:3], original):
        assert_near(actual, 0.5 * value, 'despill disabled applies only alpha')

    # Refactoring cleanup must not change Keying's image-mask, level, or edge behavior.
    neutral = make_image('legacy_neutral', lambda x, y: (0.3, 0.3, 0.3, 1.0))
    matte = harness.render('legacy_garbage', neutral, screen, garbage=garbage,
                           node_type='CompositorNodeKeying')
    assert_near(pixel(matte, 2, HEIGHT // 2)[0], 0.0, 'Keying image garbage mask')
    assert_near(pixel(matte, WIDTH - 3, HEIGHT // 2)[0], 1.0, 'Keying retains foreground')
    key_source = make_image('legacy_partial', lambda x, y: (0.05, 0.45, 0.05, 1.0))
    raw = harness.render('legacy_partial', key_source, screen, node_type='CompositorNodeKeying')
    clipped = harness.render('legacy_white_level', key_source, screen,
                             settings={'White Level': 0.5, 'Edge Search Size': 0},
                             node_type='CompositorNodeKeying')
    assert 0.1 < pixel(raw, 0, 0)[0] < 0.9, 'Keying retains a fractional matte'
    assert_near(pixel(clipped, 0, 0)[0], 1.0, 'Keying White Level clipping')

    # The shared distance shader also serves the regular Dilate/Erode node. Exercise asymmetric
    # windows at image borders, where horizontal and vertical window limits differ.
    corner_mask = make_image('corner_mask', lambda x, y: (
        (float(x >= WIDTH // 2 and y >= HEIGHT // 2),) * 3 + (1.0,)))
    for distance in (3, -3):
        result = harness.render(f'distance_node_{distance}', corner_mask, output='Mask',
                                settings={'Type': 'Distance', 'Size': distance},
                                node_type='CompositorNodeDilateErode')
        assert_near(pixel(result, WIDTH - 1, HEIGHT - 1)[0], 1.0, 'retain the corner interior')
        if distance > 0:
            assert_near(pixel(result, WIDTH // 2 - 2, HEIGHT // 2 - 2)[0], 1.0,
                        'circular dilation reaches a diagonal neighbor')
        else:
            assert_near(pixel(result, WIDTH // 2 + 1, HEIGHT // 2 + 1)[0], 0.0,
                        'circular erosion removes a boundary neighbor')
    return harness.results


def test_save_reload(directory):
    tree = bpy.data.node_groups.new('IBK_persistence', 'CompositorNodeTree')
    tree.use_fake_user = True
    node = tree.nodes.new('CompositorNodeIBK')
    node.inputs['Screen'].default_value = 'Blue'
    node.inputs['Plate Exposure'].default_value = 1.25
    node.inputs['Postprocess Dilate Size'].default_value = -2
    node.inputs['Despill Strength'].default_value = 0.75
    path = os.path.join(directory, 'ibk_persistence.blend')
    bpy.ops.wm.save_as_mainfile(filepath=path)
    bpy.ops.wm.open_mainfile(filepath=path, load_ui=False)
    node = bpy.data.node_groups['IBK_persistence'].nodes[0]
    assert node.bl_idname == 'CompositorNodeIBK'
    assert node.inputs['Screen'].default_value == 'Blue'
    assert_near(node.inputs['Plate Exposure'].default_value, 1.25, 'saved exposure')
    assert node.inputs['Postprocess Dilate Size'].default_value == -2
    assert_near(node.inputs['Despill Strength'].default_value, 0.75, 'saved despill')
    print('IBK_SAVE_RELOAD_OK')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--device', choices=('CPU', 'GPU', 'BOTH'), default='BOTH')
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
    assert hasattr(bpy.types, 'CompositorNodeIBK'), 'IBK is not registered'
    with tempfile.TemporaryDirectory(prefix='blender_ibk_') as directory:
        devices = ('CPU', 'GPU') if args.device == 'BOTH' else (args.device,)
        results = {device: test_device(Harness(device, directory)) for device in devices}
        if args.device == 'BOTH':
            for tag, cpu in results['CPU'].items():
                gpu = results['GPU'][tag]
                error = max(abs(first - second) for first, second in zip(cpu, gpu))
                if error > TOLERANCE:
                    indices = [index for index, (first, second) in enumerate(zip(cpu, gpu))
                               if abs(first - second) > TOLERANCE]
                    details = [(index // 4 % WIDTH, index // 4 // WIDTH, index % 4,
                                round(cpu[index], 5), round(gpu[index], 5)) for index in indices[:12]]
                    raise AssertionError(f'{tag}: CPU/GPU error {error:.6f}, '
                                         f'{len(indices)} differing channels; {details}')
            print(f'IBK_CPU_GPU_PARITY_OK {len(results["CPU"])} cases')
        test_save_reload(directory)
    print('COMPOSITOR_IBK_TEST_OK')


if __name__ == '__main__':
    main()
