# Vector Blur exposure reconstruction

With Curved off, Vector Blur uses Blender's directional gather filter. It samples
the input image along straight directions, using the pixel and neighboring tile
motion with direction, spread, and depth weights. Falloff None uses uniform
exposure weighting. There is no extra sharp copy of the moving original.

The directional filter reconstructs up to four neighbouring tile proposals and
interpolates their resulting colours/opacity at tile centres. Opposing vectors
stay separate, so their directions cannot cancel. Identical proposals are merged
into one. This replaces discrete, jittered tile selection that could imprint a
32-pixel grid onto smooth input. The pixel's own direction provides a recovery
sample path without abruptly replacing a shorter tile vector. Conservative tile
dilation includes the neighbouring tiles required by this interpolation.

Guidance and pixel recovery paths use the requested number of samples. Their
contributions are weighted by the distance each path integrates. A short path
therefore does not acquire excessive influence from its higher spatial sample
density. Shared support between guidance and recovery paths is balanced so it
is not counted twice. Proposal support ends at the shutter segment; it is not
extended by the image sampling footprint. This prevents antialiased silhouette
vectors, which can be mixed with zero-motion background, from imprinting a narrow
contour at the current shape.
Input vectors are preserved, including genuinely slow sources; no longest-vector
substitution is used to repair silhouette pixels.

With Curved on and genuinely nonlinear motion, the node distributes source pixels
along approximations of their quadratic or cubic paths. Entirely straight motion
fields use the directional filter in either setting, so constant-velocity motion
does not change just because Curved is checked.

The Falloff menu defines a curve independently for every source path:

| Choice | Curve value at fractional shutter time t |
| --- | --- |
| None | 1 |
| Linear | 1 - t |
| Smooth | 1 - t*t*(3 - 2*t) |
| Ease In | 1 - t*t |
| Ease Out | (1 - t)*(1 - t) |
| Custom | Editable shutter curve over the full previous-to-next interval |

Each shutter half starts at t=0 and ends at t=1. Linear is the default for new
nodes. None means a uniform window; saved choices are preserved. The directional
filter obtains each contribution's phase from its
distance divided by that sampled pixel's own motion length, rather than the longest
tile vector's phase. With a selected fade in directional mode, colour reconstruction
and streak opacity are resolved separately. The moving contributions are averaged
over valid foreground samples rather than the total number of foreground and empty
background samples. Their intrinsic alpha is multiplied by the curve once, so an
isolated opaque source starts near opacity one and fades toward zero without an
additional exposure-density attenuation. Overlapping sources still filter their
colours. Each sample represents an interval clipped to its source's shutter window;
its fade is the curve's average over that interval. This keeps endpoint weights
continuous despite filtered-vector rounding and preserves exposure on short paths.
Intervals beyond the source window cannot enter the conditional opacity denominator.
This prevents extra attenuation of short windows. There is
no separately preserved sharp original. Fade values stay in [0, 1].

Foreground coverage is stored independently of source alpha. At silhouette edges,
motion and depth are interpolated from moving sources only; stationary or empty
pixels must not shorten the motion used to evaluate the curve. The sampling
directions, tile proposals, and sample count remain those of the directional filter.

Opacity normalization removes exposure density while retaining spatial footprint
coverage. The strongest valid bilinear footprint controls geometric coverage, so
a grazing sample stays partially covered rather than becoming an opaque contour.
The current source footprint participates in coverage without adding its sharp
colour as a separate image layer. Motion/depth means use their own interpolation
weight sum, independent of hardware texture-filter rounding.
At the shutter boundary, a partially intersecting sample bin also limits its
footprint by the fraction inside the source interval. Conditional normalization
must not amplify a nearly empty intersection into a visible edge, including when
a custom curve has nonzero endpoint strength.

Slow moving pixels remain part of the foreground footprint rather than becoming
background seeds below the half-pixel blur threshold. Subpixel motion can still
return unchanged colour when all available directions are below that threshold.

The stationary/transparent background estimate fills the current moving footprint
and uncovered areas. A fully moving image instead normalizes its colour kernel and
retains its intrinsic alpha, keeping camera motion opaque. None resolves uniform
moving exposure over this background estimate too, retaining the native depth and
spread tests. At equal depth, moving-source coverage receives full weight. Uniform
exposure density uses each sampled source's motion length plus the half-pixel area
of the spread ramp. Intrinsic alpha is retained, including slow translucent sources.
This avoids using the current moving silhouette as a background colour estimate. When
no stationary background exists, full-image reconstruction retains its existing
sampling weights; None retains the native foreground/background colour resolve.

Custom expands a native Curve Mapping editor inside the node, with the same
Smooth, Round, Root, Sharp, Linear and Uniform preset buttons as Render > Motion
Blur. The horizontal axis spans the previous endpoint (0), current frame (0.5),
and next endpoint (1). The vertical axis is contribution strength, clamped to
[0, 1]. A new custom curve is a linear tent with a peak at the current frame.
Control points and handles can be edited independently on either side.

CPU and GPU evaluate the same 1,025-knot window and its integral, baked once for
each execution. Directional samples average the curve over their represented
source-relative intervals, preserving the silhouette and short-motion fixes.
Nonlinear curved exposure uses that same interval average. Custom normalizes
curved exposure once over the full window, so an asymmetric curve retains the
relative contribution of its two halves. A completely closed curve has no moving
exposure and does not divide by zero. Existing presets retain their established
normalization and algorithms.

In directional mode, tile vectors only propose sample locations; each contribution
uses its own motion for weighting and falloff. Colors and motion data come from
the same sample location. Curved interpolation still
uses three frame positions, or five with a connected Speed Extended input.
Vector Extended remains disabled by default; linking the enabled matching pass
still preserves explicit links.

Samples directly controls image samples in directional mode: low values can alias
or produce gaps and higher values improve the filter. The old automatic floor no
longer replaces 1 and 64 with the same count. In nonlinear curved mode, Samples
controls the number of curve segments per shutter half. Spatial coverage along the
chords remains filled independently, so long paths do not develop empty slices
merely because geometric approximation is coarse. Source contributions use bilinear
footprints, midpoint weights, and a bounded 8,192 spatial samples per shutter half.

In nonlinear curved mode, the moving layer is composited over the stationary image.
To replace the original
moving silhouette, a nearest stationary/transparent background estimate fills its
current footprint. The existing compositor jump-flooding algorithm provides that
estimate on CPU and GPU. Fully moving opaque images use spatial-coverage normalization
instead, so camera motion blurs the texture without making the image transparent.
A positive Depth input lets opaque stationary surfaces occlude farther contributions.

This remains a reconstruction from one frame: hidden backgrounds are estimated,
not recovered from the scene. Complex overlaps between moving layers and deformation
can differ from renderer motion blur. The normalized cubic is extrapolated beyond
its known frame samples. Sampling has a finite upper bound. None of those limitations
is a reason to retain a sharp moving original or use another pixel's falloff.

CPU and GPU use the same algorithms and windows. In nonlinear curved mode, the GPU
uses portable compare-and-swap float accumulation in a uint buffer and short dispatches
rather than requiring a vendor-specific float-atomic extension. It processes a compact
list of moving, nonempty sources. Cost increases with moving area and path length;
the directional filter retains tile reduction, dilation, and gather stages. Varying
motion can require up to four distinct guidance reconstructions per pixel; equal
directions share one reconstruction. This continuity fix increases gather work in
regions where neighbouring tiles disagree.

Regression tests:

- compositor_vector_blur_exposure_test.py: softening, per-source falloff, long-trail
  coverage, small shutter increments, returning curves above shutter two, camera motion.
- compositor_vector_blur_falloff_test.py: falloff choices, alpha, geometry, persistence.
- compositor_vector_blur_motion_test.py: constant/Extended inputs and connection behavior.
- compositor_vector_blur_curved_test.py and compositor_vector_blur_cubic_test.py:
  trajectory shape and linear-motion equivalence.
- compositor_vector_blur_directional_test.py: stock filter snapshots, effective
  Samples control, constant velocity inputs, straight paths in Curved mode, and
  curve approximation quality.
- compositor_vector_blur_opacity_test.py: measured opacity profile, opaque-background
  contrast, source alpha, HDR, per-source lengths, and filtered colour without a sharp copy.
- compositor_vector_blur_edges_test.py: antialiasing of a diagonal pixel footprint,
  strong streak centres, and premultiplied edge alpha.
- compositor_vector_blur_tiles_test.py: continuity of a smooth input with varying
  affine motion across guidance tiles, with and without opacity falloff.
- compositor_vector_blur_silhouette_test.py: source contours with coverage-mixed
  motion at 32/256 samples, uniform and Smooth windows, opaque background alpha,
  and genuine slow and subpixel sources beside faster guidance.
- compositor_vector_blur_shutter_test.py: Linear default, native shutter presets,
  custom-curve equivalence, independent shutter halves, per-source timing,
  curved normalization, closed shutter, deep copies, and save/reload.
