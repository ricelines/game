#!/usr/bin/env python3
"""Summarize the latest Ninja timing record for each native build output."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def summarize(directory: Path) -> dict:
    latest = {}
    for line in (directory / ".ninja_log").read_text().splitlines():
        if line.startswith("#"):
            continue
        start, end, _, output, command = line.split("\t")
        latest[output] = (int(start), int(end), command)

    groups = {kind: [] for kind in ("compile", "archive", "link", "other")}
    seen = set()
    for output, (start, end, command) in latest.items():
        action = (start, end, command)
        if action in seen:
            continue
        seen.add(action)
        path = directory / output
        if output.endswith((".o", ".gch", ".pch", ".gcm", ".pcm")):
            kind = "compile"
        elif output.endswith(".a"):
            kind = "archive"
        elif path.is_file():
            with path.open("rb") as stream:
                kind = "link" if stream.read(4) == b"\x7fELF" else "other"
        else:
            kind = "other"
        groups[kind].append({"output": output, "seconds": (end - start) / 1000})

    return {
        "directory": str(directory),
        "basis": (
            "Latest recorded action per output; parallel action times are not elapsed build time."
        ),
        "groups": {
            kind: {
                "actions": len(rows),
                "action_seconds": round(sum(row["seconds"] for row in rows), 3),
                "slowest": sorted(rows, key=lambda row: row["seconds"], reverse=True)[:5],
            }
            for kind, rows in groups.items()
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", nargs="+", type=Path)
    args = parser.parse_args()
    print(json.dumps([summarize(path) for path in args.directories], indent=2))


if __name__ == "__main__":
    main()
