"""Launch repository Python with public CAD library paths, without reading secrets."""

import json
import os
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
runtime = root / ".cache/bake-runtime.json"
environment = os.environ.copy()
if runtime.exists():
    data = json.loads(runtime.read_text())
    environment["LD_LIBRARY_PATH"] = ":".join(data["library_paths"])
os.chdir(root)
os.execve(root / ".venv/bin/python", [str(root / ".venv/bin/python"), *sys.argv[1:]], environment)
