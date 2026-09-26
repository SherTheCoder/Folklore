from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import venv

import PIL


class PythonDiscovery(unittest.TestCase):
    def test_prefers_local_venv_without_resolving_its_interpreter(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            environment = root / '.venv'
            venv.EnvBuilder(with_pip=False, symlinks=True).create(environment)
            site = environment / 'lib' / f'python{sys.version_info.major}.{sys.version_info.minor}' / 'site-packages'
            (site / 'real-pillow.pth').write_text(str(Path(PIL.__file__).resolve().parents[1]) + '\n')
            script = Path(__file__).with_name('find_python.py').resolve()
            result = subprocess.run([sys.executable, str(script)], cwd=root, text=True, capture_output=True, check=True)
            self.assertEqual(result.stdout.strip(), str(environment / 'bin' / 'python'))


if __name__ == '__main__':
    unittest.main()
