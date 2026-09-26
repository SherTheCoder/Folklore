# Procedural flower asset factory

Headless Blender generates separable, shaded botanical sprites for an STM32N657
2.5D compositor. The Python scripts and references are the source of truth. A
saved `flower.blend` is optional, disposable output; it is never read as input.

Confirmed configuration: **800 × 480 composite frame, 16 main petals, one sprite
per twig**. The open scene has 50 independently movable elements: center, 16
petals, 30 filament florets, two compound-leaf twigs, and glow. Five bloom samples
produce 250 sprite samples. The gold filament fans and olive fernlike leaves are
procedural approximations; reference images are not traced or used as textures.

## Commands

Run from this directory. All Blender calls are background calls with mandatory
`--factory-startup`, and all script arguments follow `--`.

```sh
make preview       # EEVEE; out/preview/composite.png, 800 x 480
make layers        # Cycles; open pose, 50 sprites plus manifest/contact sheet
make poses         # Cycles; 5 poses, 250 sprites plus manifest/contact sheet
make all           # poses, then a separate Cycles whole-scene reference render
make check         # packer boundary tests; no Blender required
make stages        # camera -> petals -> leaves -> materials diagnostic renders
make clean         # remove generated out/ and Python caches only
```

`make layers` and `make poses` replace the root manifest with that run's set.
Only files listed in the manifest belong to the current build; older files are
not silently consumed. `make clean` removes them. The comparison composite is
separate so `make all` does not overwrite the firmware manifest.

```sh
BLENDER=/path/to/blender make all
make preview EXTRA='--samples 4'
make poses STEPS=3 EXTRA='--fast'
make layers OUT=out/checks/small EXTRA='--width 200 --height 120 --samples 16'
make layers EXTRA='--only petal_03 floret_03 leaf_L glow'
make check PYTHON=/path/to/python-with-pillow
```

`BLENDER` defaults to `/Applications/Blender.app/Contents/MacOS/Blender`; both
environment and Make command-line overrides work. `PYTHON` is a **system** Python
with Pillow, selected by `scripts/find_python.py` from the local virtualenv,
system Python, or the installed Codex image runtime. If none exists, `make setup`
creates `.venv` and installs the pinned `scripts/requirements.txt` dependency.
Blender does not import Pillow or NumPy.

Equivalent direct commands:

```sh
export BLENDER="${BLENDER:-/Applications/Blender.app/Contents/MacOS/Blender}"
"$BLENDER" --version
"$BLENDER" -b --factory-startup -P scripts/build_flower.py -- --mode poses --steps 5 --save-blend
python3 scripts/pack_assets.py --manifest out/source_manifest.json
python3 scripts/verify_assets.py out/manifest.json
```

Use the selected Pillow-capable interpreter instead of `python3` for the last
two commands if necessary. Blender is never launched with a GUI, even when
saving the optional scene.

## Rebuild from scripts/ and refs/ alone

Canonical build and documentation files also live under `scripts/`. If literally
everything else, including the root Makefile, has been deleted, GNU Make needs
its surviving entrypoint specified once:

```sh
make -f scripts/Makefile all
```

That restores the root Makefile and README; subsequent builds use `make all`.
Deleting only generated outputs requires no bootstrap. A bare `make all` cannot
find a Makefile that has itself been deleted.

## Image and firmware contract

- Camera is orthographic, at `(0, 0, 14)`, looking down -Z. XY is the artwork
  plane. Camera and three area lights stay fixed for every element and pose.
- Every raw PNG is **3200 × 1920**, four times the target frame on each axis.
  Geometry borders reduce rendered work without changing the canvas or camera.
- `out/layers/petal_03_bloom_0.50.png` is a full-frame **straight-alpha** RGBA8
  PNG. Standard view transform, None look, exposure 0, gamma 1, transparent film,
  no compositor, no glare/bloom. Cycles uses CPU and denoising.
- The packer filters **premultiplied** RGB and alpha with Lanczos, converts back
  to straight alpha for `out/sprites/*.png`, then trims the nonzero-alpha bbox.
  Hidden RGB never leaks into edges. The trimmed PNGs remain normal PNGs.
- `out/sprites/*.rgba` contains the runtime payload: tightly packed RGBA8888,
  **premultiplied** sRGB byte values, row-major from the top left, no header.
  Blend with source ONE, destination ONE_MINUS_SRC_ALPHA. Do not premultiply
  these bytes again. PNG has no premultiplied-alpha flag, so premultiplied data
  is deliberately supplied as raw bytes rather than mislabeled PNG content.
- `out/manifest.json` is the firmware entrypoint. `source_manifest.json` is the
  renderer-to-packer handoff. Before packing, the renderer also writes an initial
  manifest with `status: rendered`; firmware should require `status: packed`.

Each manifest `renders` record represents one element at one bloom sample:

| Field | Meaning |
| --- | --- |
| `source`, `source_dimensions`, `source_alpha_bbox` | Full-resolution source PNG, dimensions and tight bounds |
| `png`, `rgba`, `dimensions`, `row_stride_bytes` | Trimmed files and packed sprite layout |
| `alpha_bbox` | Target-frame `[left, top, right, bottom]`; right/bottom exclusive |
| `trim_offset_px` | Target-frame location of the trimmed sprite's top-left corner |
| `pivot_composite_px` | Projected Empty in **800 × 480** top-left frame pixels |
| `pivot_source_px` | Same pivot in the 3200 × 1920 source frame |
| `pivot_sprite_px` | Pivot relative to trimmed texture; may lie outside the texture |
| `rest_rotation_euler_rad`, `rest_scale` | Actual object rest transform already baked into the image |
| `runtime_rotation_deg`, `runtime_scale` | Identity delta transform: 0 degrees, `[1,1]` |
| `draw_order`, `bloom` | Ascending back-to-front order and pose value |
| `rgba_bytes`, `rgba_sha256` | Payload size and integrity hash |

Coordinates are continuous pixel-edge coordinates: first pixel center is
`(0.5, 0.5)`. For a local sprite point `p`, the resting placement is:

```text
frame_point = pivot_composite_px + (p - pivot_sprite_px)
```

Apply runtime scale/rotation/perspective to the parenthesized displacement and
then translate to the frame pivot. The resting transform must not be applied a
second time: mesh orientations are baked. A child `<object>_pivot` Empty sits at
each petal/stem base. Hinge locations remain fixed across bloom textures.

Cross-fading adjacent poses must align both by their own `pivot_sprite_px`, not
by their independently trimmed top-left corners. Weight both premultiplied RGB
and alpha together when interpolating. Two normal source-over draws at weighted
opacity approximate a cross-fade; exact premultiplied interpolation requires a
temporary combined sprite or a suitable blending operation.

Glow is drawn first, as its own radial emission-plane texture. Audio-driven
opacity must multiply **all four premultiplied RGBA channels** by the same factor;
changing alpha alone does not dim source RGB with a source-ONE blend. It is
hidden for all other layer renders and cannot
bake a halo into their textures. Rendering each collection alone also excludes
neighboring objects' shadows and indirect light. The whole-scene comparison has
natural inter-object shading; the `reconstructed_bloom_*.png` files show the
actual sprite-compositor result and are the authoritative runtime layout check.

## Tuning parameters

The flat block at the top of `scripts/build_flower.py` is the art-direction
interface. Colors are linear RGBA. Useful ranges are commented next to each
constant; no geometry or material is loaded from a saved scene.

| Parameters | Effect |
| --- | --- |
| `TARGET_WIDTH`, `TARGET_HEIGHT` | Default composite resolution, 800 × 480 |
| `PETAL_COUNT`, `PETAL_BASE_RADIUS` | Main petal count and hinge circle |
| `PETAL_LENGTH`, `PETAL_WIDTH`, `PETAL_TAPER` | Main petal silhouette |
| `PETAL_PITCH`, `PETAL_CURL`, `PETAL_TWIST` | Real 3D bending and surface orientation |
| `PETAL_COLOR_ROOT`, `PETAL_COLOR_TIP`, `PETAL_EDGE_DARKENING` | Surface color ramp |
| `PETAL_ROUGHNESS`, `PETAL_SUBSURFACE`, `PETAL_TRANSLUCENCY` | Baked lighting response |
| `FLOWER_RADIUS`, `FLOWER_FRAME_HEIGHT` | Cluster size and approximate reference framing |
| `FLORET_COUNT`, `FLORET_STAMENS`, `FLORET_SIZE` | Individual filament fan density and scale |
| `FILAMENT_RADIUS`, `ANTHER_RADIUS` | Stem thickness and tip bead size |
| `LEAF_LENGTH`, `LEAF_CURL`, `LEAF_BRANCH_PAIRS`, `LEAFLET_PAIRS` | Compound twig shape |
| `LEAF_COLOR_ROOT`, `LEAF_COLOR_TIP` | Olive shading ramp |
| `GLOW_RADIUS`, `GLOW_ALPHA`, `GLOW_COLOR` | Independent halo layer |
| `RNG_SEED` | Deterministic variation, default 657 |
| `FAST_SAMPLES`, `CYCLES_SAMPLES` | 12-sample EEVEE and 48-sample denoised Cycles |

`--bloom 0..1` selects one state for composite/layers; `--mode poses --steps N`
includes both endpoints, default 0.00, 0.25, 0.50, 0.75, 1.00. Petals extend and
uncurl while floret fans open. Twigs, center and glow remain static but retain
explicit records at each pose for a simple uniform firmware lookup.

Reference dimensions are read from their PNG headers every run: supplied files
are 2816 × 1536. `refs/leaf_sprig.png` is accepted as an alias when
`refs/leaf_twig.png` is absent. Framing approximates the flower's 68% height
occupancy and adapts the reference's aspect ratio to the 800 × 480 panel.

## Versions, reproducibility and verification

Development checks use Blender **4.5.14 LTS**. The renderer branches on
`bpy.app.version`: EEVEE Next on 4.2+, legacy EEVEE on 3.6–4.1. A socket resolver
handles Emission/Emission Color, Specular/Specular IOR Level and Subsurface/
Subsurface Weight. Older branches are compatibility paths, not claimed tested
installations. Importing Blender's bundled `world_to_camera_view` was explicitly
approved as the sole addition to bpy, mathutils and the standard library.

Fixed seed, CPU Cycles, no adaptive sampling, fixed camera, fixed sample counts,
and pinned Pillow keep rebuilds repeatable on the same toolchain. Blender and
denoiser changes can change pixels; a `.blend` container is not promised to have
an identical hash. The packer strips Blender's timestamp/render-duration PNG
text chunks without recompressing pixel data. Runtime manifests contain no
wall-clock timestamps.

Each render prints `IMAGE_REPORT` with path, dimensions, encoded byte size, exact
alpha bbox and a nonempty/non-full-frame plausibility check. Packing rechecks
the PNGs independently with Pillow and writes `out/image_reports.json`.
`out/contact_sheet.png` tiles every rendered layer/pose for human visual review;
`out/reconstructed_bloom_*.png` places the sprites using manifest offsets.
`verify_assets.py` checks every payload, SHA, bbox, projected/trimmed pivot
relationship, stable pose hinges and complete pose coverage.

If a `--fast` render exceeds 60 seconds, the script reports it, halves the
samples, and rerenders that output. It preserves dimensions and supersampling.
This timing-dependent fallback means preview pixels can vary across machines
or runs that cross the threshold; reproducible production builds use Cycles.
On this macOS host Blender's Metal startup crashed inside the sandbox before
Python ran; allowing the same **headless** command outside the sandbox worked.
This is a host execution requirement, not a reason to open Blender's GUI.
