#!/usr/bin/env python3
"""Check the direct native owners, public headers and dependency direction.

These mechanical checks complement source review and behavioral tests. They do
not prove that a direct function has the right inputs or preserves game rules.
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
    "snap": {"store"},
    "react": {"store", "record", "snap"},
    "perm": {"store", "snap"},
    "space": {"store", "snap"},
    "net": {"store", "record", "snap", "react", "perm", "space"},
    "elaborate": {"store", "record", "snap"},
    "step": {"store", "record", "snap", "react", "perm", "space", "net", "elaborate"},
}
INCLUDE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]', re.MULTILINE)
LIBRARY = re.compile(r"\badd_library\(\s*(amber_\w+)\b")
UNIVERSAL = re.compile(r"\b(?:class|struct)\s+(?:Core|World|Store|Services|RuntimeContext)\b")
REMOVED_BOUNDARY = re.compile(
    r"\b(?:NativeOperations|InputOperations|CreditResolver|ResourceExchange|BodyEndpoints|"
    r"ObjectLifecycle|SceneProjection|RegistryPublication|StructuralOperations)\b"
)
AUTHORED_LITERAL = re.compile(
    r"""["'](?:aq\.[\w.]+|base\.(?:strike|push|pour|throw|sit|oxygen|fish_health))["']"""
)
REFERENCE_FIELD = re.compile(r"^\s*(?:const\s+)?[\w:<>, ]+&\s*\w+\s*;", re.MULTILINE)


def reference_structs(source: str) -> list[str]:
    """Find exported reference bundles, leaving ordinary opaque classes alone."""
    found = []
    for match in re.finditer(r"\bstruct\s+(\w+)\s*\{", source):
        start = match.end()
        depth = 1
        end = start
        while end < len(source) and depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        if REFERENCE_FIELD.search(source[start : end - 1]):
            found.append(match[1])
    return found


def check() -> list[str]:
    errors = []
    libraries = set()
    for path in SERVER.rglob("CMakeLists.txt"):
        libraries.update(LIBRARY.findall(path.read_text()))
        if path.parent.name not in ALLOWED:
            errors.append(f"{path.relative_to(ROOT)}: no extra adapter/module build target")
    expected = {f"amber_{owner}" for owner in ALLOWED}
    if libraries != expected:
        errors.append(
            "native libraries differ from the owner set: "
            f"missing={sorted(expected - libraries)}, extra={sorted(libraries - expected)}"
        )

    files = sorted(SERVER.rglob("*.hpp")) + sorted(SERVER.rglob("*.cpp"))
    for path in files:
        relative = path.relative_to(SERVER)
        owner = relative.parts[0] if len(relative.parts) > 1 else None
        if owner is not None and owner not in ALLOWED:
            errors.append(f"{relative}: source is outside the nine native owners")
            continue
        source = path.read_text()
        public = owner is not None and path.name == f"{owner}.hpp"
        if owner and path.suffix == ".hpp" and not public and not path.name.endswith("private.hpp"):
            errors.append(f"{relative}: merge public API into {owner}/{owner}.hpp")
        if public and owner != "store":
            for name in reference_structs(source):
                errors.append(f"{relative}: exported reference bundle {name}")
        for match in INCLUDE.finditer(source):
            included = match[1]
            parts = included.split("/")
            target = parts[0]
            if len(parts) > 1 and (SERVER / target).is_dir():
                if target not in ALLOWED:
                    errors.append(f"{relative}: include from removed owner {target}")
                elif target != owner:
                    if owner is not None and target not in ALLOWED[owner]:
                        errors.append(f"{relative}: forbidden dependency on {target}")
                    if included != f"{target}/{target}.hpp":
                        errors.append(
                            f"{relative}: imports another owner's private header {included}"
                        )
            if owner and target in {"Jolt", "Recast.h", "DetourNavMesh.h", "DetourCrowd.h"}:
                if owner != "space":
                    errors.append(f"{relative}: physics/navigation implementation outside Space")
            if owner and target == "App.h" and owner != "net":
                errors.append(f"{relative}: socket implementation outside Net")
        if UNIVERSAL.search(source):
            errors.append(f"{relative}: universal world/services aggregate")
        if owner == "space" and re.search(r"\b(?:react|perm|catalog|nlohmann)::|\bJson\b", source):
            errors.append(f"{relative}: semantic tables or JSON in numerical Space")
        if REMOVED_BOUNDARY.search(source):
            errors.append(f"{relative}: removed adapter boundary remains")
        if AUTHORED_LITERAL.search(source):
            errors.append(f"{relative}: authored mechanic selected by a native literal")
    return errors


def main() -> int:
    errors = check()
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Nine native owners, public headers and dependency directions checked.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
