# Optical Flow compositor node

`CompositorNodeOpticalFlow`, under **Add > Tracking > Optical Flow**. This is a native
compositor node compiled into the native compositor.

## Contract

Connect two explicit spatial images. `Image` is the source frame; `Next Image` is the destination
frame. The node does not reevaluate the scene at another time or fetch frames itself. Use aligned
inputs of the same resolution and color space. The source defines the output domain; ordinary
compositor realization handles other inputs, without automatically stretching a smaller image.

- **Flow**: a two-component source-to-next displacement in output pixels. Positive X is right;
  positive Y is up. Destination = source pixel + Flow. This is not a normalized UV field or the
  packed four-channel past/future Speed pass required by Vector Blur. Use Flow to Speed, or the
  ready-wired Footage Motion Blur workflow described below, to make that conversion.
- **Confidence**: a 0–1 heuristic, not a probability or a guaranteed occlusion matte.
- **Quality**: Fast (up to 4 levels, 3 iterations, refinement radius 1), Balanced (6, 5, 2),
  High (8, 8, 3). Coarse initialization uses at least radius 2, even in Fast mode.
  Pyramids stop before the next level's smallest dimension falls below 12 pixels.
- **Max Motion**: radial cap in output pixels, default 64. It is not a capture-range guarantee.
- **Mask**: zero suppresses both outputs; soft values weight confidence, not displacement.
  The solver still analyzes the image outside the mask.
- **Consistency Check**: additionally estimates reverse flow to reduce unreliable confidence.
  The reverse estimate is skipped when Confidence is unconnected.

Flat, transparent, invalid, and out-of-frame areas cannot provide reliable measurements. Missing
spatial inputs or zero Max Motion produce zero outputs. Swapping frames computes a reverse field
at the other frame's coordinates; simply negating Flow is not an equivalent general operation.

## Implementation

The dependency-free dense solver follows a coarse-to-fine Lucas–Kanade approach, with independent
CPU and GPU implementations of the same stages:

1. Convert premultiplied color to alpha-weighted signed-log luminance using the active OCIO
   luminance coefficients. Invalid colors do not contaminate neighboring pyramid values.
2. Build a five-tap binomial Gaussian pyramid with ceil half sizes.
3. Initialize the coarsest field using bounded subpixel patch search (up to an 8-pixel search radius),
   rejecting unsupported or flat patches and preferring smaller motion on ties.
   Patch support is at least 5x5: 3x3 initialization produced false distant matches that were
   propagated into finer levels. Fast retains 3x3 refinement; only its coarse support is enlarged.
   Coarse seeds use quarter-pixel spacing when the level's motion cap exceeds four pixels and
   half-pixel spacing otherwise. This avoids rounding a small true motion into a worse match than a
   distant repeating patch. The default-range regression is separate from the original tests
   that explicitly limited Max Motion to 16 pixels.
   A weak patch-variance-normalized motion-length prior breaks ambiguous near-matches in favor
   of smaller motion; repeating patterns can still remain fundamentally ambiguous.
4. Refine each field with regularized, per-pixel local normal equations, bounded updates, bilinear
   target sampling, and a radial displacement cap. Upsample predictions by two between levels.
5. Calculate texture/support and photometric confidence; optionally check the warped reverse
   displacement. Masked, invalid or out-of-frame destinations output zero.

All internal stages and final outputs use full precision. Output-specific shaders avoid allocating
unused output images. Intermediates are released after use; no state is cached across frames.

Algorithm background: Jean-Yves Bouguet,
[Pyramidal Implementation of the Lucas Kanade Feature Tracker](https://robots.stanford.edu/cs223b04/algo_tracking.pdf).
This is a dense adaptation with additional initialization and reliability checks, not a copy of
the paper's sparse tracker.

## Scope

The Optical Flow node itself performs motion estimation only. Separate editable workflows now
connect that estimator to motion blur and frame interpolation. There is no learned model,
automatic retiming of an entire clip, or reconstruction of hidden surfaces. Repeated patterns, scene cuts,
uniform regions, large displacement, motion boundaries, lighting changes, and changing transparency
remain difficult cases. Confidence should be inspected rather than treating every vector as valid.
Accuracy and performance on production footage must be assessed per shot; no real-time claim is made.
CPU and GPU floating-point differences can change individual uncertain or thresholded pixels.
Use a consistent compositor device for a shot; the estimated workflows are not bit-identical
across backends, especially at occlusions and frame boundaries.

## Footage Motion Blur

**Add > Tracking > Footage Motion Blur** creates an ordinary editable node group, not a new opaque
blur implementation. If a single-layer Image node is selected, the operator connects its Image,
Previous Frame and Next Frame outputs. Otherwise connect those three images explicitly.

- Inputs: Previous Image, Image, Next Image, Shutter, Samples, Quality, Max Motion, Mask,
  Confidence Threshold. The three images must represent consecutive, equally spaced frames.
- Outputs: Image, Confidence, Motion Preview.
- Two Optical Flow nodes independently measure current-to-previous and current-to-next motion.
  Both fields are indexed at the current image's pixels. Negating a next-frame field is not a
  substitute for measuring the previous-frame field.
- Native **Flow to Speed** (`CompositorNodeFlowToSpeed`) packs
  `(previous.x, previous.y, -next.x, -next.y)`. Vector Blur applies its existing opposite sign to
  the future half of the shutter. Accepted vectors keep their full physical displacement;
  low-confidence halves are rejected rather than scaled into misleading slower motion.
- The existing Vector Blur node provides filtering and shutter controls. Depth is left at its
  uniform default for footage. Equal depth cannot resolve true front/back ordering.
- Vector Blur recognizes Speed produced by Flow to Speed as estimated motion. A rejected vector
  is not evidence of a stationary background. For estimated motion, both linear and curved blur
  skip nearest-background reconstruction and use visible image samples. This prevents a few
  unreliable pixels from spreading their colours into Voronoi-like cells. It applies to saved
  Footage Motion Blur groups without rewiring. Direct render-pass and extended-motion workflows
  retain their existing background reconstruction.
- Confidence is the minimum of the two accepted confidences. It remains a heuristic.

## Frame Interpolate

**Add > Tracking > Frame Interpolate** creates an editable group with Image, Next Image, Factor,
Quality, Max Motion, Mask, Confidence Threshold and Fallback inputs. It returns Image, Confidence
and Motion Preview. Factor 0 returns Image exactly, Factor 1 returns Next Image exactly, and 0.5
requests an in-between image. It does not change scene time or automatically traverse a clip.

Inside the group, two Optical Flow nodes estimate first-to-second and second-to-first motion.
The native **Flow Interpolate** (`CompositorNodeFlowInterpolate`) also accepts these fields and
their confidences directly, for externally generated flow or custom graphs.

Forward flow is defined at source pixels. Blender's Displace node performs a backward gather,
so simply scaling/negating a source-indexed field does not give a general inverse warp. The new
consumer solves `source + time * flow(source) = destination` with 12 bounded iterations, then
uses matching pixel-space bilinear gathers built on Blender's image access helpers on CPU/GPU.
Integer neighbour coordinates avoid round-off-induced border holes. It checks inverse residual, forward/backward consistency,
frame boundaries and supplied confidence before blending premultiplied colors.

Fallback applies where neither warped frame has usable support:

- **Crossfade**: blend the unwarped source frames (default).
- **Nearest Frame**: use the nearer unwarped original; ties use Next Image.
- **Transparent**: output transparent black.

Confidence is zero in fallback areas; exact original-frame endpoints report one because no
motion estimate is needed. This is not a calibrated probability. There is no generative hole
filling or guaranteed occlusion repair. Cuts, fast motion, disocclusions and repeating texture
can still ghost or distort; inspect results and use masks/fallback deliberately.

## Adjacent frames and cache lifetime

Single-layer **Image** nodes now have a collapsed **Adjacent Frames** panel containing
**Previous Frame** and **Next Frame**. Image and Alpha retain their existing identifiers and
behaviour. The extra outputs support image sequences and movies, use the same color space and
timing as Image, and do not mutate the stored ImageUser. Still images return the same image.
Non-cyclic sequences hold at their configured duration boundaries; cyclic sequences wrap.
Multilayer EXR pass declarations remain unchanged.

All three outputs use Blender's existing CachedImageContainer: effective frame, pass, view and
image identity determine frame reuse, and the image update counter invalidates changed data.
Reload footage after changing files on disk, just as for a normal Image node. No private pointer
cache or cross-frame solver state is introduced. Flow is recomputed from current inputs and
settings; persistent caching of solved flow fields is not part of this implementation.

## Motion preview

Native **Flow Visualize** (`CompositorNodeFlowVisualize`) accepts Flow, Confidence and Display
Range and returns a viewable color image. Hue is direction; brightness is motion length divided
by Display Range, clamped to one and weighted by confidence. Rightward motion is red and leftward
motion cyan. Zero/rejected motion is black. Preview alpha is one. This display image is not a
motion-data format and must not be connected to Speed as if it contained raw vectors.

## Workflow verification

The Blender 5.3 Beta Windows Release build passed these checks on 2026-11-08:

- `tests/python/compositor_flow_workflow_test.py`: 66 cases per device with both OpenGL and
  Vulkan. Includes analytical native consumers, both-output shader variants, exact-frame
  endpoints, constant folding, affine inverse mapping, HDR/alpha, opaque-border preservation,
  mask/fallback behaviour, two editable groups, timing boundaries, reload invalidation and
  copy/save/reload. The added rejection cases cover linear blur, curved blur, and a Speed field
  aligned to a larger image. They check that a change in a rejected pixel's colour cannot affect
  distant pixels, the intended blur remains visible, and opaque footage stays opaque.
- The same suite exercises `tests/files/tracking/footage/fabric-eingang.avi`, downsampled to
  97x65 for a bounded real-movie workflow smoke. A separate GPU test rendered one 1920x1088 frame
  with its actual adjacent frames. This is not a production-shot accuracy or performance benchmark.
- The 257x145 camera-pan reproduction no longer shows the background cells on CPU or GPU. At
  the 95th percentile, the channel error relative to exact-vector blur dropped from about 0.140
  to 0.0049 on this fixture.
- A Footage Motion Blur group saved by the previous Alpha build renders on CPU and GPU with its
  existing links. All eleven direct Vector Blur test scripts passed on CPU and GPU with OpenGL.
- Analytical consumers use strict maximum-error comparisons. Estimated workflows use full-frame
  mean and 99th-percentile error limits, plus a strict known-translation interior check; a few
  ambiguous boundary pixels can differ more. Confidence remains a heuristic, not a probability.

Additional checks on the 2026-10-18 Alpha build:

- `compositor_flow_workflow_ui_smoke.py`: real editor creation, automatic frame links, non-overlap,
  undo/redo and drawing passed. Groups are placed beside their source Image node, keep its parent
  frame and avoid existing nodes.
- `compositor_flow_workflow_compatibility_test.py`: a file saved with the pre-change executable
  reopens with Image/Alpha identifiers, links and timing intact; still-image adjacent outputs agree.
- Existing artist-node, IBK and Vector Blur motion/custom-shutter regressions pass. Film Grain's
  source is unchanged. Standalone source contracts, MSVC syntax checks, shader-to-C++ checks and
  wiki TypeScript/ESLint checks are separate from those native tests.

Regression fixes found during implementation: preserve single-value outputs instead of emitting
one-pixel images; initialize fractional coarse motion with an ambiguity prior at the default search
range; and use integer bilinear neighbours to avoid CPU border opacity/coordinate round-off errors.

## Validation

The Windows Release builds were tested on 2026-10-18 (Alpha) and 2026-11-08 (Beta):

- 23 generated cases per device, CPU/GPU comparisons and saved-default persistence passed with
  both OpenGL and Vulkan on NVIDIA GTX 1080 Ti.
- Cases include zero, integer/subpixel/large/reverse/local motion, masks, alpha/HDR, flat and
  missing inputs, displacement caps, occlusion confidence and output independence. Two cases
  explicitly cover the default 64-pixel range, in addition to the earlier 21-case suite.
- Fast's (3, -2)-pixel translation regression improved from 1.706 pixels to 0.004 pixels at the
  90th percentile after enlarging coarse initialization support. The original accuracy threshold
  is unchanged. This is a synthetic-fixture result, not a general accuracy guarantee.
- Native type and menu registration are included in the Windows Release build.
- Source registration, MSVC syntax-only, and shader-to-C++ checks also pass. Those static checks
  are separate from the executed native rendering and driver shader tests.

`tests/python/compositor_optical_flow_test.py` provides generated analytical fixtures for zero,
integer/subpixel/large/reverse/local motion, masks, alpha/HDR, flat and missing inputs, range caps,
occlusion confidence, output independence, persistence, and CPU/GPU comparisons on an odd image size.

```text
python tests/python/compositor_optical_flow_test.py --source-only
blender -b --factory-startup --python-exit-code 1 --python tests/python/compositor_optical_flow_test.py -- --device BOTH
```

CTest registers the native CPU suite as `compositor_optical_flow`. Use a supported GPU context for
`--device BOTH`; passing syntax checks is not a substitute for running that suite.
