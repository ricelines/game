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
    "net": {"store", "record", "snap", "react", "perm", "space", "step"},
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


def cpp_tokens(source: str) -> list[tuple[str, int]]:
    """Keep C++ punctuation, excluding comments and string/character contents."""
    pattern = re.compile(
        r'//[^\n]*|/\*.*?\*/|R"([^\s()\\]*)\(.*?\)\1"|'
        r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|'
        r"[A-Za-z_]\w*|::|\.\.\.|[^\s]",
        re.DOTALL,
    )
    return [
        (match.group(), match.start())
        for match in pattern.finditer(source)
        if not match.group().startswith(("//", "/*", '"', "'", 'R"'))
    ]


def matching_token(tokens: list[tuple[str, int]], start: int) -> int:
    opening = tokens[start][0]
    closing = {"(": ")", "{": "}", "[": "]"}[opening]
    depth = 1
    for index in range(start + 1, len(tokens)):
        token = tokens[index][0]
        depth += (token == opening) - (token == closing)
        if depth == 0:
            return index
    return len(tokens) - 1


def header_parameter_counts(source: str) -> list[tuple[str, int, int]]:
    """Count declaration parameters, including nested template/function types."""
    tokens = cpp_tokens(source)
    found = []
    skip_until = -1
    excluded = {
        "if",
        "for",
        "while",
        "switch",
        "catch",
        "sizeof",
        "alignof",
        "decltype",
        "static_assert",
        "noexcept",
        "requires",
        "return",
    }
    for index, (token, _) in enumerate(tokens):
        if index <= skip_until or token != "(" or index == 0:
            continue
        name, offset = tokens[index - 1]
        if not re.fullmatch(r"[A-Za-z_]\w*", name) or name in excluded:
            continue
        end = matching_token(tokens, index)
        after = end + 1
        while after < len(tokens) and tokens[after][0] in {"const", "override", "final", "&"}:
            after += 1
        if after < len(tokens) and tokens[after][0] == "noexcept":
            after += 1
            if after < len(tokens) and tokens[after][0] == "(":
                after = matching_token(tokens, after) + 1
        if after >= len(tokens) or tokens[after][0] not in {";", "{", "=", ":"}:
            continue
        # Calls in default arguments and member initializers are not declarations.
        before = index - 2
        if before >= 0 and tokens[before][0] in {"=", ".", "return", ",", "("}:
            continue
        count = int(end > index + 1 and not (end == index + 2 and tokens[index + 1][0] == "void"))
        depths = {"(": 0, "[": 0, "{": 0, "<": 0}
        closes = {")": "(", "]": "[", "}": "{", ">": "<"}
        for part, _ in tokens[index + 1 : end]:
            if part in depths:
                depths[part] += 1
            elif part in closes:
                opening = closes[part]
                depths[opening] = max(0, depths[opening] - 1)
            elif part == "," and not any(depths.values()):
                count += 1
        found.append((name, count, source.count("\n", 0, offset) + 1))
        if tokens[after][0] == "{":
            skip_until = matching_token(tokens, after)
    return found


def mixed_state_structs(source: str) -> list[str]:
    """Reject direct fields holding states from multiple owners, including pointers."""
    tokens = cpp_tokens(source)
    found = []
    for index, (token, _) in enumerate(tokens):
        if token != "struct" or index + 2 >= len(tokens):
            continue
        name = tokens[index + 1][0]
        opening = index + 2
        while opening < len(tokens) and tokens[opening][0] not in {"{", ";"}:
            opening += 1
        if opening == len(tokens) or tokens[opening][0] != "{":
            continue
        end = matching_token(tokens, opening)
        owners = set()
        position = opening + 1
        statement = []
        while position < end:
            part = tokens[position][0]
            if part == "{":
                position = matching_token(tokens, position) + 1
                if "(" in statement or "struct" in statement:
                    statement = []
                continue
            if part == ";":
                # A method's arguments do not make its class an owner bundle.
                if "(" not in statement and "struct" not in statement:
                    for field in range(len(statement)):
                        if statement[field] == "ObjectState":
                            owners.add("store")
                        elif (
                            field >= 2
                            and statement[field] == "State"
                            and statement[field - 1] == "::"
                        ):
                            owners.add(statement[field - 2])
                statement = []
            else:
                statement.append(part)
            position += 1
        if len(owners) >= 2:
            found.append(name)
    return found


# S10 snapshot composition necessarily reads all owners. flush_gaps captures the same
# cross-owner snapshot; its exception was explicitly approved during Phase 2 completion.
SNAPSHOT_COMPOSITION = {"encode_checkpoint", "decode_checkpoint", "save_snapshot", "flush_gaps"}


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
        if path.suffix == ".hpp":
            for name, count, line in header_parameter_counts(source):
                if count > 10 and name not in SNAPSHOT_COMPOSITION:
                    errors.append(
                        f"{relative}:{line}: {name} declares {count} parameters (maximum 10)"
                    )
            for name in mixed_state_structs(source):
                errors.append(f"{relative}: {name} holds states from multiple owners")
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
