#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p client/dist
.tools/bin/godot --headless --path client --export-release Web "$PWD/client/dist/index.html"
python3 - <<'PY'
import gzip
from pathlib import Path
for artifact in Path('client/dist').iterdir():
    if artifact.suffix in {'.wasm', '.pck', '.js'}:
        artifact.with_name(artifact.name + '.gz').write_bytes(gzip.compress(artifact.read_bytes(), compresslevel=6, mtime=0))
PY
