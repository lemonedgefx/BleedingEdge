# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Motion-workflow render, sequence, cache-invalidation and persistence regressions.

blender -b --factory-startup --python-exit-code 1 --python compositor_flow_workflow_test.py -- --device BOTH
python compositor_flow_workflow_test.py --source-only
"""

import argparse
from array import array
import ast
import math
from pathlib import Path
import re
import runpy
import statistics
import sys
import tempfile

_fixtures = runpy.run_path(str(Path(__file__).with_name('compositor_optical_flow_test.py')))
WIDTH, HEIGHT = _fixtures['WIDTH'], _fixtures['HEIGHT']
image, pixels, interior = (_fixtures[name] for name in ('image', 'pixels', 'interior'))


def source_contracts():
    root = Path(__file__).resolve().parents[2]
    compositor = root / 'source/blender/compositor'
    source = (root / 'source/blender/nodes/composite/nodes/node_composite_flow_tools.cc').read_text()
    infos = (compositor / 'shaders/infos/compositor_flow_tools_infos.hh').read_text()
    requested = set(re.findall(r'"(compositor_flow_\w+)"', source))
    registered = set(re.findall(r'GPU_SHADER_CREATE_INFO\((\w+)\)', infos))
    assert requested <= registered, requested - registered
    cmake = (compositor / 'CMakeLists.txt').read_text()
    shader_cmake = (compositor / 'shaders/CMakeLists.txt').read_text()
    for filename in re.findall(r'COMPUTE_SOURCE\("([^"]+)"\)', infos):
        assert filename in cmake and filename in shader_cmake
        assert (compositor / 'shaders' / filename).is_file()
    rna = (root / 'source/blender/makesrna/intern/rna_nodetree.cc').read_text()
    menu = (root / 'scripts/startup/bl_ui/node_add_menu_compositor.py').read_text()
    for name in ('FlowToSpeed', 'FlowVisualize', 'FlowInterpolate'):
        assert f'define("CompositorNode", "CompositorNode{name}")' in rna
        assert f'"CompositorNode{name}"' in menu
    node_cmake = (root / 'source/blender/nodes/composite/CMakeLists.txt').read_text()
    assert 'nodes/node_composite_flow_tools.cc' in node_cmake
    workflow = root / 'scripts/startup/bl_operators/node_optical_flow.py'
    ast.parse(workflow.read_text())
    image_source = (root / 'source/blender/nodes/composite/nodes/node_composite_image.cc').read_text()
    assert 'image_user.sfra += frame - target' in image_source
    assert '"Previous Frame"_ustr' in image_source and '"Next Frame"_ustr' in image_source
    assert 'CachedImageContainer' in image_source
    print('FLOW_WORKFLOW_SOURCE_CONTRACTS_OK')


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

    def tree_output(self):
        self.tree = self.bpy.data.node_groups.new('FlowWorkflow_' + self.device, 'CompositorNodeTree')
        self.tree.interface.new_socket('Image', in_out='OUTPUT', socket_type='NodeSocketColor')
        self.scene.compositing_node_group = self.tree
        self.output = self.tree.nodes.new('NodeGroupOutput')

    def setup(self, node_type, values, output='Image'):
        self.tree_output()
        self.node = self.tree.nodes.new(node_type)
        for name, value in values.items():
            if isinstance(value, self.bpy.types.Image):
                source = self.tree.nodes.new('CompositorNodeImage')
                source.image = value
                self.tree.links.new(source.outputs['Image'], self.node.inputs[name])
            else:
                self.node.inputs[name].default_value = value
        if output in {'Speed', 'Speed W', 'Speed Both'}:
            # Speed is data, not premultiplied color: never export its W as image alpha.
            separate = self.tree.nodes.new('CompositorNodeSeparateColor')
            self.tree.links.new(self.node.outputs['Speed'], separate.inputs['Image'])
            if output == 'Speed W':
                self.tree.links.new(separate.outputs['Alpha'], self.output.inputs['Image'])
            else:
                combine = self.tree.nodes.new('CompositorNodeCombineColor')
                for channel in ('Red', 'Green', 'Blue'):
                    self.tree.links.new(separate.outputs[channel], combine.inputs[channel])
                if output == 'Speed Both':
                    self.tree.links.new(self.node.outputs['Confidence'], combine.inputs['Blue'])
                self.tree.links.new(combine.outputs['Image'], self.output.inputs['Image'])
        elif output == 'Interpolate Both':
            separate = self.tree.nodes.new('CompositorNodeSeparateColor')
            combine = self.tree.nodes.new('CompositorNodeCombineColor')
            self.tree.links.new(self.node.outputs['Image'], separate.inputs['Image'])
            for channel in ('Red', 'Green', 'Blue'):
                self.tree.links.new(separate.outputs[channel], combine.inputs[channel])
            self.tree.links.new(self.node.outputs['Confidence'], combine.inputs['Alpha'])
            self.tree.links.new(combine.outputs['Image'], self.output.inputs['Image'])
        else:
            self.tree.links.new(self.node.outputs[output], self.output.inputs['Image'])

    def render(self, tag):
        self.tree.update_tag()
        path = self.folder / (self.device + '_' + tag + '.exr')
        self.scene.render.filepath = str(path)
        self.bpy.ops.render.render(write_still=True)
        output = self.bpy.data.images.load(str(path), check_existing=False)
        output.alpha_mode = 'PREMUL'
        assert tuple(output.size) == (WIDTH, HEIGHT), (tag, tuple(output.size))
        data = array('f', [0]) * (WIDTH * HEIGHT * 4)
        output.pixels.foreach_get(data)
        self.bpy.data.images.remove(output)
        assert all(math.isfinite(value) for value in data), tag
        self.results[tag] = data
        print('FLOW_WORKFLOW_CASE_OK', self.device, tag, flush=True)
        return data


def compare(a, b, tag, indices=None, tolerance=2e-5):
    indices = range(len(a)) if indices is None else indices
    error = max(abs(a[i] - b[i]) for i in indices)
    assert error < tolerance, (tag, error, tolerance)


def uniform(data, expected, tag):
    for channel, value in enumerate(expected):
        assert max(abs(x - value) for x in data[channel::4]) < 2e-5, (tag, channel, value)


def compare_workflow(cpu, gpu, tag):
    # The estimator and existing Vector Blur are approximate. Check full-frame error
    # distributions, not bit identity at a few ambiguous/thresholded boundary pixels.
    # Analytical consumer fixtures still use strict maximum-error checks below.
    errors = sorted(abs(a - b) for a, b in zip(cpu, gpu))
    mean = statistics.mean(errors)
    p99 = errors[int(0.99 * (len(errors) - 1))]
    print('FLOW_WORKFLOW_PARITY_ERROR', tag, 'mean', mean, 'p99', p99, 'max', errors[-1], flush=True)
    assert mean < 0.003 and p99 < 0.03, (tag, mean, p99)
    if tag.startswith('workflow_'):
        compare(cpu, gpu, tag + ' known-translation interior',
                [i + c for i in interior() for c in range(4)], tolerance=0.003)


def test_rejected_motion(h):
    """Rejected flow is not a clean plate: its colour must stay local to the blur path."""
    bpy = h.bpy
    original = pixels()
    rejected = ((WIDTH // 3, HEIGHT // 3), (2 * WIDTH // 3, 2 * HEIGHT // 3))
    confidence = array('f', (1, 1, 1, 1)) * (WIDTH * HEIGHT)
    altered = array('f', original)
    for x, y in rejected:
        start = 4 * (y * WIDTH + x)
        confidence[start:start + 3] = array('f', (0, 0, 0))
        altered[start:start + 3] = array('f', (1, 0, 1))
    certainty = image(bpy, 'sparse_rejected_motion_confidence', confidence)
    source = image(bpy, 'sparse_rejected_motion_original', original)
    replacement = image(bpy, 'sparse_rejected_motion_changed', altered)
    # Realizing a smaller Speed result on the image domain must not turn it into
    # render-pass motion. Crop evenly so the confidence holes stay aligned.
    margin = 4
    smaller_certainty = bpy.data.images.new(
        'sparse_rejected_motion_smaller_confidence', WIDTH - 2 * margin, HEIGHT - 2 * margin,
        alpha=True, float_buffer=True,
    )
    smaller_certainty.colorspace_settings.name = 'Non-Color'
    smaller_certainty.alpha_mode = 'PREMUL'
    cropped = array('f')
    for y in range(margin, HEIGHT - margin):
        cropped.extend(confidence[4 * (y * WIDTH + margin):4 * ((y + 1) * WIDTH - margin)])
    smaller_certainty.pixels.foreach_set(cropped)
    smaller_certainty.update()
    # The exposure path is only a few pixels long. A changed pixel must not affect
    # the other side of the frame, even when its motion estimate is rejected.
    far_pixels = [i + c for i in interior()
                  if all(max(abs((i // 4) % WIDTH - x), abs((i // 4) // WIDTH - y)) > 12
                         for x, y in rejected)
                  for c in range(4)]

    for mode, curved, confidence_image in (
        ('linear', False, certainty),
        ('curved', True, certainty),
        ('realized', False, smaller_certainty),
    ):
        previous, following = (-8, 3), ((5, 6) if curved else (8, -3))
        h.setup('CompositorNodeVecBlur', {
            'Image': source, 'Speed': (*previous, -following[0], -following[1]),
            'Shutter': 0.5, 'Curved': curved,
        })
        reference = h.render('rejected_motion_' + mode + '_exact')
        h.setup('CompositorNodeFlowToSpeed', {
            'Previous Flow': previous, 'Next Flow': following,
            'Previous Confidence': confidence_image, 'Next Confidence': confidence_image,
        }, 'Speed')
        adapter = h.node
        blur = h.tree.nodes.new('CompositorNodeVecBlur')
        blur.inputs['Shutter'].default_value = 0.5
        blur.inputs['Curved'].default_value = curved
        upstream = h.tree.nodes.new('CompositorNodeImage')
        upstream.image = source
        h.tree.links.new(upstream.outputs['Image'], blur.inputs['Image'])
        h.tree.links.new(adapter.outputs['Speed'], blur.inputs['Speed'])
        h.tree.links.new(blur.outputs['Image'], h.output.inputs['Image'])
        sparse = h.render('rejected_motion_' + mode + '_sparse')
        upstream.image = replacement
        changed = h.render('rejected_motion_' + mode + '_changed')
        compare(sparse, changed, mode + ' rejected pixel colour travelled beyond the shutter path',
                far_pixels, tolerance=0.0005)
        errors = sorted(abs(sparse[i] - reference[i]) for i in far_pixels)
        assert errors[int(0.95 * (len(errors) - 1))] < 0.002, (mode, 'rejected motion changed distant blur')
        assert max(abs(sparse[i] - original[i]) for i in far_pixels) > 0.002, (mode, 'no motion blur')
        assert min(sparse[3::4]) > 0.999, (mode, 'rejected motion made opaque footage transparent')
    print('FLOW_REJECTED_MOTION_LOCALITY_OK', h.device, flush=True)


def test_device(h):
    bpy = h.bpy
    source = image(bpy, 'flow_workflow_source', pixels())
    target = image(bpy, 'flow_workflow_target', pixels(4, -2))
    mid = pixels(2, -1)
    rgb_interior = [i + c for i in interior() for c in range(4)]
    settings = {'Image': source, 'Next Image': target, 'Forward Flow': (4, -2), 'Backward Flow': (-4, 2)}
    h.setup('CompositorNodeFlowInterpolate', settings)
    compare(h.render('interpolate_translation'), mid, 'translation midpoint', rgb_interior)
    h.setup('CompositorNodeFlowInterpolate', settings, 'Interpolate Both')
    compare(h.render('interpolate_both_outputs'), mid, 'both-output midpoint', rgb_interior)
    for factor, expected in ((0, pixels()), (1, pixels(4, -2))):
        h.setup('CompositorNodeFlowInterpolate', {**settings, 'Factor': factor, 'Forward Confidence': 0})
        compare(h.render('endpoint_' + str(factor)), expected, 'exact endpoint')
    h.setup('CompositorNodeFlowInterpolate', {**settings, 'Factor': 0.25})
    quarter = h.render('quarter_frame')
    quarter_expected = pixels(1, -0.5)
    assert statistics.mean(abs(quarter[i] - quarter_expected[i]) for i in interior()) < 0.003
    h.setup('CompositorNodeFlowInterpolate', {**settings, 'Forward Confidence': 0, 'Backward Confidence': 0})
    baseline = array('f', ((a + b) * 0.5 for a, b in zip(pixels(), pixels(4, -2))))
    compare(h.render('fallback_crossfade'), baseline, 'crossfade')
    h.node.inputs['Fallback'].default_value = 'Nearest Frame'
    h.node.inputs['Factor'].default_value = 0.25
    compare(h.render('fallback_nearest'), pixels(), 'nearest')
    h.node.inputs['Fallback'].default_value = 'Transparent'
    uniform(h.render('fallback_transparent'), (0, 0, 0, 0), 'transparent fallback')
    h.setup('CompositorNodeFlowInterpolate', {**settings, 'Forward Confidence': 0, 'Backward Confidence': 0}, 'Confidence')
    uniform(h.render('zero_confidence'), (0, 0, 0, 1), 'confidence zero')
    h.setup('CompositorNodeFlowInterpolate', settings, 'Confidence')
    confidence = h.render('interpolate_confidence_only')
    assert statistics.mean(confidence[i] for i in interior()) > 0.99

    # A spatially varying affine field: point + t*flow(point) must be inverted, not negated.
    affine_a, affine_b, forward, backward = (array('f') for _ in range(4))
    scale = 1.12
    center = WIDTH / 2
    for y in range(HEIGHT):
        for x in range(WIDTH):
            a = 0.2 + 0.004 * x + 0.002 * y
            bx = center + (x - center) / scale
            b = 0.2 + 0.004 * bx + 0.002 * y
            affine_a.extend((a, a, a, 1))
            affine_b.extend((b, b, b, 1))
            forward.extend(((scale - 1) * (x - center), 0, 0, 1))
            backward.extend(((1 / scale - 1) * (x - center), 0, 0, 1))
    h.setup('CompositorNodeFlowInterpolate', {
        'Image': image(bpy, 'affine_a', affine_a), 'Next Image': image(bpy, 'affine_b', affine_b),
        'Forward Flow': image(bpy, 'affine_forward', forward),
        'Backward Flow': image(bpy, 'affine_backward', backward),
    })
    affine = h.render('affine_inverse_warp')
    for i in interior():
        x, y = (i // 4) % WIDTH, (i // 4) // WIDTH
        expected = 0.2 + 0.004 * (center + (x - center) / (1 + (scale - 1) * 0.5)) + 0.002 * y
        assert abs(affine[i] - expected) < 0.0003, ('affine inverse', x, affine[i], expected)

    # Premultiplied alpha and unclipped HDR must survive motion sampling.
    h.setup('CompositorNodeFlowInterpolate', {
        **settings, 'Image': image(bpy, 'hdr_a', pixels(alpha=0.4, exposure=8)),
        'Next Image': image(bpy, 'hdr_b', pixels(4, -2, alpha=0.4, exposure=8)),
    })
    compare(h.render('alpha_hdr'), pixels(2, -1, alpha=0.4, exposure=8), 'alpha HDR', rgb_interior)
    h.setup('CompositorNodeFlowInterpolate', {'Image': (0.2, 0.1, 0.4, 0.5), 'Next Image': (0.4, 0.3, 0.2, 0.5)})
    uniform(h.render('constant_images'), (0.3, 0.2, 0.3, 0.5), 'constant interpolation')
    h.setup('CompositorNodeFlowInterpolate', {'Image': (0, 0, 0, 0), 'Next Image': (0, 0, 0, 0)}, 'Confidence')
    uniform(h.render('transparent_support'), (0, 0, 0, 1), 'transparent regions have no support')
    h.setup('CompositorNodeFlowInterpolate', {
        **settings,
        'Forward Flow': image(bpy, 'invalid_forward', array('f', [math.nan, 0, 0, 1]) * (WIDTH * HEIGHT)),
        'Backward Flow': image(bpy, 'invalid_backward', array('f', [math.inf, 0, 0, 1]) * (WIDTH * HEIGHT)),
    })
    compare(h.render('invalid_flow_fallback'), baseline, 'invalid-flow fallback')

    speed_settings = {'Previous Flow': (-4, 3), 'Next Flow': (5, -2)}
    h.setup('CompositorNodeFlowToSpeed', speed_settings, 'Speed')
    uniform(h.render('speed_packing'), (-4, 3, -5, 1), 'past/future XYZ signs')
    h.setup('CompositorNodeFlowToSpeed', speed_settings, 'Speed W')
    uniform(h.render('speed_packing_w'), (2, 2, 2, 1), 'future W sign')
    h.setup('CompositorNodeFlowToSpeed', {**speed_settings, 'Previous Confidence': 0.5, 'Next Confidence': 0.8}, 'Speed')
    uniform(h.render('speed_soft_confidence'), (-4, 3, -5, 1), 'confidence must not shorten vectors')
    h.setup('CompositorNodeFlowToSpeed', {**speed_settings, 'Previous Confidence': 0}, 'Speed')
    uniform(h.render('speed_rejected_previous'), (0, 0, -5, 1), 'per-half rejection')
    h.setup('CompositorNodeFlowToSpeed', {**speed_settings, 'Next Confidence': 0.2, 'Confidence Threshold': 0.5}, 'Speed')
    uniform(h.render('speed_threshold'), (-4, 3, 0, 1), 'threshold')
    h.setup('CompositorNodeFlowToSpeed', {**speed_settings, 'Next Confidence': 0}, 'Speed W')
    uniform(h.render('speed_rejected_w'), (0, 0, 0, 1), 'rejected future W')
    h.setup('CompositorNodeFlowToSpeed', {**speed_settings, 'Previous Confidence': 0.4, 'Next Confidence': 0.8}, 'Confidence')
    uniform(h.render('speed_confidence_only'), (0.4, 0.4, 0.4, 1), 'combined confidence')
    h.setup('CompositorNodeFlowToSpeed', {
        **speed_settings,
        'Previous Flow': image(bpy, 'spatial_previous_flow', array('f', [-4, 3, 0, 1]) * (WIDTH * HEIGHT)),
        'Next Flow': image(bpy, 'spatial_next_flow', array('f', [5, -2, 0, 1]) * (WIDTH * HEIGHT)),
        'Previous Confidence': 0.4, 'Next Confidence': 0.8,
    }, 'Speed Both')
    uniform(h.render('speed_spatial_both_outputs'), (-4, 3, 0.4, 1), 'spatial adapter both outputs')
    for flow, expected, tag in (((0, 0), (0, 0, 0, 1), 'zero'), ((64, 0), (1, 0, 0, 1), 'right'),
                               ((-64, 0), (0, 1, 1, 1), 'left')):
        h.setup('CompositorNodeFlowVisualize', {'Flow': flow})
        uniform(h.render('preview_' + tag), expected, tag)
    h.setup('CompositorNodeFlowVisualize', {'Flow': (64, 0), 'Confidence': 0.5})
    uniform(h.render('preview_confidence'), (0.5, 0, 0, 1), 'preview weighting')
    h.setup('CompositorNodeFlowVisualize', {
        'Flow': image(bpy, 'spatial_preview_flow', array('f', [64, 0, 0, 1]) * (WIDTH * HEIGHT)),
        'Confidence': 0.5,
    })
    uniform(h.render('preview_spatial'), (0.5, 0, 0, 1), 'spatial preview weighting')

    # The adapter must preserve the existing renderer for exactly supplied vectors.
    h.setup('CompositorNodeVecBlur', {'Image': source, 'Speed': (-4, 2, -4, 2), 'Shutter': 0.5})
    legacy_blur = h.render('legacy_known_motion_blur')
    h.setup('CompositorNodeFlowToSpeed', {'Previous Flow': (-4, 2), 'Next Flow': (4, -2)}, 'Speed')
    adapter = h.node
    blur = h.tree.nodes.new('CompositorNodeVecBlur')
    upstream = h.tree.nodes.new('CompositorNodeImage')
    upstream.image = source
    h.tree.links.new(upstream.outputs['Image'], blur.inputs['Image'])
    h.tree.links.new(adapter.outputs['Speed'], blur.inputs['Speed'])
    h.tree.links.new(blur.outputs['Image'], h.output.inputs['Image'])
    compare(h.render('adapter_known_motion_blur'), legacy_blur, 'adapter preserves legacy blur')

    from bl_operators.node_optical_flow import build_optical_flow_group
    for mode in ('MOTION_BLUR', 'INTERPOLATE'):
        group = build_optical_flow_group(mode)
        h.tree_output()
        node = h.tree.nodes.new('CompositorNodeGroup')
        node.node_tree = group
        for name, value in {'Image': source, 'Next Image': target, **({'Previous Image': image(bpy, 'previous', pixels(-4, 2))} if mode == 'MOTION_BLUR' else {})}.items():
            upstream = h.tree.nodes.new('CompositorNodeImage')
            upstream.image = value
            h.tree.links.new(upstream.outputs['Image'], node.inputs[name])
        h.tree.links.new(node.outputs['Image'], h.output.inputs['Image'])
        rendered = h.render('workflow_' + mode)
        if mode == 'INTERPOLATE':
            assert statistics.mean(abs(rendered[i] - mid[i]) for i in interior()) < 0.02
            assert max(abs(alpha - 1) for alpha in rendered[3::4]) < 0.0001, 'Interpolation made opaque borders transparent'
        else:
            unblurred = pixels()
            assert max(abs(rendered[i] - unblurred[i]) for i in interior()) > 0.0001
            assert max(abs(rendered[i + 3] - 1) for i in interior()) < 0.001
        assert node.inputs['Quality'].default_value == 'Balanced'
        # The groups must remain ordinary editable graphs in a stable left-to-right layout.
        assert all(n.location.x >= 0 for n in group.nodes if n.bl_idname not in {'NodeGroupInput'})
        for output in ('Confidence', 'Motion Preview'):
            h.tree.links.new(node.outputs[output], h.output.inputs['Image'])
            h.render('workflow_' + mode + '_' + output.replace(' ', '_'))
        node.inputs['Mask'].default_value = 0
        h.tree.links.new(node.outputs['Image'], h.output.inputs['Image'])
        compare(h.render('workflow_' + mode + '_masked'), pixels() if mode == 'MOTION_BLUR' else baseline,
                'masked workflow fallback', tolerance=0.0002)
    return h.results


def sequence_and_persistence(h):
    bpy = h.bpy
    folder = h.folder / ('sequence_' + h.device)
    folder.mkdir(exist_ok=True)
    for frame in range(1, 4):
        frame_image = image(bpy, 'sequence_frame', array('f', [frame / 10, 0, 0, 1]) * (WIDTH * HEIGHT))
        frame_image.filepath_raw = str(folder / f'frame_{frame:04d}.exr')
        frame_image.file_format = 'OPEN_EXR'
        frame_image.save()
        bpy.data.images.remove(frame_image)
    sequence = bpy.data.images.load(str(folder / 'frame_0001.exr'), check_existing=False)
    sequence.source = 'SEQUENCE'
    h.tree_output()
    source = h.tree.nodes.new('CompositorNodeImage')
    source.image = sequence
    source.frame_duration = 3
    source.frame_start = 1
    source.frame_offset = 0
    source.use_cyclic = False
    original = (source.frame_start, source.frame_offset)

    def check(frame, socket, value, tag):
        h.scene.frame_set(frame)
        h.tree.links.new(source.outputs[socket], h.output.inputs['Image'])
        data = h.render(tag)
        assert abs(statistics.mean(data[0::4]) - value) < 0.0002, (tag, statistics.mean(data[0::4]), value)
        assert (source.frame_start, source.frame_offset) == original, 'Adjacent outputs mutated ImageUser'

    for socket, value in (('Previous Frame', 0.1), ('Image', 0.2), ('Next Frame', 0.3)):
        check(2, socket, value, 'sequence_' + socket.replace(' ', '_'))
    check(1, 'Previous Frame', 0.1, 'sequence_hold_start')
    check(3, 'Next Frame', 0.3, 'sequence_hold_end')
    source.use_cyclic = True
    check(1, 'Previous Frame', 0.3, 'sequence_wrap_start')
    check(3, 'Next Frame', 0.1, 'sequence_wrap_end')
    source.use_cyclic = False
    # Reload the same ID with changed bytes; retained image results must invalidate.
    replacement = image(bpy, 'replacement_frame', array('f', [0.7, 0, 0, 1]) * (WIDTH * HEIGHT))
    replacement.filepath_raw = str(folder / 'frame_0002.exr')
    replacement.file_format = 'OPEN_EXR'
    replacement.save()
    bpy.data.images.remove(replacement)
    sequence.reload()
    check(2, 'Image', 0.7, 'sequence_reload_invalidation')

    from bl_operators.node_optical_flow import build_optical_flow_group
    for mode in ('MOTION_BLUR', 'INTERPOLATE'):
        group = build_optical_flow_group(mode)
        group.use_fake_user = True
        copied = group.copy()
        flow = next(n for n in copied.nodes if n.bl_idname == 'CompositorNodeOpticalFlow')
        flow.inputs['Consistency Check'].default_value = not flow.inputs['Consistency Check'].default_value
        original_flow = next(n for n in group.nodes if n.bl_idname == 'CompositorNodeOpticalFlow')
        assert flow.inputs['Consistency Check'].default_value != original_flow.inputs['Consistency Check'].default_value
        bpy.data.node_groups.remove(copied)
    source.frame_start = 7
    source.frame_offset = 3
    source.use_cyclic = True
    path = str(h.folder / ('flow_workflow_' + h.device + '.blend'))
    tree_name, source_name = h.tree.name, source.name
    bpy.ops.wm.save_as_mainfile(filepath=path)
    bpy.ops.wm.open_mainfile(filepath=path, load_ui=False)
    restored = bpy.data.node_groups[tree_name].nodes[source_name]
    assert restored.frame_start == 7 and restored.frame_offset == 3 and restored.use_cyclic
    assert 'Previous Frame' in restored.outputs and 'Next Frame' in restored.outputs
    assert bpy.ops.node.add_optical_flow_workflow.get_rna_type().identifier == 'NODE_OT_add_optical_flow_workflow'
    print('FLOW_WORKFLOW_SEQUENCE_PERSISTENCE_OK', h.device, flush=True)


def test_footage(h, path):
    """Real movie smoke, not a ground-truth accuracy or production-performance claim."""
    bpy = h.bpy
    movie = bpy.data.images.load(str(path), check_existing=False)
    assert movie.source == 'MOVIE', (path, movie.source)
    h.tree_output()
    source = h.tree.nodes.new('CompositorNodeImage')
    source.image = movie
    source.frame_start = 1
    source.frame_duration = movie.frame_duration
    scale = h.tree.nodes.new('CompositorNodeScale')
    scale.inputs['Type'].default_value = 'Absolute'
    scale.inputs['X'].default_value = WIDTH
    scale.inputs['Y'].default_value = HEIGHT
    h.tree.links.new(source.outputs['Image'], scale.inputs['Image'])
    h.tree.links.new(scale.outputs['Image'], h.output.inputs['Image'])
    originals = {}
    middle = min(100, max(2, movie.frame_duration // 2))
    for frame in (middle - 1, middle, middle + 1):
        h.scene.frame_set(frame)
        originals[frame] = h.render('movie_frame_' + str(frame))
    assert statistics.mean(originals[middle][0::4]) > 0.005, 'Movie fixture is empty'
    assert max(abs(a - b) for a, b in zip(originals[middle], originals[middle + 1])) > 0.0001, 'Movie fixture has no frame change'
    h.scene.frame_set(middle)
    for socket, frame in (('Previous Frame', middle - 1), ('Next Frame', middle + 1)):
        h.tree.links.new(source.outputs[socket], scale.inputs['Image'])
        compare(h.render('movie_' + socket.replace(' ', '_')), originals[frame], 'movie adjacent-frame mapping')
    frames = {frame: image(bpy, 'movie_sample_' + str(frame), data) for frame, data in originals.items()}
    from bl_operators.node_optical_flow import build_optical_flow_group
    for mode in ('MOTION_BLUR', 'INTERPOLATE'):
        h.tree_output()
        workflow = h.tree.nodes.new('CompositorNodeGroup')
        workflow.node_tree = build_optical_flow_group(mode)
        for name, frame in (('Image', middle), ('Next Image', middle + 1),
                            *(((('Previous Image', middle - 1),)) if mode == 'MOTION_BLUR' else ())):
            upstream = h.tree.nodes.new('CompositorNodeImage')
            upstream.image = frames[frame]
            h.tree.links.new(upstream.outputs['Image'], workflow.inputs[name])
        for output in ('Image', 'Confidence', 'Motion Preview'):
            h.tree.links.new(workflow.outputs[output], h.output.inputs['Image'])
            data = h.render('movie_' + mode + '_' + output.replace(' ', '_'))
            if output == 'Image' and mode == 'INTERPOLATE' and all(
                    min(frame[3::4]) > 0.9999 for frame in originals.values()):
                assert max(abs(alpha - 1) for alpha in data[3::4]) < 0.0001, 'Opaque movie interpolation gained holes'
            if output == 'Confidence':
                assert min(data[0::4]) >= 0 and max(data[0::4]) <= 1
                print('FLOW_FOOTAGE_CONFIDENCE', h.device, mode,
                      statistics.mean(data[i] for i in interior()), flush=True)
    print('FLOW_REAL_FOOTAGE_SMOKE_OK', h.device, str(path), flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--device', choices=('CPU', 'GPU', 'BOTH'), default='CPU')
    parser.add_argument('--source-only', action='store_true')
    parser.add_argument('--rejected-motion-only', action='store_true')
    parser.add_argument('--output-dir')
    parser.add_argument('--footage', help='Optional real movie for a downsampled workflow smoke')
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else sys.argv[1:]
    args = parser.parse_args(argv)
    if args.source_only:
        source_contracts()
        return
    import bpy
    with tempfile.TemporaryDirectory(prefix='blender_flow_workflow_') as temporary:
        folder = Path(args.output_dir or temporary)
        folder.mkdir(exist_ok=True, parents=True)
        devices = ('CPU', 'GPU') if args.device == 'BOTH' else (args.device,)
        results = {}
        for device in devices:
            h = Harness(bpy, device, folder)
            test_rejected_motion(h)
            if not args.rejected_motion_only:
                test_device(h)
                if args.footage:
                    test_footage(h, Path(args.footage))
                sequence_and_persistence(h)
            results[device] = h.results
        if args.device == 'BOTH':
            for tag, cpu in results['CPU'].items():
                gpu = results['GPU'][tag]
                estimated = tag.startswith(('workflow_', 'movie_MOTION_BLUR_', 'movie_INTERPOLATE_'))
                renderer = tag.startswith('rejected_motion_') or tag in {
                    'legacy_known_motion_blur', 'adapter_known_motion_blur'}
                if estimated or renderer:
                    compare_workflow(cpu, gpu, tag)
                else:
                    # Movie scaling uses the existing CPU/GPU image samplers, not flow math.
                    compare(cpu, gpu, tag, tolerance=0.002 if tag.startswith('movie_') else 0.0005)
            print('FLOW_WORKFLOW_CPU_GPU_PARITY_OK', len(results['CPU']), 'cases')
    print('COMPOSITOR_FLOW_WORKFLOW_TEST_OK')


if __name__ == '__main__':
    main()
