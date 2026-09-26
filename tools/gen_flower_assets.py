#!/usr/bin/env python3

import argparse
import json
import os
import struct
import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.stderr.write(
        "gen_flower_assets.py needs Pillow.\n"
        "  Kikkar_UI_Assets/scripts/find_python.py locates a Pillow-capable\n"
        "  interpreter; pass it as PYTHON=... to the build.\n")
    raise

KIND_GLOW, KIND_LEAF, KIND_CENTER, KIND_PETAL, KIND_FLORET = range(5)
KIND_NAMES = ["GLOW", "LEAF", "CENTER", "PETAL", "FLORET"]

TEX_ARGB4444, TEX_A8 = 0, 1
TEX_FORMAT_NAMES = {TEX_ARGB4444: "ARGB4444", TEX_A8: "A8"}
TEX_BPP = {TEX_ARGB4444: 2, TEX_A8: 1}

STATIC_ELEMENTS = ("center", "glow", "leaf_L", "leaf_R")

BAYER4 = [
    [0, 8, 2, 10],
    [12, 4, 14, 6],
    [3, 11, 1, 9],
    [15, 7, 13, 5],
]

SPRITE_ALIGN = 32
Q4 = 16.0


def classify(element):
    if element == "glow":
        return KIND_GLOW, 0
    if element.startswith("leaf"):
        return KIND_LEAF, 0 if element.endswith("_L") else 1
    if element == "center":
        return KIND_CENTER, 0
    if element.startswith("petal_"):
        return KIND_PETAL, int(element.split("_")[1])
    if element.startswith("floret_"):
        return KIND_FLORET, int(element.split("_")[1])
    raise ValueError("unclassifiable manifest element: %r" % element)


def q4(value):
    """Pixel coordinate to Q12.4 fixed point, range-checked."""
    v = int(round(value * Q4))
    if not -32768 <= v <= 32767:
        raise ValueError("coordinate %r does not fit Q12.4" % value)
    return v


def pack_argb4444(img, dither):
    w, h = img.size
    src = img.load()
    out = bytearray(w * h * 2)
    o = 0
    for y in range(h):
        row = BAYER4[y & 3]
        for x in range(w):
            r, g, b, a = src[x, y]
            if dither:
                t = (row[x & 3] + 0.5) * (17.0 / 16.0) - 8.5
                r = min(255, max(0, int(r + t)))
                g = min(255, max(0, int(g + t)))
                b = min(255, max(0, int(b + t)))
                a = min(255, max(0, int(a + t)))
            r4 = (r * 15 + 127) // 255
            g4 = (g * 15 + 127) // 255
            b4 = (b * 15 + 127) // 255
            a4 = (a * 15 + 127) // 255
            halfword = (a4 << 12) | (r4 << 8) | (g4 << 4) | b4
            out[o] = halfword & 0xFF
            out[o + 1] = (halfword >> 8) & 0xFF
            o += 2
    return bytes(out)


def pack_a8(img):
    return img.split()[3].tobytes()


def glow_colour(img):
    """Mean colour of the glow, and how far it strays from a single hue."""
    src = img.load()
    w, h = img.size
    n = 0
    sums = [0, 0, 0]
    lo = [255, 255, 255]
    hi = [0, 0, 0]
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            r, g, b, a = src[x, y]
            if a < 32:
                continue
            n += 1
            for i, c in enumerate((r, g, b)):
                sums[i] += c
                lo[i] = min(lo[i], c)
                hi[i] = max(hi[i], c)
    if n == 0:
        return (255, 255, 255), 0
    mean = tuple(s // n for s in sums)
    spread = max(hi[i] - lo[i] for i in range(3))
    return mean, spread


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)

    ap = argparse.ArgumentParser(description="Pack Kikkar_UI_Assets/out into the firmware's texture atlas and tables.",
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest",
                    default=os.path.join(root, "Kikkar_UI_Assets", "out", "manifest.json"))
    ap.add_argument("--out-inc", default=os.path.join(root, "Appli", "Core", "Inc"))
    ap.add_argument("--out-src", default=os.path.join(root, "Appli", "Core", "Src"))
    ap.add_argument("--poses", default="0.0,1.0",
                    help="bloom poses to bake, ascending (default: 0.0,1.0)")
    ap.add_argument("--alpha", choices=("straight", "premultiplied"), default="straight",
                    help="straight for the DMA2D backend, premultiplied for NemaGFX")
    ap.add_argument("--no-dither", action="store_true")
    ap.add_argument("--glow-a8", dest="glow_a8", action="store_true", default=True)
    ap.add_argument("--no-glow-a8", dest="glow_a8", action="store_false")
    ap.add_argument("--scale", type=float, default=1.0,
                    help="resample every sprite and every coordinate by this "
                         "factor (default 1.0). The art is authored for an "
                         "800x480 frame; the STM32N657's usable SRAM cannot "
                         "hold framebuffers plus a full-size atlas, so the "
                         "whole composition is scaled down at build time "
                         "rather than cropped or stripped of elements.")
    ap.add_argument("--kind-scale", action="append", default=[],
                    metavar="KIND=FACTOR",
                    help="extra resample factor for one kind of element, e.g. "
                         "leaf=1.5.  The sprite grows about its own anchor, so "
                         "a leaf twig gets longer without moving where it "
                         "joins the flower.  Repeatable; kinds are glow, leaf, "
                         "center, petal, floret.  The source layers are "
                         "3200x1920, four times the composite, so factors well "
                         "above 1 are still downsampling and stay sharp.")
    ap.add_argument("--compose-margin", type=int, default=12,
                    help="slack in px around the flower bbox for the compose buffer")
    ap.add_argument("--max-bytes", type=int, default=0,
                    help="fail if the atlas exceeds this many bytes (0 = no cap)")
    args = ap.parse_args()

    poses = [float(p) for p in args.poses.split(",") if p.strip()]
    if not poses:
        ap.error("--poses must list at least one bloom value")
    if poses != sorted(poses):
        ap.error("--poses must be ascending")

    with open(args.manifest) as fh:
        manifest = json.load(fh)

    if manifest.get("status") != "packed":
        sys.exit("manifest status is %r, expected 'packed' -- run `make poses` in "
                 "Kikkar_UI_Assets first" % manifest.get("status"))
    if manifest.get("schema_version") != 1:
        sys.exit("unsupported manifest schema_version %r" % manifest.get("schema_version"))

    asset_root = os.path.dirname(os.path.abspath(args.manifest))
    frame_w, frame_h = manifest["frame"]["target_dimensions"]

    sc = args.scale
    if not (0.05 <= sc <= 1.0):
        ap.error("--scale must be in 0.05..1.0")

    kind_names = {"glow": KIND_GLOW, "leaf": KIND_LEAF, "center": KIND_CENTER,
                  "petal": KIND_PETAL, "floret": KIND_FLORET}
    kind_scale = {v: 1.0 for v in kind_names.values()}
    for spec in args.kind_scale:
        kname, _, fac = spec.partition("=")
        if kname not in kind_names:
            ap.error("--kind-scale: unknown kind %r (want one of %s)"
                     % (kname, ", ".join(sorted(kind_names))))
        kind_scale[kind_names[kname]] = float(fac)
    if sc != 1.0:
        frame_w = int(round(frame_w * sc))
        frame_h = int(round(frame_h * sc))

    available = set(manifest["bloom_steps"])
    for p in poses:
        if p not in available:
            sys.exit("bloom pose %g is not in the manifest (has %s)"
                     % (p, sorted(available)))

    by_element = {}
    for rec in manifest["renders"]:
        if rec["bloom"] not in poses:
            continue
        by_element.setdefault(rec["element"], {})[rec["bloom"]] = rec

    elements = sorted(by_element, key=lambda e: by_element[e][poses[0]]["draw_order"])
    for name in elements:
        missing = [p for p in poses if p not in by_element[name]]
        if missing:
            sys.exit("element %s is missing bloom pose(s) %s" % (name, missing))

    for name in STATIC_ELEMENTS:
        if name not in by_element:
            continue
        shas = {by_element[name][p]["rgba_sha256"] for p in poses}
        if len(shas) != 1:
            sys.exit("element %s was assumed pose-invariant but its payload "
                     "changes across poses -- update STATIC_ELEMENTS" % name)

    atlas = bytearray()
    textures = []
    tex_index = {}
    glow_rgb = (255, 84, 5)
    glow_spread = 0

    for name in elements:
        for pose in poses:
            rec = by_element[name][pose]
            png = os.path.join(asset_root, rec["png"])
            ks  = kind_scale[classify(name)[0]]
            esc = sc * ks
            key = (rec["rgba_sha256"], ks)
            if key in tex_index:
                continue

            img = Image.open(png).convert("RGBA")
            if tuple(img.size) != tuple(rec["dimensions"]):
                sys.exit("%s is %s but the manifest says %s"
                         % (rec["png"], img.size, rec["dimensions"]))

            if esc != 1.0:
                w2 = max(1, int(round(img.size[0] * esc)))
                h2 = max(1, int(round(img.size[1] * esc)))
                px = img.load()
                for yy in range(img.size[1]):
                    for xx in range(img.size[0]):
                        r, g, b, a = px[xx, yy]
                        px[xx, yy] = (r * a // 255, g * a // 255, b * a // 255, a)
                img = img.resize((w2, h2), Image.LANCZOS)
                px = img.load()
                for yy in range(h2):
                    for xx in range(w2):
                        r, g, b, a = px[xx, yy]
                        if a == 0:
                            px[xx, yy] = (0, 0, 0, 0)
                        else:
                            px[xx, yy] = (min(255, r * 255 // a),
                                          min(255, g * 255 // a),
                                          min(255, b * 255 // a), a)

            fmt = TEX_ARGB4444
            if name == "glow" and args.glow_a8:
                glow_rgb, glow_spread = glow_colour(img)
                if glow_spread > 96:
                    sys.stderr.write(
                        "note: glow hue spread is %d/255, too wide for A8 -- "
                        "storing it as ARGB4444 instead\n" % glow_spread)
                else:
                    fmt = TEX_A8

            if args.alpha == "premultiplied" and fmt == TEX_ARGB4444:
                px = img.load()
                for y in range(img.size[1]):
                    for x in range(img.size[0]):
                        r, g, b, a = px[x, y]
                        px[x, y] = (r * a // 255, g * a // 255, b * a // 255, a)

            blob = (pack_a8(img) if fmt == TEX_A8
                    else pack_argb4444(img, not args.no_dither))

            pad = (-len(atlas)) % SPRITE_ALIGN
            atlas.extend(b"\0" * pad)
            offset = len(atlas)
            atlas.extend(blob)

            tex_index[key] = len(textures)
            textures.append({
                "name": os.path.splitext(os.path.basename(rec["png"]))[0],
                "w": img.size[0],
                "h": img.size[1],
                "fmt": fmt,
                "offset": offset,
                "bytes": len(blob),
                "pivot_sx": rec["pivot_sprite_px"][0] * esc,
                "pivot_sy": rec["pivot_sprite_px"][1] * esc,
            })

    if args.max_bytes and len(atlas) > args.max_bytes:
        sys.exit("atlas is %d bytes, over the %d byte budget by %d -- drop a pose "
                 "(--poses) or shrink the compose buffer"
                 % (len(atlas), args.max_bytes, len(atlas) - args.max_bytes))

    els = []
    bbox = [frame_w, frame_h, 0, 0]
    rot_bbox = [frame_w, frame_h, 0, 0]
    for name in elements:
        base = by_element[name][poses[0]]
        kind, index = classify(name)
        tex_ids = []
        for pose in poses:
            rec = by_element[name][pose]
            ks = kind_scale[kind]
            tex_ids.append(tex_index[(rec["rgba_sha256"], ks)])
            l, t, r, b = [v * sc for v in rec["alpha_bbox"]]
            if ks != 1.0:
                ax = rec["pivot_composite_px"][0] * sc
                ay = rec["pivot_composite_px"][1] * sc
                l, r = ax + (l - ax) * ks, ax + (r - ax) * ks
                t, b = ay + (t - ay) * ks, ay + (b - ay) * ks
            bbox = [min(bbox[0], int(l)), min(bbox[1], int(t)),
                    max(bbox[2], int(round(r))), max(bbox[3], int(round(b)))]
            if kind in (KIND_CENTER, KIND_PETAL, KIND_FLORET):
                rot_bbox = [min(rot_bbox[0], int(l)), min(rot_bbox[1], int(t)),
                            max(rot_bbox[2], int(round(r))),
                            max(rot_bbox[3], int(round(b)))]
        els.append({
            "name": name,
            "kind": kind,
            "index": index,
            "draw_order": base["draw_order"],
            "pivot_fx": base["pivot_composite_px"][0] * sc,
            "pivot_fy": base["pivot_composite_px"][1] * sc,
            "tex": tex_ids,
        })

    for name in elements:
        pivots = {tuple(by_element[name][p]["pivot_composite_px"]) for p in poses}
        if len(pivots) != 1:
            sys.exit("element %s moves its composite pivot between poses (%s); "
                     "cross-fading would smear it" % (name, pivots))

    m = args.compose_margin
    cx0 = max(0, (rot_bbox[0] - m) & ~1)
    cy0 = max(0, rot_bbox[1] - m)
    cx1 = min(frame_w, rot_bbox[2] + m)
    cy1 = min(frame_h, rot_bbox[3] + m)
    cw = (cx1 - cx0 + 1) & ~1
    ch = cy1 - cy0

    petals = sum(1 for e in els if e["kind"] == KIND_PETAL)
    florets = sum(1 for e in els if e["kind"] == KIND_FLORET)

    os.makedirs(args.out_inc, exist_ok=True)
    os.makedirs(args.out_src, exist_ok=True)
    banner = "/* Generated by tools/gen_flower_assets.py. Do not edit. */\n"

    h = [banner, "#ifndef FLOWER_ASSETS_H\n#define FLOWER_ASSETS_H\n\n",
         "#include <stdint.h>\n\n"]
    h.append("#define FLOWER_ART_SCALE_PCT      %d\n\n" % int(round(sc * 100)))
    for _kn, _kv in (("GLOW", KIND_GLOW), ("LEAF", KIND_LEAF),
                     ("CENTER", KIND_CENTER), ("PETAL", KIND_PETAL),
                     ("FLORET", KIND_FLORET)):
        h.append("#define FLOWER_KIND_SCALE_%s_PCT%s%d\n"
                 % (_kn, " " * (12 - len(_kn)), int(round(kind_scale[_kv] * 100))))
    h.append("\n")
    h.append("#define FLOWER_FRAME_W            %d\n" % frame_w)
    h.append("#define FLOWER_FRAME_H            %d\n\n" % frame_h)
    h.append("#define FLOWER_BBOX_X0            %d\n" % bbox[0])
    h.append("#define FLOWER_BBOX_Y0            %d\n" % bbox[1])
    h.append("#define FLOWER_BBOX_X1            %d\n" % bbox[2])
    h.append("#define FLOWER_BBOX_Y1            %d\n\n" % bbox[3])
    h.append("#define FLOWER_ROT_BBOX_X0        %d\n" % rot_bbox[0])
    h.append("#define FLOWER_ROT_BBOX_Y0        %d\n" % rot_bbox[1])
    h.append("#define FLOWER_ROT_BBOX_X1        %d\n" % rot_bbox[2])
    h.append("#define FLOWER_ROT_BBOX_Y1        %d\n\n" % rot_bbox[3])
    h.append("#define FLOWER_COMPOSE_X0         %d\n" % cx0)
    h.append("#define FLOWER_COMPOSE_Y0         %d\n" % cy0)
    h.append("#define FLOWER_COMPOSE_W          %d\n" % cw)
    h.append("#define FLOWER_COMPOSE_H          %d\n\n" % ch)
    h.append("#define FLOWER_POSE_COUNT         %d\n" % len(poses))
    h.append("#define FLOWER_ELEMENT_COUNT      %d\n" % len(els))
    h.append("#define FLOWER_TEXTURE_COUNT      %d\n" % len(textures))
    h.append("#define FLOWER_PETAL_COUNT        %d\n" % petals)
    h.append("#define FLOWER_FLORET_COUNT       %d\n" % florets)
    h.append("#define FLOWER_ATLAS_BYTES        %du\n\n" % len(atlas))
    h.append("#define FLOWER_POSE_BLOOM_Q4      { %s }\n\n"
             % ", ".join(str(q4(p)) for p in poses))
    h.append("#define FLOWER_GLOW_RGB           0x%02X%02X%02Xu\n\n" % glow_rgb)
    for k, v in sorted(TEX_FORMAT_NAMES.items()):
        h.append("#define FLOWER_TEX_%-12s %d\n" % (v, k))
    h.append("\n")
    for i, n in enumerate(KIND_NAMES):
        h.append("#define FLOWER_KIND_%-10s %d\n" % (n, i))
    h.append("""
typedef struct
{
    uint32_t offset;
    uint16_t w;
    uint16_t h;
    uint8_t  format;
    uint8_t  reserved;
    int16_t  pivot_x_q4;
    int16_t  pivot_y_q4;
} flower_tex_t;

typedef struct
{
    uint16_t draw_order;
    uint8_t  kind;
    uint8_t  index;
    int16_t  pivot_fx_q4;
    int16_t  pivot_fy_q4;
    uint16_t tex[FLOWER_POSE_COUNT];
} flower_element_t;

extern const uint8_t          flower_atlas_blob[];
extern const flower_tex_t     flower_textures[FLOWER_TEXTURE_COUNT];
extern const flower_element_t flower_elements[FLOWER_ELEMENT_COUNT];

#endif /* FLOWER_ASSETS_H */
""")

    c = [banner, '#include "flower_assets.h"\n\n',
         "const flower_tex_t flower_textures[FLOWER_TEXTURE_COUNT] =\n{\n"]
    for t in textures:
        c.append("    { %8du, %4d, %4d, FLOWER_TEX_%-8s, 0, %7d, %7d },\n"
                 % (t["offset"], t["w"], t["h"], TEX_FORMAT_NAMES[t["fmt"]],
                    q4(t["pivot_sx"]), q4(t["pivot_sy"])))
    c.append("};\n\nconst flower_element_t flower_elements[FLOWER_ELEMENT_COUNT] =\n{\n")
    for e in els:
        c.append("    { %4d, FLOWER_KIND_%-7s, %2d, %7d, %7d, { %s } },\n"
                 % (e["draw_order"], KIND_NAMES[e["kind"]], e["index"],
                    q4(e["pivot_fx"]), q4(e["pivot_fy"]),
                    ", ".join("%3d" % i for i in e["tex"])))
    c.append("};\n")

    s = [banner,
         """
#ifndef FLOWER_ATLAS_BIN
#define FLOWER_ATLAS_BIN "../Core/Src/flower_atlas.bin"
#endif

    .section .flower_atlas, "a", %progbits
    .global  flower_atlas_blob
    .type    flower_atlas_blob, %object
    .balign  32
flower_atlas_blob:
    .incbin  FLOWER_ATLAS_BIN
    .size    flower_atlas_blob, . - flower_atlas_blob
""".lstrip("\n")]

    def write(path, text):
        with open(path, "w") as fh:
            fh.write(text)

    write(os.path.join(args.out_inc, "flower_assets.h"), "".join(h))
    write(os.path.join(args.out_src, "flower_assets.c"), "".join(c))
    write(os.path.join(args.out_src, "flower_atlas.S"), "".join(s))
    with open(os.path.join(args.out_src, "flower_atlas.bin"), "wb") as fh:
        fh.write(atlas)

    per_kind = {}
    counted = set()
    for e in els:
        for tid in e["tex"]:
            if tid in counted:
                continue
            counted.add(tid)
            per_kind.setdefault(KIND_NAMES[e["kind"]], [0, 0])
            per_kind[KIND_NAMES[e["kind"]]][0] += 1
            per_kind[KIND_NAMES[e["kind"]]][1] += textures[tid]["bytes"]

    print("flower assets: %d elements, %d unique textures, poses %s, alpha=%s, scale=%.2f"
          % (len(els), len(textures), poses, args.alpha, sc))
    for k in KIND_NAMES:
        if k in per_kind:
            n, b = per_kind[k]
            print("  %-7s %3d tex %9d B (%6.1f KiB)" % (k, n, b, b / 1024.0))
    print("  %-7s %3d tex %9d B (%6.1f KiB)  <- .flower_atlas"
          % ("TOTAL", len(textures), len(atlas), len(atlas) / 1024.0))
    glow_tex = next((t for t in textures if t["name"].startswith("glow")), None)
    if glow_tex is not None:
        print("  glow stored as %s, flat colour #%02X%02X%02X (hue spread %d/255)"
              % (TEX_FORMAT_NAMES[glow_tex["fmt"]],
                 glow_rgb[0], glow_rgb[1], glow_rgb[2], glow_spread))
    print("  rotating bbox %dx%d at (%d,%d)"
          % (rot_bbox[2] - rot_bbox[0], rot_bbox[3] - rot_bbox[1],
             rot_bbox[0], rot_bbox[1]))
    print("  flower bbox %dx%d at (%d,%d); compose buffer %dx%d at (%d,%d) = %d B"
          % (bbox[2] - bbox[0], bbox[3] - bbox[1], bbox[0], bbox[1],
             cw, ch, cx0, cy0, cw * ch * 2))
    if args.max_bytes:
        print("  budget %d B, %d B spare (%.1f%%)"
              % (args.max_bytes, args.max_bytes - len(atlas),
                 100.0 * (args.max_bytes - len(atlas)) / args.max_bytes))


if __name__ == "__main__":
    main()
