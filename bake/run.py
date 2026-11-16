"""Launch repository Python with public CAD library paths, without reading secrets."""

import json
import os
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
runtime = root / ".cache/bake-runtime.json"
environment = os.environ.copy()
python = str(root / ".venv/bin/python")
arguments = [python, *sys.argv[1:]]
executable = python
if runtime.exists():
    data = json.loads(runtime.read_text())
    executable = data["loader"]
    arguments = [executable, "--library-path", ":".join(data["library_paths"]), *arguments]
os.chdir(root)
os.execve(executable, arguments, environment)
