#!/usr/bin/env python3
"""Check native dependency directions and the restart's prohibited shortcuts.

Behavioral extension tests and interface review complement these checks. This
script does not claim that source matching proves an architectural boundary.
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SERVER = ROOT / "server"
ALLOWED = {
    "store": set(),
    "record": {"store", "snap"},
    "step": {"store", "record", "snap"},
    "catalog": {"store", "react", "snap"},
    "lifecycle": {"store", "catalog", "snap"},
    "install": {"store", "react", "catalog", "snap"},
    "drive": {"store", "step", "react", "install", "command", "perm", "record", "snap"},
    "react": {"store", "record", "step", "snap"},
    "perm": {"store", "snap"},
    "space": {"store", "snap"},
    "command": {"store", "catalog", "react"},
    "control": {"store", "net", "snap"},
    "observe": {"store", "catalog", "react", "space", "perm"},
    "net": {"store"},
    "snap": {"store"},
    "elaborate": {"store", "record", "snap"},
    # Each adapter still needs review of the exact references it retains.
    "bindings": {
        "store",
        "record",
        "step",
        "catalog",
        "react",
        "perm",
        "space",
        "observe",
        "net",
        "snap",
        "elaborate",
        "control",
        "drive",
        "lifecycle",
        "install",
        "command",
    },
}

INCLUDE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]', re.MULTILINE)
AGGREGATE = re.compile(r"\b(?:class|struct)\s+(?:Core|World|Store|Services|RuntimeContext)\b")
WHOLE_OBJECTS = re.compile(r"\bObjects\s*(?:const\s*)?&")
AUTHORED_LITERAL = re.compile(
    r"""["'](?:aq\.[\w.]+|base\.(?:strike|push|pour|throw|sit|oxygen|fish_health))["']"""
)


def check() -> list[str]:
    errors = []
    files = sorted(SERVER.rglob("*.hpp")) + sorted(SERVER.rglob("*.cpp"))
    for path in files:
        relative = path.relative_to(SERVER)
        if len(relative.parts) == 1:
            # main.cpp composes modules; it is reviewed for absence of game rules.
            continue
        module = relative.parts[0]
        if module not in ALLOWED:
            errors.append(f"{relative}: module has no reviewed dependency contract")
            continue
        source = path.read_text()
        for match in INCLUDE.finditer(source):
            target = match[1].split("/")[0]
            execution = module == "drive" and match[1] == "bindings/execution/execution.hpp"
            if (
                target in ALLOWED
                and target != module
                and target not in ALLOWED[module]
                and not execution
            ):
                errors.append(f"{relative}: forbidden dependency on {target}")
            if module in {"store", "step", "react", "perm", "record", "snap"} and target in {
                "Jolt",
                "Recast.h",
                "DetourNavMesh.h",
                "DetourCrowd.h",
                "App.h",
            }:
                errors.append(f"{relative}: engine/network header in an independent module")
        if AGGREGATE.search(source):
            errors.append(f"{relative}: prohibited universal aggregate")
        if (
            module not in {"store", "lifecycle"}
            and path not in {SERVER / "snap" / "objects.cpp", SERVER / "snap" / "objects.hpp"}
            and WHOLE_OBJECTS.search(source)
        ):
            errors.append(
                f"{relative}: complete Objects access outside lifecycle/object serializer"
            )
        if AUTHORED_LITERAL.search(source):
            errors.append(f"{relative}: authored mechanic selected by a native literal")
    return errors


def main() -> int:
    errors = check()
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Native dependency and prohibited-access checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
