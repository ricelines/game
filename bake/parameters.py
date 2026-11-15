"""Literal parameter declarations can be resolved without executing author code."""

import ast
import hashlib
import json
import math


def resolve(source: str, supplied: dict | None) -> tuple[dict, dict]:
    tree = ast.parse(source, filename="source.py")
    declaration = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id == "PARAMS" for target in node.targets
        ):
            declaration = ast.literal_eval(node.value)
    return resolve_declaration(declaration, supplied)


def resolve_declaration(declaration: dict, supplied: dict | None) -> tuple[dict, dict]:
    if not isinstance(declaration, dict):
        raise ValueError("PARAMS must be a dictionary")
    supplied = supplied or {}
    if not isinstance(supplied, dict):
        raise ValueError("params must be an object")
    if extra := supplied.keys() - declaration.keys():
        raise ValueError(f"Unknown parameters: {', '.join(sorted(extra))}")
    values = {}
    for name, limits in declaration.items():
        if not isinstance(name, str) or not isinstance(limits, tuple | list) or len(limits) != 3:
            raise ValueError("Parameter declarations are name: (default, minimum, maximum)")
        default, lower, upper = limits
        value = supplied.get(name, default)
        for number in (default, lower, upper, value):
            if (
                isinstance(number, bool)
                or not isinstance(number, int | float)
                or not math.isfinite(number)
            ):
                raise ValueError(f"{name} must contain finite numbers")
        if not lower <= default <= upper or not lower <= value <= upper:
            raise ValueError(f"Parameter {name} is outside [{lower}, {upper}]")
        values[name] = float(value)
    return values, declaration


def canonical_params(params: dict) -> str:
    normalized = {}
    for name, value in params.items():
        if (
            isinstance(value, bool)
            or not isinstance(value, int | float)
            or not math.isfinite(value)
        ):
            raise ValueError("Parameters must contain finite numbers")
        normalized[name] = float(value)
    return json.dumps(normalized, sort_keys=True, separators=(",", ":"), allow_nan=False)


def asset_hash(source: str, params: dict, version: str) -> str:
    encoded = canonical_params(params)
    return hashlib.sha256(
        b"\0".join((source.encode(), encoded.encode(), version.encode()))
    ).hexdigest()
