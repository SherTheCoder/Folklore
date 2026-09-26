# Procedural flower asset factory

The supplied brief is the design specification. Work directly in this dedicated,
non-Git asset directory. The user authorizes implementation and sensible defaults
with confirmed choices of an 800x480 frame, 16 petals, and one sprite per twig.
The bpy_extras projection-helper import exception is explicitly approved.

## Architecture

`build_flower.py` constructs a deterministic orthographic scene from a flat
parameter block, performs staged renders, and records full-frame source geometry
and image metadata. `pack_assets.py` filters premultiplied colors and alpha before
trimming, emits straight-alpha preview PNGs and premultiplied RGBA8888 bytes, and
converts pivots into target-frame and trimmed-sprite coordinates. The manifest
defines top-left pixel-edge coordinates and exclusive bbox upper bounds.

## Tasks and verification

- [x] Write packer boundary tests with tiny, hand-calculated images: alpha-aware
  reduction excludes invisible RGB; premultiplication rounds correctly; trimming
  preserves a pivot outside the sprite; empty layers fail clearly.
- [x] Implement system-Python packer, per-image numeric reports, contact sheets,
  runtime reconstruction, and strict manifest consistency checks.
- [x] Build camera/lights plus a diagnostic center. Render a small validation
  frame headlessly, check dimensions, byte size, and nontrivial alpha bounds.
- [x] Add curved petal geometry and render the petals stage.
- [x] Add curling compound leaves and render the leaves stage.
- [x] Add shaded materials, floret filaments/anthers, independent radial emission
  glow, deterministic poses and collection-isolated renders. Render final stage.
- [x] Package Make targets and parameter/version/runtime documentation. Keep
  canonical Makefile and README under scripts so deletion of artifacts is safe.
- [x] Run packer tests, all staged renders, a Cycles smoke render, layer/pose
  integration validation and a deterministic repeated-render check. Report all
  alpha bounds numerically; the user reviews rendered appearance.

## Decisions

- Fixed camera looks down -Z, XY is the artwork plane, no baked perspective.
- Raw layers share the supersampled composite canvas; 4x is fixed. Trimming is
  deferred to the packer so projected pivots never depend on object bounds.
- Gold filament fans approximate refs; reference images are not mapped to meshes.
- `leaf_sprig.png` is accepted as the supplied alias for `leaf_twig.png`.
- CPU Cycles, fixed seed, Standard / None, transparent film, no compositor.
- Production Cycles render: 50 elements x 5 bloom values, 3200x1920 raw frames.
- Review corrected inward bead normals and virtualenv symlink handling; each
  fix has a regression test. Setup also repairs an incomplete default virtualenv.
