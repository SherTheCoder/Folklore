import importlib.util
from pathlib import Path
import tempfile
import unittest

from PIL import Image, PngImagePlugin


class PackerContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = Path(__file__).with_name('pack_assets.py')
        cls.module_path = path
        if path.exists():
            spec = importlib.util.spec_from_file_location('pack_assets', path)
            cls.pack = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(cls.pack)

    def require_packer(self):
        self.assertTrue(self.module_path.exists(), 'packer implementation is missing')

    def test_invisible_red_does_not_pollute_blue_edge(self):
        self.require_packer()
        im = Image.new('RGBA', (8, 8), (255, 0, 0, 0))
        for y in range(8):
            for x in range(4):
                im.putpixel((x, y), (0, 0, 255, 255))
        result = self.pack.process_image(im, (2, 2), (1.5, 1.0))
        for r, g, b, a in result['preview'].get_flattened_data():
            if a:
                self.assertEqual((r, g, b), (0, 0, 255))
        self.assertEqual(result['bbox'], [0, 0, 2, 2])

    def test_trim_preserves_off_sprite_pivot(self):
        self.require_packer()
        im = Image.new('RGBA', (10, 8))
        for y in range(3, 6):
            for x in range(4, 7):
                im.putpixel((x, y), (200, 100, 50, 128))
        result = self.pack.process_image(im, im.size, (2.5, 7.0))
        self.assertEqual(result['bbox'], [4, 3, 7, 6])
        self.assertEqual(result['pivot_px'], [-1.5, 4.0])
        self.assertEqual(result['preview'].size, (3, 3))
        self.assertEqual(result['rgba'][:4], bytes((100, 50, 25, 128)))

    def test_empty_layer_is_an_error(self):
        self.require_packer()
        with self.assertRaisesRegex(ValueError, 'empty'):
            self.pack.process_image(Image.new('RGBA', (8, 8)), (2, 2), (0, 0))

    def test_rejects_anisotropic_resize(self):
        self.require_packer()
        with self.assertRaisesRegex(ValueError, 'aspect'):
            self.pack.process_image(Image.new('RGBA', (8, 4)), (2, 2), (0, 0))

    def test_removes_volatile_png_metadata_without_changing_pixels(self):
        self.require_packer()
        self.assertTrue(hasattr(self.pack, 'canonicalize_png'), 'PNG canonicalization is missing')
        with tempfile.TemporaryDirectory() as directory:
            paths = [Path(directory) / f'{i}.png' for i in range(2)]
            for i, path in enumerate(paths):
                info = PngImagePlugin.PngInfo()
                info.add_text('Date', f'2026/09/18 19:0{i}:00')
                Image.new('RGBA', (2, 2), (30, 80, 160, 128)).save(path, pnginfo=info)
                self.pack.canonicalize_png(path)
                with Image.open(path) as im:
                    self.assertEqual(im.getpixel((0, 0)), (30, 80, 160, 128))
                    self.assertNotIn('Date', im.info)
            self.assertEqual(paths[0].read_bytes(), paths[1].read_bytes())


if __name__ == '__main__':
    unittest.main()
