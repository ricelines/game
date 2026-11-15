"""CAD/measurement pipeline, called only in an isolated job or labelled trusted test."""

from __future__ import annotations

import ctypes
import hashlib
import json
import math
import re
import time
from pathlib import Path

import build123d as bd
from OCP.BRep import BRep_Tool
from OCP.TopAbs import TopAbs_EDGE, TopAbs_FACE, TopAbs_SOLID, TopAbs_VERTEX
from OCP.TopExp import TopExp_Explorer

from amb.shapes import configure_stairs, provenance
from bake.gltf import write_glb
from bake.parameters import asset_hash, resolve_declaration


class BakeError(ValueError):
    def __init__(self, stage: str, message: str):
        self.stage = stage
        super().__init__(message)


def _json_data(value):
    return json.loads(json.dumps(value, allow_nan=False))


def _polygon(wire):
    points = []
    for edge in wire.order_edges():
        segments = 1 if edge.geom_type == bd.GeomType.LINE else 32
        points.extend(list(edge.position_at(i / segments)) for i in range(segments))
    return points


def _measure(shape: bd.Shape, config: dict) -> dict:
    box = shape.bounding_box()
    low, high = list(box.min), list(box.max)
    size = [high[i] - low[i] for i in range(3)]
    if max(size) > config["MODEL_MAX_M"]:
        raise BakeError("geometry", "Model exceeds configured bounds")
    solids = shape.solids()
    if (
        not solids
        or not shape.is_valid
        or any(
            TopExp_Explorer(shape.wrapped, kind, TopAbs_SOLID).More()
            for kind in (TopAbs_FACE, TopAbs_EDGE, TopAbs_VERTEX)
        )
        or any(
            not BRep_Tool.IsClosed_s(shell.wrapped) for solid in solids for shell in solid.shells()
        )
    ):
        raise BakeError("geometry", "build must return valid closed solids")
    volume = sum(solid.volume for solid in solids)
    if not math.isfinite(volume) or volume <= 0:
        raise BakeError("geometry", "Model has no positive solid volume")
    supports = []
    minimum_normal = math.cos(math.radians(config["SUPPORT_NORMAL_MAX_DEG"]))
    for face in shape.faces():
        if face.geom_type != bd.GeomType.PLANE or face.area < config["SUPPORT_AREA_MIN_M2"]:
            continue
        if face.normal_at().Z < minimum_normal:
            continue
        polygon = _polygon(face.outer_wire())
        supports.append(
            {
                "height": face.center().Z,
                "area": face.area,
                "polygon": polygon,
                "holes": [_polygon(wire) for wire in face.inner_wires()],
            }
        )
    return {
        "bounds": {"min": low, "max": high},
        "size": size,
        "volume": volume,
        "supports": supports,
    }


def _native(library, mesh: dict, output: Path, config: dict):
    source = output / "input-mesh.json"
    source.write_text(json.dumps(mesh, separators=(",", ":"), allow_nan=False))
    error = ctypes.create_string_buffer(1024)
    result = library.amb_bake_compile(
        str(source).encode(), str(output).encode(), config["CONVEX_POINTS_MAX"], error, len(error)
    )
    source.unlink()
    if result:
        raise BakeError("native", error.value.decode(errors="replace"))
    measured = json.loads((output / "native-measured.json").read_text())
    lods = json.loads((output / "lod-indices.json").read_text())
    (output / "native-measured.json").unlink()
    (output / "lod-indices.json").unlink()
    return measured, lods


def load_native(path: Path):
    library = ctypes.CDLL(str(path))
    library.amb_bake_compile.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_uint,
        ctypes.c_char_p,
        ctypes.c_size_t,
    ]
    library.amb_bake_compile.restype = ctypes.c_int
    return library


def _geometry(shape, output, metadata, config, library, exact=None):
    output.mkdir(parents=True, exist_ok=True)
    measured = _measure(shape, config)
    started = time.perf_counter()
    tolerance = max(
        config["LINEAR_TOLERANCE_M"], max(measured["size"]) * config["RELATIVE_TOLERANCE"]
    )
    vertices, triangles = shape.tessellate(tolerance)
    tessellated = time.perf_counter()
    if len(triangles) > config["TRIS_MAX"]:
        raise BakeError("tessellation", "Model exceeds configured triangle limit")
    points = [list(vertex) for vertex in vertices]
    indices = [index for triangle in triangles for index in triangle]
    native, lods = _native(
        library,
        {
            "vertices": points,
            "indices": indices,
            "capacity_grid": 0 if exact else config["CAPACITY_GRID"],
            "lod_ratios": config["LOD_RATIOS"],
            "lod_relative_error": config["LOD_RELATIVE_ERROR"],
        },
        output,
        config,
    )
    measured["capacity"] = exact.capacity if exact else native["capacity"]
    measured["capacity_method"] = "standard_exact" if exact else "voxel_flood"
    measured["geometry_timings"] = {"tessellation_s": tessellated - started, **native["timings"]}
    geometry_lods = [indices, lods["lod1"], lods["lod2"]]
    measured["lods"] = [
        {"file": f"lod{i}.glb", "triangles": write_glb(output / f"lod{i}.glb", points, values)}
        for i, values in enumerate(geometry_lods)
    ]
    measured["collision"] = {
        "static": "collision.bin",
        "dynamic": "convex.bin",
        "mesh": "mesh.bin",
        "jolt": "5.6.0",
    }
    measured.update(metadata)
    return measured


def bake(source: str, supplied: dict, output: Path, config: dict, library) -> dict:
    """Execute author source. Public callers must use BakePool's sandbox."""
    start = time.perf_counter()
    configure_stairs(config)
    namespace = {"__name__": "ambergris_model", "__file__": "source.py"}
    try:
        exec(compile(source, "source.py", "exec"), namespace)
        params, schema = resolve_declaration(namespace.get("PARAMS", {}), supplied)
        shape = namespace["build"](params)
    except Exception as error:
        raise BakeError("build", f"{type(error).__name__}: {error}") from error
    if not isinstance(shape, bd.Shape):
        raise BakeError("geometry", "build must return a build123d Shape")
    built = time.perf_counter()
    full_measurements = _measure(shape, config)
    exact = provenance(shape)
    identity = asset_hash(source, params, config["BAKE_VERSION"])
    metadata = {
        "version": 1,
        "hash": identity,
        "bake_version": config["BAKE_VERSION"],
        "coordinates": "z_up_meters",
        "params": params,
        "parameter_schema": schema,
        "default_material": namespace.get("MATERIAL", "base.generic"),
        "default_script": namespace.get("SCRIPT"),
        "sites": namespace.get("SITES", {}),
        "parts": {},
        "replicated": namespace.get("REPLICATED", []),
        "bindings": namespace.get("BINDINGS", {}),
    }
    metadata = _json_data(metadata)
    parts = namespace.get("PARTS", {})
    solids = list(shape.solids())
    selected = set()
    children = {}
    for name, part in parts.items():
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]*", name):
            raise BakeError("metadata", "Part names must be path-safe identifiers")
        index = part["solid"]
        if (
            isinstance(index, bool)
            or not isinstance(index, int)
            or index in selected
            or not 0 <= index < len(solids)
        ):
            raise BakeError("metadata", "Each part must select a unique existing solid")
        selected.add(index)
        origin = part["origin"]
        if len(origin) != 3 or not all(math.isfinite(value) for value in origin):
            raise BakeError("metadata", "Part origin must contain three finite coordinates")
        child_hash = hashlib.sha256(f"{identity}\0part\0{name}".encode()).hexdigest()
        local = solids[index].translate(tuple(-value for value in origin))
        child_meta = {
            **metadata,
            "hash": child_hash,
            "parent_asset": identity,
            "part_name": name,
            "solid_index": index,
            "pivot": origin,
            "parts": {},
            "sites": part.get("sites", {}),
            "default_script": part.get("script"),
            "default_material": part.get("material", metadata["default_material"]),
            "replicated": part.get("replicated", []),
            "bindings": part.get("bindings", {}),
        }
        child = _geometry(local, output / "parts" / name, child_meta, config, library)
        children[name] = child
        (output / "parts" / name / "manifest.json").write_text(
            json.dumps(child, indent=2, allow_nan=False)
        )
        (output / "parts" / name / "source.py").write_text(source)
        metadata["parts"][name] = {
            "asset": child_hash,
            "position": origin,
            "rotation": [0, 0, 0, 1],
        }
    remaining = [solid for index, solid in enumerate(solids) if index not in selected]
    if remaining:
        root = shape if not selected else bd.Compound(remaining)
        manifest = _geometry(
            root, output, metadata, config, library, exact if not selected else None
        )
    else:
        output.mkdir(parents=True, exist_ok=True)
        manifest = {
            **metadata,
            "bounds": full_measurements["bounds"],
            "size": full_measurements["size"],
            "volume": 0.0,
            "capacity": 0.0,
            "capacity_method": "empty",
            "supports": [],
            "collision": {"static": None, "dynamic": None, "mesh": None, "jolt": "5.6.0"},
            "lods": [
                {"file": f"lod{i}.glb", "triangles": write_glb(output / f"lod{i}.glb", [], [])}
                for i in range(3)
            ],
        }
    if selected:
        manifest["assembly"] = full_measurements
    manifest["timings"] = {
        "build_s": built - start,
        "geometry_s": time.perf_counter() - built,
        "total_s": time.perf_counter() - start,
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2, allow_nan=False))
    (output / "source.py").write_text(source)
    return {
        "manifest": manifest,
        "children": children,
        "hash": identity,
        "timings": manifest["timings"],
    }
