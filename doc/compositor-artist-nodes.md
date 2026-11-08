# Compositor artist nodes: phased implementation

These are native compositor nodes. The implementation adds new node types;
existing node types and saved defaults are not replaced. All three are now included in the single
Windows Release build, alongside Optical Flow and the existing custom features.

## Phase 1: Color Match

`CompositorNodeColorMatch`, under Color > Adjust.

- Image and Reference are analyzed independently, so reference resolution need not match the source.
- Source Mask and Reference Mask choose the comparison regions. They do not limit where correction
  is applied; use Factor for that. Masks follow their respective images' compositing space and
  are not automatically stretched to fit an unrelated image.
- Color mode matches RGB means and standard deviations. Luminance uses the active OCIO luminance
  coefficients and applies one gain/offset to all channels. Exposure matches positive mean
  luminance with one multiplier, without an offset.
- Contrast blends the standard-deviation match; it is ignored in Exposure mode. Factor blends
  the final correction and can be an image mask.
- Fully transparent, non-finite and extreme pixels do not contaminate analysis. Soft masks and
  alpha weight the statistics. A flat source uses unit contrast gain rather than dividing by zero.
- Missing reference or empty analysis passes the image through and sets Valid to false. Gain/Offset
  describe the correction before Factor; no HDR clipping is applied and the source alpha is preserved.
- Lock Match copies the latest evaluated Gain/Offset into ordinary Stored Gain/Stored Offset
  socket defaults on the main thread. Unlock Match returns to per-frame analysis. The lock and
  saved values survive copy/save/reload and can be undone. Locking requires a valid compositor
  evaluation and an editable scene compositor context; it is not a render-thread cache or a
  time-dependent first-frame latch. The two stored inputs must be unlinked when capturing a lock.
  Lock/unlock is rejected while a render or compositor job is still running. Use a completed
  interactive compositor preview: background final-render logs stay on the evaluated scene and
  are not published to the editable node editor.

## Phase 2: Film Grain / Regrain

`CompositorNodeFilmGrain`, displayed as Film Grain, under Creative.

- Amount is a scene-linear noise standard deviation, before tonal response.
- Red Size, Green Size and Blue Size set independent channel grain sizes in output pixels. Color
  blends from monochrome to independent RGB patterns; monochrome uses red size. Intermediate color
  amounts can change the per-channel variance because the patterns are being blended.
- Shadows, Midtones, Highlights and Pivot shape a continuous luminance response. The active OCIO
  luminance coefficients are used. All three response strengths set to one give a flat response.
- Seed is deterministic. Animate includes the integer scene frame in the seed; disabling Animate
  keeps a fixed pattern. Integer hashing is mirrored on CPU/GPU, avoiding sine-based random noise.
- Spatial interpolation is variance-normalized. Grain on a constant image still produces an image,
  not one random value broadcast across the frame. Data-window offsets preserve crop alignment.
- Mask and alpha scale the additive grain. Alpha is unchanged, transparent pixels remain unchanged,
  and negative/HDR color values are not clipped.
- Regrain is a manual workflow: adjust Amount, the three grain sizes and tonal response to the plate.
  Automatic grain estimation, frequency-spectrum matching, and automatic degrain/regrain are future
  work, not implemented by this phase.

## Phase 3: Relight

`CompositorNodeRelight`, under Color > Adjust.

- Takes the image, signed world-space Normal, world-space Position, unlit Diffuse Color/albedo,
  and a Mask. Normal and Position must use the same coordinate space as the light controls.
- Directional uses Direction **toward** the light. Point uses Light Position and optionally
  inverse-square distance falloff. Minimum Distance limits near-light singularities.
- Normal vectors and light directions are normalized. Invalid/zero normals and a point exactly
  at the light contribute no light. A missing Normal pass gets a visible informational warning.
- Diffuse Color is not Diffuse Direct or a Combined render. Strength is an artistic diffuse-light
  multiplier, not physical watts. Lighting is the masked premultiplied contribution; Image adds
  that contribution to the input while preserving input alpha.
- This is Lambertian diffuse relighting only. It does not synthesize cast shadows, occlusion,
  reflections, specular highlights, hidden surfaces, or physically accurate light transport.
- Only connected output images are allocated on the CPU/GPU path.

## Phase 4: validation and documentation

- Native analytical fixtures cover Color Match modes, masks, flat/empty regions, alpha/HDR,
  independent reference sizes, correction locking, and persistence.
- Grain fixtures cover deterministic seeds, animation/static behavior, RGB sizing, tonal
  response, mask/alpha behavior, spatial generation on constant inputs, and CPU/GPU parity.
- Relight fixtures cover directional/point behavior, normal normalization, falloff, zero normals,
  diffuse color, mask/alpha, Lighting output and output parity.
- CTest registration runs these on CPU. `--device BOTH` additionally checks GPU parity on a
  machine with a supported GPU context. Source-contract and independent mathematical tests run
  without Blender; they do not replace executing the native node tests.
- The wiki has a dedicated page for each node. None of these major nodes is filed as Miscellaneous.

### Verification status

Completed static checks:

- MSVC syntax-only checks for all three node implementations; no object files or executable generated.
- Blender's shader-to-C++ syntax checks for all four kernels, including both Relight outputs.
  These are not GPU-driver compilation or image-parity tests.
- Source registration/wiring contracts and independent mathematical reference tests.
- Python parsing, C++/shader formatting, whitespace checks, wiki TypeScript/ESLint, and local wiki routes.

Completed against the Blender 5.3.0 Alpha Windows Release executable on 2026-10-18:

- `compositor_artist_nodes_test.py --device BOTH`: 41 analytical cases per device,
  CPU/GPU comparisons, and copy/save/reload passed on OpenGL and Vulkan (NVIDIA GTX 1080 Ti).
- `compositor_color_match_lock_ui_smoke.py`: the real interactive compositor publishes a valid
  correction; the lock/unlock operator, captured values, and undo/redo passed. The test waits for
  compositor jobs to finish between operations, preserving the production job-safety guard.
- All three node types instantiate. The Add-menu script is installed with the build.
- Optical Flow's independent 21-case suite also passes CPU/OpenGL/Vulkan and persistence; see
  [Optical Flow](compositor-optical-flow.md) for its separate contract and limitations.

These are analytical regression results, not production-shot visual approval or a performance
benchmark. Automatic grain estimation and physically complete relighting remain unimplemented.

Repeat the runtime checks:

```text
blender -b --factory-startup --python-exit-code 1 --python tests/python/compositor_artist_nodes_test.py -- --device BOTH
blender --factory-startup --python-exit-code 1 --python tests/python/compositor_color_match_lock_ui_smoke.py
```

Run the same checks against the target Blender build to confirm these nodes remain available.
