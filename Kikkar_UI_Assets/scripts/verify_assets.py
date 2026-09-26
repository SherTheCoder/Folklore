import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image


def verify(path):
    manifest = json.loads(path.read_text())
    assert manifest['status'] == 'packed', 'manifest has not been packed'
    width, height = manifest['frame']['target_dimensions']
    assert manifest['frame']['supersample'] == 4
    samples = manifest['renders']
    assert samples, 'no sprite samples'
    expected = {element['element'] for element in manifest['elements']}
    if manifest.get('selection'):
        expected &= set(manifest['selection'])
    assert {sample['element'] for sample in samples} == expected, 'missing or unexpected elements'
    keys = set()
    by_name = {}
    for sample in samples:
        name, bloom = sample['element'], sample['bloom']
        key = (name, bloom)
        assert key not in keys, f'duplicate {key}'
        keys.add(key)
        by_name.setdefault(name, []).append(sample)
        with Image.open(path.parent / sample['source']) as im:
            assert im.size == (width * 4, height * 4), key
            assert list(im.getchannel('A').getbbox()) == sample['source_alpha_bbox'], key
        with Image.open(path.parent / sample['png']) as im:
            assert im.mode == 'RGBA' and list(im.size) == sample['dimensions'], key
            assert im.getchannel('A').getbbox() == (0, 0, *im.size), f'not tightly trimmed: {key}'
            expected_bytes = im.convert('RGBa').tobytes()
        raw = (path.parent / sample['rgba']).read_bytes()
        assert raw == expected_bytes, f'premultiplication mismatch: {key}'
        assert len(raw) == sample['rgba_bytes'] == sample['dimensions'][0] * sample['dimensions'][1] * 4
        assert hashlib.sha256(raw).hexdigest() == sample['rgba_sha256']
        left, top, right, bottom = sample['alpha_bbox']
        assert [right - left, bottom - top] == sample['dimensions']
        assert sample['trim_offset_px'] == [left, top]
        for i, offset in enumerate((left, top)):
            assert abs(sample['pivot_sprite_px'][i] + offset - sample['pivot_composite_px'][i]) < 1e-6, key
            assert abs(sample['pivot_source_px'][i] - 4 * sample['pivot_composite_px'][i]) < 1e-6, key
    steps = manifest['bloom_steps']
    for name, group in by_name.items():
        assert sorted(s['bloom'] for s in group) == sorted(steps), f'missing poses: {name}'
        assert len({tuple(s['pivot_composite_px']) for s in group}) == 1, f'moving hinge: {name}'
    if 'glow' in by_name:
        assert all(s['draw_order'] == 0 for s in by_name['glow'])
    print(f'VERIFIED {len(samples)} sprites, {len(by_name)} elements, {len(steps)} poses, frame {width}x{height}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Check the packed assets against the firmware's image contract.")
    parser.add_argument('manifest', type=Path)
    verify(parser.parse_args().manifest)
