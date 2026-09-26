#!/usr/bin/env python3
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

from PIL import Image, ImageDraw


def canonicalize_png(path):
    """Drops Blender's text stamps without re-encoding the image data."""
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path}: not a PNG')
    parts, offset = [data[:8]], 8
    while offset < len(data):
        length = struct.unpack_from('>I', data, offset)[0]
        end = offset + length + 12
        if end > len(data):
            raise ValueError(f'{path}: truncated PNG chunk')
        kind = data[offset + 4:offset + 8]
        if kind not in (b'tEXt', b'zTXt', b'iTXt', b'tIME'):
            parts.append(data[offset:end])
        offset = end
    result = b''.join(parts)
    if result != data:
        path.write_bytes(result)


def process_image(image, target_size, pivot_full_px):
    image = image.convert('RGBA')
    width, height = target_size
    if width <= 0 or height <= 0:
        raise ValueError('target dimensions must be positive')
    if image.width * height != image.height * width:
        raise ValueError('resize must preserve aspect ratio')
    if image.size != (width, height):
        image = image.convert('RGBa').resize((width, height), Image.Resampling.LANCZOS).convert('RGBA')
    bbox = image.getchannel('A').getbbox()
    if bbox is None:
        raise ValueError('empty alpha layer')
    cropped = image.crop(bbox)
    return {
        'preview': cropped,
        'rgba': cropped.convert('RGBa').tobytes(),
        'bbox': list(bbox),
        'pivot_px': [pivot_full_px[0] - bbox[0], pivot_full_px[1] - bbox[1]],
    }


def image_report(path):
    with Image.open(path) as im:
        im.load()
        bbox = im.convert('RGBA').getchannel('A').getbbox()
        return {'path': str(path), 'dimensions': list(im.size),
                'bytes': path.stat().st_size, 'alpha_bbox': list(bbox) if bbox else None,
                'alpha_plausible': bbox is not None and bbox != (0, 0, im.width, im.height)}


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def contact_sheet(samples, out):
    tile, label_height, columns = 176, 36, min(6, len(samples))
    sheet = Image.new('RGB', (columns * tile, math.ceil(len(samples) / columns) * (tile + label_height)), (17, 21, 24))
    draw = ImageDraw.Draw(sheet)
    for i, sample in enumerate(samples):
        x, y = (i % columns) * tile, (i // columns) * (tile + label_height)
        for cy in range(0, tile, 16):
            for cx in range(0, tile, 16):
                c = 37 if (cx // 16 + cy // 16) % 2 else 29
                draw.rectangle((x + cx, y + cy, x + cx + 15, y + cy + 15), fill=(c, c, c))
        with Image.open(out / sample['png']) as im:
            im.thumbnail((tile - 12, tile - 12), Image.Resampling.LANCZOS)
            sheet.paste(im, (x + (tile - im.width) // 2, y + (tile - im.height) // 2), im)
        draw.text((x + 5, y + tile + 3), f"{sample['element']}  b={sample['bloom']:.2f}", fill=(241, 213, 151))
        draw.text((x + 5, y + tile + 18), f"{sample['dimensions'][0]} x {sample['dimensions'][1]}", fill=(160, 171, 177))
    sheet.save(out / 'contact_sheet.png')


def pack(source_path):
    source = json.loads(source_path.read_text())
    out = source_path.parent
    frame = source['frame']['target_dimensions']
    factor = source['frame']['supersample']
    manifest = dict(source)
    manifest['status'] = 'packed'
    manifest['coordinate_system'] = {
        'origin': 'top-left', 'units': 'target composite-frame pixels',
        'pixel_coordinates': 'pixel edges; center of first pixel is (0.5, 0.5)',
        'bbox': '[left, top, right, bottom], exclusive right and bottom',
        'rest_transform': 'baked into PNG; runtime identity is rotation=0 and scale=(1,1)',
    }
    manifest['runtime_format'] = {
        'format': 'RGBA8888', 'channel_order': 'R,G,B,A', 'alpha': 'premultiplied',
        'color_space': 'sRGB byte values premultiplied by coverage',
        'blend': 'src ONE, dst ONE_MINUS_SRC_ALPHA',
        'rows': 'top to bottom, tightly packed; row_stride_bytes=width*4',
        'png_alpha': 'straight / unassociated',
    }
    packed = []
    reports = []
    for item in source['renders']:
        src = out / item['source']
        canonicalize_png(src)
        report = image_report(src)
        reports.append(report)
        print(json.dumps(report), flush=True)
        if not report['alpha_plausible']:
            raise ValueError(f'{src}: alpha is empty or covers the full frame')
        if report['dimensions'] != [frame[0] * factor, frame[1] * factor]:
            raise ValueError(f'{src}: inconsistent supersampled dimensions')
        with Image.open(src) as im:
            if item['element'] == 'composite':
                preview = im.convert('RGBa').resize(tuple(frame), Image.Resampling.LANCZOS).convert('RGBA')
                dst = out / 'composite.png'
                preview.save(dst)
                manifest['composite'] = {'png': 'composite.png', 'dimensions': frame,
                                         'alpha_bbox': list(preview.getchannel('A').getbbox())}
                continue
            pivot = item['pivot_composite_px']
            result = process_image(im, tuple(frame), pivot)
        stem = Path(item['source']).stem
        png = Path('sprites') / (stem + '.png')
        raw = Path('sprites') / (stem + '.rgba')
        (out / png).parent.mkdir(parents=True, exist_ok=True)
        result['preview'].save(out / png, optimize=False, compress_level=9)
        (out / raw).write_bytes(result['rgba'])
        sample = {**item, 'png': png.as_posix(), 'rgba': raw.as_posix(),
                  'bytes': report['bytes'],
                  'source_dimensions': report['dimensions'], 'source_alpha_bbox': report['alpha_bbox'],
                  'dimensions': list(result['preview'].size), 'alpha_bbox': result['bbox'],
                  'trim_offset_px': result['bbox'][:2], 'pivot_sprite_px': result['pivot_px'],
                  'row_stride_bytes': result['preview'].width * 4, 'rgba_bytes': len(result['rgba']),
                  'rgba_sha256': hashlib.sha256(result['rgba']).hexdigest()}
        packed.append(sample)
    manifest['renders'] = packed
    unique_payloads = {sample['rgba_sha256']: sample['rgba_bytes'] for sample in packed}
    manifest['statistics'] = {'sprite_samples': len(packed),
                              'total_rgba_bytes': sum(sample['rgba_bytes'] for sample in packed),
                              'unique_rgba_bytes': sum(unique_payloads.values()),
                              'unique_payloads': len(unique_payloads)}
    write_json(out / 'manifest.json', manifest)
    write_json(out / 'image_reports.json', reports)
    if packed:
        contact_sheet(packed, out)
        poses = sorted({sample['bloom'] for sample in packed})
        for pose in poses:
            canvas = Image.new('RGBA', tuple(frame))
            for sample in sorted((s for s in packed if s['bloom'] == pose), key=lambda s: s['draw_order']):
                with Image.open(out / sample['png']) as im:
                    canvas.alpha_composite(im, tuple(sample['trim_offset_px']))
            canvas.save(out / f'reconstructed_bloom_{pose:.2f}.png')
        print(f'PACKED {len(packed)} sprites; {sum(s["rgba_bytes"] for s in packed)} RGBA bytes; {out / "contact_sheet.png"}', flush=True)
    return manifest


def main():
    parser = argparse.ArgumentParser(description='Filter, trim and pack the rendered layers for the MCU.')
    parser.add_argument('--manifest', type=Path, default=Path('out/source_manifest.json'))
    parser.add_argument('--inspect', type=Path, nargs='+')
    args = parser.parse_args()
    if args.inspect:
        for path in args.inspect:
            print(json.dumps(image_report(path)))
    else:
        pack(args.manifest)


if __name__ == '__main__':
    main()
