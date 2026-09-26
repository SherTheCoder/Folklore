from pathlib import Path
import shutil

root = Path(__file__).resolve().parents[1]
for path in (root / 'out', root / 'scripts' / '__pycache__'):
    if path.is_symlink():
        path.unlink()
    elif path.exists():
        shutil.rmtree(path)
print('Removed generated out/ and scripts/__pycache__/')
