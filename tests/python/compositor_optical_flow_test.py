# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Optical Flow analytical render fixtures; no external test images are needed.

blender -b --factory-startup --python-exit-code 1 --python compositor_optical_flow_test.py -- --device BOTH
python compositor_optical_flow_test.py --source-only
"""

import argparse
from array import array
import math
from pathlib import Path
import re
import statistics
import sys
import tempfile


WIDTH, HEIGHT = 97, 65


def texture(x, y):
    return (0.45 + 0.12 * math.sin(0.31 * x + 0.13 * y) +
            0.10 * math.cos(0.17 * x - 0.37 * y) +
            0.09 * math.sin(0.09 * x + 0.11 * y + 0.001 * x * y))


def pixels(dx=0, dy=0, alpha=1, exposure=1, local=False, occluded=False):
    values = array('f')
    for y in range(HEIGHT):
        for x in range(WIDTH):
            horizontal = (dx if y < HEIGHT // 2 else -dx) if local else dx
            value = texture(x - horizontal, y - dy) * exposure * alpha
            if occluded and x > WIDTH * 2 // 3:
                value = 0
            values.extend((value, value, value, alpha))
    return values


def image(bpy, name, values):
    result = bpy.data.images.new(name, WIDTH, HEIGHT, alpha=True, float_buffer=True)
    result.colorspace_settings.name = 'Non-Color'
    result.alpha_mode = 'PREMUL'
    result.pixels.foreach_set(values)
    result.update()
    return result


def source_contracts():
    root = Path(__file__).resolve().parents[2]
    compositor = root / 'source/blender/compositor'
    source = (root / 'source/blender/nodes/composite/nodes/node_composite_optical_flow.cc').read_text(encoding='utf-8')
    info = (compositor / 'shaders/infos/compositor_optical_flow_infos.hh').read_text(encoding='utf-8')
    registered = set(re.findall(r'GPU_SHADER_CREATE_INFO\((\w+)\)', info))
    requested = set(re.findall(r'"(compositor_optical_flow_\w+)"', source))
    assert requested <= registered, requested - registered
    cmake = (compositor / 'CMakeLists.txt').read_text(encoding='utf-8')
    shader_cmake = (compositor / 'shaders/CMakeLists.txt').read_text(encoding='utf-8')
    for filename in re.findall(r'COMPUTE_SOURCE\("([^"]+)"\)', info):
        assert (compositor / 'shaders' / filename).is_file(), filename
        assert filename in cmake and filename in shader_cmake, filename
    assert 'gpu_shader_compositor_optical_flow.glsl' in cmake
    node_cmake = (root / 'source/blender/nodes/composite/CMakeLists.txt').read_text(encoding='utf-8')
    assert 'nodes/node_composite_optical_flow.cc' in node_cmake
    rna = (root / 'source/blender/makesrna/intern/rna_nodetree.cc').read_text(encoding='utf-8')
    assert 'define("CompositorNode", "CompositorNodeOpticalFlow")' in rna
    menu = (root / 'scripts/startup/bl_ui/node_add_menu_compositor.py').read_text(encoding='utf-8')
    assert '"CompositorNodeOpticalFlow"' in menu
    assert 'get_result("Confidence").should_compute() &&' in source
    assert 'ResultPrecision::Full' in source
    print('OPTICAL_FLOW_SOURCE_CONTRACTS_OK')


class Harness:
    def __init__(self, bpy, device, folder):
        self.bpy, self.device, self.folder = bpy, device, Path(folder)
        self.results = {}
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

    def setup(self, values, output='BOTH'):
        bpy = self.bpy
        self.tree = bpy.data.node_groups.new('OpticalFlow_' + self.device, 'CompositorNodeTree')
        self.tree.interface.new_socket('Image', in_out='OUTPUT', socket_type='NodeSocketColor')
        self.scene.compositing_node_group = self.tree
        self.node = self.tree.nodes.new('CompositorNodeOpticalFlow')
        for name, value in values.items():
            if isinstance(value, bpy.types.Image):
                source = self.tree.nodes.new('CompositorNodeImage')
                source.image = value
                self.tree.links.new(source.outputs['Image'], self.node.inputs[name])
            else:
                self.node.inputs[name].default_value = value
        combine = self.tree.nodes.new('CompositorNodeCombineColor')
        if output in ('BOTH', 'FLOW'):
            separate = self.tree.nodes.new('ShaderNodeSeparateXYZ')
            self.tree.links.new(self.node.outputs['Flow'], separate.inputs['Vector'])
            self.tree.links.new(separate.outputs['X'], combine.inputs['Red'])
            self.tree.links.new(separate.outputs['Y'], combine.inputs['Green'])
        if output in ('BOTH', 'CONFIDENCE'):
            self.tree.links.new(self.node.outputs['Confidence'], combine.inputs['Blue'])
        out = self.tree.nodes.new('NodeGroupOutput')
        self.tree.links.new(combine.outputs['Image'], out.inputs['Image'])

    def render(self, tag):
        self.tree.update_tag()
        path = self.folder / (self.device + '_' + tag + '.exr')
        self.scene.render.filepath = str(path)
        self.bpy.ops.render.render(write_still=True)
        result = self.bpy.data.images.load(str(path), check_existing=False)
        result.alpha_mode = 'PREMUL'
        assert tuple(result.size) == (WIDTH, HEIGHT), (tag, tuple(result.size))
        data = array('f', [0]) * (WIDTH * HEIGHT * 4)
        result.pixels.foreach_get(data)
        self.bpy.data.images.remove(result)
        assert all(math.isfinite(value) for value in data), tag + ': non-finite output'
        assert min(data[2::4]) >= -1e-6 and max(data[2::4]) <= 1.000001, tag + ': confidence range'
        self.results[tag] = data
        print(f'OPTICAL_FLOW_CASE_OK {self.device} {tag}', flush=True)
        return data


def interior(margin=16):
    return [4 * (y * WIDTH + x) for y in range(margin, HEIGHT - margin)
            for x in range(margin, WIDTH - margin)]


def check_translation(data, dx, dy, label, indices=None, tolerance=0.3):
    indices = interior() if indices is None else indices
    errors = sorted(math.hypot(data[i] - dx, data[i + 1] - dy) for i in indices)
    median = statistics.median(errors)
    p90 = errors[int(0.9 * (len(errors) - 1))]
    confidence = statistics.mean(data[i + 2] for i in indices)
    print(f'OPTICAL_FLOW_ERROR {label} median={median:.6f} p90={p90:.6f} confidence={confidence:.6f}', flush=True)
    assert median < tolerance and p90 < tolerance * 3, (label, median, p90)
    assert confidence > 0.015, (label, 'no useful confidence', confidence)


def test_device(h):
    source = image(h.bpy, 'flow_source', pixels())
    h.setup({'Image': source, 'Next Image': source})
    same = h.render('identical')
    assert max(abs(value) for c in (0, 1) for value in same[c::4]) < 0.0001
    for values, tag in (({'Image': source}, 'missing_next'),
                        ({'Image': (0.2, 0.2, 0.2, 1), 'Next Image': (0.4, 0.4, 0.4, 1)}, 'single_values'),
                        ({'Image': source, 'Next Image': source, 'Max Motion': 0}, 'zero_range')):
        h.setup(values)
        result = h.render(tag)
        assert max(abs(value) for c in (0, 1, 2) for value in result[c::4]) < 0.00001
    flat = image(h.bpy, 'flow_flat', array('f', (0.2, 0.2, 0.2, 1)) * (WIDTH * HEIGHT))
    h.setup({'Image': flat, 'Next Image': flat})
    result = h.render('flat')
    assert max(abs(value) for c in (0, 1, 2) for value in result[c::4]) < 0.0001

    target = image(h.bpy, 'flow_next', pixels(3, -2))
    settings = {'Image': source, 'Next Image': target, 'Max Motion': 16, 'Consistency Check': False}
    for quality in ('Fast', 'Balanced', 'High'):
        h.setup({**settings, 'Quality': quality})
        result = h.render('translation_' + quality.lower())
        check_translation(result, 3, -2, h.device + ' ' + quality)
    baseline = h.results['translation_balanced']
    for dx, dy in ((4, -2), (3, -2)):
        default_target = image(h.bpy, 'flow_default_range', pixels(dx, dy))
        h.setup({'Image': source, 'Next Image': default_target})
        check_translation(h.render('default_range_' + str(dx)), dx, dy,
                          f'{h.device} default range ({dx}, {dy})')
    for dx, dy in ((-4, 3), (0.75, -1.25), (12, -7)):
        next_image = image(h.bpy, 'flow_translation', pixels(dx, dy))
        h.setup({**settings, 'Next Image': next_image})
        result = h.render('translation_' + str(dx) + '_' + str(dy))
        check_translation(result, dx, dy, f'{h.device} ({dx}, {dy})')

    h.setup({**settings, 'Image': target, 'Next Image': source})
    check_translation(h.render('reverse'), -3, 2, h.device + ' reverse')
    h.setup({**settings, 'Consistency Check': True})
    checked = h.render('consistency')
    check_translation(checked, 3, -2, h.device + ' consistency')
    assert all(checked[i + 2] <= baseline[i + 2] + 0.0001 for i in range(0, len(checked), 4))

    mask_values = array('f')
    for y in range(HEIGHT):
        for x in range(WIDTH):
            value = 0 if x < WIDTH // 2 else 0.5
            mask_values.extend((value, value, value, 1))
    mask = image(h.bpy, 'flow_mask', mask_values)
    h.setup({**settings, 'Mask': mask})
    masked = h.render('mask')
    for y in range(HEIGHT):
        for x in range(WIDTH):
            index = 4 * (y * WIDTH + x)
            if x < WIDTH // 2:
                assert all(abs(masked[index + c]) < 0.0001 for c in (0, 1, 2))
            else:
                assert abs(masked[index] - baseline[index]) < 0.0001
                assert abs(masked[index + 1] - baseline[index + 1]) < 0.0001
                assert abs(masked[index + 2] - baseline[index + 2] * 0.5) < 0.0001
    h.setup({**settings, 'Max Motion': 1})
    limited = h.render('limited')
    assert max(math.hypot(limited[i], limited[i + 1]) for i in range(0, len(limited), 4)) <= 1.0001

    for output, tag in (('FLOW', 'flow_only'), ('CONFIDENCE', 'confidence_only')):
        h.setup(settings, output=output)
        isolated = h.render(tag)
        channels = (0, 1) if output == 'FLOW' else (2,)
        for c in channels:
            assert max(abs(a - b) for a, b in zip(isolated[c::4], baseline[c::4])) < 0.0001

    alpha_source = image(h.bpy, 'flow_alpha_source', pixels(alpha=0.4, exposure=8))
    alpha_target = image(h.bpy, 'flow_alpha_next', pixels(3, -2, alpha=0.4, exposure=8))
    h.setup({**settings, 'Image': alpha_source, 'Next Image': alpha_target})
    check_translation(h.render('alpha_hdr'), 3, -2, h.device + ' alpha/HDR', tolerance=0.35)
    transparent = image(h.bpy, 'flow_transparent', array('f', [0]) * (WIDTH * HEIGHT * 4))
    h.setup({'Image': transparent, 'Next Image': source})
    empty = h.render('transparent')
    assert max(abs(value) for c in (0, 1, 2) for value in empty[c::4]) < 0.0001

    local = image(h.bpy, 'flow_local', pixels(dx=3, local=True))
    h.setup({**settings, 'Next Image': local})
    result = h.render('local_motion')
    lower = [4 * (y * WIDTH + x) for y in range(12, HEIGHT // 2 - 8) for x in range(16, WIDTH - 16)]
    upper = [4 * (y * WIDTH + x) for y in range(HEIGHT // 2 + 8, HEIGHT - 12) for x in range(16, WIDTH - 16)]
    check_translation(result, 3, 0, h.device + ' lower local motion', lower, tolerance=0.5)
    check_translation(result, -3, 0, h.device + ' upper local motion', upper, tolerance=0.5)
    occluded = image(h.bpy, 'flow_occluded', pixels(3, -2, occluded=True))
    h.setup({**settings, 'Next Image': occluded, 'Consistency Check': True})
    result = h.render('occluded')
    valid = [result[4 * (y * WIDTH + x) + 2] for y in range(16, HEIGHT - 16) for x in range(16, 45)]
    hidden = [result[4 * (y * WIDTH + x) + 2] for y in range(16, HEIGHT - 16) for x in range(82, WIDTH - 3)]
    assert statistics.mean(hidden) < statistics.mean(valid), 'occluded region retained excessive confidence'
    return h.results


def persistence(bpy, folder):
    tree = bpy.data.node_groups.new('OpticalFlowPersistence', 'CompositorNodeTree')
    tree.use_fake_user = True
    node = tree.nodes.new('CompositorNodeOpticalFlow')
    node.inputs['Quality'].default_value = 'High'
    node.inputs['Max Motion'].default_value = 123
    node.inputs['Consistency Check'].default_value = False
    copied = tree.copy()
    copied.nodes[0].inputs['Max Motion'].default_value = 17
    assert node.inputs['Max Motion'].default_value == 123
    bpy.data.node_groups.remove(copied)
    path = str(Path(folder) / 'optical_flow_persistence.blend')
    bpy.ops.wm.save_as_mainfile(filepath=path)
    bpy.ops.wm.open_mainfile(filepath=path, load_ui=False)
    node = bpy.data.node_groups['OpticalFlowPersistence'].nodes[0]
    assert node.inputs['Quality'].default_value == 'High'
    assert node.inputs['Max Motion'].default_value == 123
    assert not node.inputs['Consistency Check'].default_value
    print('OPTICAL_FLOW_PERSISTENCE_OK')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--device', choices=('CPU', 'GPU', 'BOTH'), default='CPU')
    parser.add_argument('--source-only', action='store_true')
    parser.add_argument('--output-dir')
    arguments = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else (
        sys.argv[1:] if '--source-only' in sys.argv else [])
    args = parser.parse_args(arguments)
    if args.source_only:
        source_contracts()
        return
    import bpy
    assert hasattr(bpy.types, 'CompositorNodeOpticalFlow'), 'Optical Flow is not registered'
    with tempfile.TemporaryDirectory(prefix='blender_optical_flow_') as temporary:
        folder = Path(args.output_dir) if args.output_dir else Path(temporary)
        folder.mkdir(parents=True, exist_ok=True)
        devices = ('CPU', 'GPU') if args.device == 'BOTH' else (args.device,)
        results = {device: test_device(Harness(bpy, device, folder)) for device in devices}
        if args.device == 'BOTH':
            for tag, cpu in results['CPU'].items():
                gpu = results['GPU'][tag]
                reliable = [i for i in interior() if min(cpu[i + 2], gpu[i + 2]) > 0.02]
                if reliable:
                    errors = sorted(math.hypot(cpu[i] - gpu[i], cpu[i + 1] - gpu[i + 1]) for i in reliable)
                    assert statistics.median(errors) < 0.05 and errors[int(0.9 * (len(errors) - 1))] < 0.3, (
                        tag, 'CPU/GPU reliable-vector mismatch', statistics.median(errors), errors[-1])
                confidence_error = statistics.mean(abs(cpu[i + 2] - gpu[i + 2]) for i in interior())
                assert confidence_error < 0.05, (tag, 'CPU/GPU confidence mismatch', confidence_error)
            print(f'OPTICAL_FLOW_CPU_GPU_PARITY_OK {len(results["CPU"])} cases')
        persistence(bpy, folder)
    print('COMPOSITOR_OPTICAL_FLOW_TEST_OK')


if __name__ == '__main__':
    main()
