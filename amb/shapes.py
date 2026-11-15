"""Meter-scale solids with recipes that the trusted bake stage verifies."""

from __future__ import annotations

import math
import weakref
from dataclasses import dataclass

import build123d as bd


@dataclass(frozen=True)
class StandardShape:
    kind: str
    parameters: dict


_standard: dict[int, tuple[weakref.ReferenceType, int, StandardShape]] = {}
_stair_limits: tuple[float, float, float, float] | None = None


def _mark(shape: bd.Shape, kind: str, **parameters):
    identity = id(shape)
    reference = weakref.ref(shape, lambda _r: _standard.pop(identity, None))
    _standard[identity] = (reference, hash(shape.wrapped), StandardShape(kind, parameters))
    return shape


def standard_shape(shape: bd.Shape) -> StandardShape | None:
    """An untrusted recipe; the measurement stage compares its reconstructed solid."""
    entry = _standard.get(id(shape))
    return entry[2] if entry and entry[0]() is shape and entry[1] == hash(shape.wrapped) else None


def configure_stairs(config: dict):
    """The worker supplies mechanical limits before executing each authored program."""
    global _stair_limits
    _stair_limits = (
        config["STAIR_RISE_MIN_M"],
        config["STAIR_RISE_MAX_M"],
        config["STAIR_RUN_MIN_M"],
        config["STAIR_WIDTH_MIN_M"],
    )


def _positive(**values):
    for name, value in values.items():
        if not math.isfinite(value) or value <= 0:
            raise ValueError(f"{name} must be finite and positive")


def box(width: float, depth: float, height: float):
    _positive(width=width, depth=depth, height=height)
    return _mark(
        bd.Box(width, depth, height, align=(bd.Align.CENTER, bd.Align.CENTER, bd.Align.MIN)),
        "box",
        width=width,
        depth=depth,
        height=height,
    )


def slab(width: float, depth: float, thickness: float):
    return box(width, depth, thickness)


def cylinder(radius: float, height: float):
    _positive(radius=radius, height=height)
    return _mark(
        bd.Cylinder(radius, height, align=(bd.Align.CENTER, bd.Align.CENTER, bd.Align.MIN)),
        "cylinder",
        radius=radius,
        height=height,
    )


def sphere(radius: float):
    _positive(radius=radius)
    return _mark(bd.Sphere(radius).translate((0, 0, radius)), "sphere", radius=radius)


def hollow_box(width: float, depth: float, height: float, wall: float, floor: float):
    _positive(width=width, depth=depth, height=height, wall=wall, floor=floor)
    if 2 * wall >= min(width, depth) or floor >= height:
        raise ValueError("Wall/floor leave no interior")
    outer = box(width, depth, height)
    cavity = box(width - 2 * wall, depth - 2 * wall, height).translate((0, 0, floor))
    return _mark(
        outer - cavity,
        "hollow_box",
        width=width,
        depth=depth,
        height=height,
        wall=wall,
        floor=floor,
    )


def hollow_cylinder(radius: float, height: float, wall: float, floor: float):
    _positive(radius=radius, height=height, wall=wall, floor=floor)
    if wall >= radius or floor >= height:
        raise ValueError("Wall/floor leave no interior")
    outer = cylinder(radius, height)
    cavity = cylinder(radius - wall, height).translate((0, 0, floor))
    return _mark(
        outer - cavity, "hollow_cylinder", radius=radius, height=height, wall=wall, floor=floor
    )


def stair(width: float, rise: float, run: float, steps: int):
    _positive(width=width, rise=rise, run=run)
    if (
        isinstance(steps, bool)
        or not isinstance(steps, int | float)
        or not math.isfinite(steps)
        or steps < 1
        or int(steps) != steps
    ):
        raise ValueError("steps must be a positive integer")
    steps = int(steps)
    if _stair_limits is not None:
        low, high, minimum_run, minimum_width = _stair_limits
        if not low <= rise <= high or run < minimum_run or width < minimum_width:
            raise ValueError("Stair rise/run/width fail configured clearance limits")
    depth = run * steps
    solids = [
        box(width, run, rise * (i + 1)).translate((0, -depth / 2 + run * (i + 0.5), 0))
        for i in range(steps)
    ]
    shape = solids[0].fuse(*solids[1:]) if steps > 1 else solids[0]
    return _mark(shape, "stair", width=width, rise=rise, run=run, steps=steps)
