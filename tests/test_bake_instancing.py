"""Real geometry compilation reuse preserves artifacts and each part's CAD facts."""

import json
import shutil
import tomllib
from pathlib import Path

import pytest
from build123d import Align, Axis, Box, Cylinder

from bake.pipeline import _geometry, _mesh, load_native
from bake.pool import BakePool

ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = ("collision.bin", "convex.bin", "mesh.bin", "lod0.glb", "lod1.glb", "lod2.glb")


@pytest.mark.parametrize("curved", [False, True])
def test_direct_occt_mesh_matches_build123d_points_winding_and_indices(curved):
    shape = Cylinder(0.4, 0.8) if curved else Box(0.6, 0.5, 0.4)
    shape = shape.rotate(Axis.X, 23).translate((0.3, -0.7, 1.1))
    reference_points, triangles = shape.tessellate(0.002)
    points, indices = _mesh(shape, 0.002)
    assert points == [list(point) for point in reference_points]
    assert indices == [index for triangle in triangles for index in triangle]


def test_repeated_part_compile_matches_uncached_bytes_and_keeps_fresh_facts(tmp_path):
    config = tomllib.loads((ROOT / "config.toml").read_text())["bake"]
    native = load_native(ROOT / ".build/bake/libamb_bake_geometry.so")
    shape = Box(0.14, 0.12, 0.16, align=(Align.CENTER, Align.CENTER, Align.MIN))
    repeated = shape.translate((2, 0, 0)).translate((-2, 0, 0))
    compiled = {}
    first = _geometry(shape, tmp_path / "first", {"hash": "first"}, config, native, compiled)
    metadata = {"hash": "second", "default_material": "different.material", "part_name": "second"}
    reused = _geometry(repeated, tmp_path / "reused", metadata, config, native, compiled)
    assert len(compiled) == 1
    fresh = _geometry(repeated, tmp_path / "fresh", metadata, config, native, {})
    assert reused == fresh
    assert reused["hash"] == "second" and first["hash"] == "first"
    assert reused["geometry_hash"] == first["geometry_hash"]
    assert reused["volume"] == pytest.approx(0.14 * 0.12 * 0.16)
    for filename in ARTIFACTS:
        assert (tmp_path / "reused" / filename).read_bytes() == (
            tmp_path / "fresh" / filename
        ).read_bytes()

    different = Box(0.15, 0.12, 0.16, align=(Align.CENTER, Align.CENTER, Align.MIN))
    changed = _geometry(different, tmp_path / "changed", {}, config, native, compiled)
    assert len(compiled) == 2 and changed["volume"] != reused["volume"]
    assert changed["geometry_hash"] != reused["geometry_hash"]
    assert (tmp_path / "changed/mesh.bin").read_bytes() != (
        tmp_path / "reused/mesh.bin"
    ).read_bytes()
    # The native request changes when trusted exact capacity replaces its grid.
    exact = _geometry(
        shape, tmp_path / "exact", {}, config, native, compiled, {"capacity": 0, "cavity": None}
    )
    assert len(compiled) == 3 and exact["capacity_method"] == "standard_exact"


def test_part_local_extraction_preserves_cad_and_compiled_bytes(tmp_path):
    from build123d import Axis, Compound, Location, Shape

    config = tomllib.loads((ROOT / "config.toml").read_text())["bake"]
    native = load_native(ROOT / ".build/bake/libamb_bake_geometry.so")
    offset = (1.5, -2, 0.35)
    assembly = Compound(children=[Box(0.14, 0.12, 0.16).rotate(Axis.Z, 31).translate(offset)])
    part = list(assembly.solids())[0]
    inverse = tuple(-value for value in offset)
    copied = part.translate(inverse)
    local = Shape.cast(part.wrapped.Moved(Location(inverse).wrapped))
    reference = _geometry(copied, tmp_path / "copied", {}, config, native, {})
    current = _geometry(local, tmp_path / "local", {}, config, native, {})
    assert current == reference
    for filename in ARTIFACTS:
        assert (tmp_path / "copied" / filename).read_bytes() == (
            tmp_path / "local" / filename
        ).read_bytes()


def test_publication_reuses_actual_byte_validation_but_checks_each_manifest(tmp_path, monkeypatch):
    import bake.pool as supervisor

    config = tomllib.loads((ROOT / "config.toml").read_text())["bake"]
    native = load_native(ROOT / ".build/bake/libamb_bake_geometry.so")
    with BakePool(tmp_path / "assets", require_cgroup=False) as pool:
        first = pool.jobs_root / "first"
        measured = _geometry(Box(0.14, 0.12, 0.16), first, {"hash": "first"}, config, native, {})
        (first / "manifest.json").write_text(json.dumps(measured))
        second = pool.jobs_root / "second"
        shutil.copytree(first, second)
        measured["hash"] = "second"
        (second / "manifest.json").write_text(json.dumps(measured))
        original = supervisor.subprocess.run
        calls = []

        def run(*args, **kwargs):
            calls.append(args[0])
            return original(*args, **kwargs)

        monkeypatch.setattr(supervisor.subprocess, "run", run)
        validated = {}
        pool._validate(first, "first", validated)
        count = len(calls)
        assert count > 0
        pool._validate(second, "second", validated)
        assert len(calls) == count
        measured["lods"][0]["triangles"] += 1
        (second / "manifest.json").write_text(json.dumps(measured))
        with pytest.raises(ValueError, match="does not match"):
            pool._validate(second, "second", validated)
        assert len(calls) == count
