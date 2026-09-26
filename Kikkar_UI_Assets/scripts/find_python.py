from pathlib import Path
import shutil
import subprocess
import sys

candidates = [Path('.venv/bin/python'), Path(sys.executable)]
bundled = Path.home() / '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3'
candidates.append(bundled)
for command in ('python3', 'python'):
    found = shutil.which(command)
    if found:
        candidates.append(Path(found))
for candidate in candidates:
    if candidate.is_file() and subprocess.run([str(candidate.absolute()), '-c', 'import PIL'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
        print(candidate.absolute())
        break
else:
    print('.venv/bin/python')
