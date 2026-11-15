"""Reproduce the pinned public comparison inputs without redistributing their records."""

import argparse
import concurrent.futures
import json
import urllib.request
from pathlib import Path

from measure.output import digest

ROOT = Path(__file__).resolve().parents[1]


def fetch(entry, destination):
    path = destination / entry["file"]
    if not path.exists() or digest(path) != entry["sha256"]:
        request = urllib.request.Request(
            entry["source"], headers={"User-Agent": "Ambergris-public-record-analysis/1"}
        )
        with urllib.request.urlopen(request, timeout=90) as source, path.open("wb") as output:
            while chunk := source.read(1024 * 1024):
                output.write(chunk)
        if digest(path) != entry["sha256"]:
            raise ValueError("Public artifact hash mismatch: " + entry["file"])
    return {**entry, "file": str(path)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", choices=("emergence", "onefthree", "moltbook"))
    parser.add_argument("--output", type=Path)
    options = parser.parse_args()
    destination = (options.output or ROOT / ".cache/measure-sources" / options.source).resolve()
    if not destination.is_relative_to(ROOT):
        raise ValueError("Keep public study files inside this workspace")
    destination.mkdir(parents=True, exist_ok=True)
    declaration = json.loads(Path(__file__).with_name("sources.json").read_text())[options.source]
    entries = declaration["worlds"] if options.source == "emergence" else declaration["files"]
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
        files = list(executor.map(lambda entry: fetch(entry, destination), entries))
    if options.source == "emergence":
        manifest = {"emergence": {**declaration, "worlds": files}}
        (destination / "study-input.json").write_text(json.dumps(manifest, indent=2) + "\n")
    else:
        (destination / "source-manifest.json").write_text(
            json.dumps({**declaration, "files": files}, indent=2) + "\n"
        )
    print(
        json.dumps({"source": options.source, "files": len(files), "directory": str(destination)})
    )


if __name__ == "__main__":
    main()
