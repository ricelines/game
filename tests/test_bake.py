"""Real CAD/artifact tests; restricted-host sandbox mode is labelled explicitly."""

import concurrent.futures
import json
import math
import struct
import subprocess
import threading
import time
from pathlib import Path

import pytest

from bake.config import ROOT, load_config
from bake.parameters import asset_hash, resolve
from bake.pipeline import BakeError, bake, load_native
from bake.pool import BakePool
from bake.sandbox import SandboxUnavailable, cgroup_probe


@pytest.fixture(scope="module")
def native():
    return load_native(ROOT / ".build/bake/libamb_bake_geometry.so")


@pytest.fixture
def config():
    result = load_config()
    result["CAPACITY_GRID"] = 64
    return result


def build(source, directory, config, native):
    return bake(source, {}, directory, config, native)["manifest"]


@pytest.mark.parametrize(
    ("expression", "volume", "capacity"),
    [
        ("box(1,2,3)", 6, 0),
        ("slab(1,2,0.1)", 0.2, 0),
        ("cylinder(0.5,2)", math.pi * 0.5, 0),
        ("sphere(0.5)", math.pi / 6, 0),
        ("hollow_box(1,1,1,0.05,0.05)", 1 - 0.9 * 0.9 * 0.95, 0.9 * 0.9 * 0.95),
        (
            "hollow_cylinder(0.5,1,0.05,0.05)",
            math.pi * (0.25 - 0.45**2 * 0.95),
            math.pi * 0.45**2 * 0.95,
        ),
    ],
)
def test_standard_actual_geometry(expression, volume, capacity, tmp_path, config, native):
    source = f"from amb.shapes import *\nPARAMS={{}}\ndef build(p): return {expression}\n"
    manifest = build(source, tmp_path, config, native)
    assert manifest["volume"] == pytest.approx(volume)
    assert manifest["capacity"] == pytest.approx(capacity)
    assert manifest["capacity_method"] == "standard_exact"
    for lod in manifest["lods"]:
        result = subprocess.run(
            [ROOT / ".build/bake/amb_bake_native", "--validate-glb", tmp_path / lod["file"]],
            check=True,
            capture_output=True,
            text=True,
        )
        assert json.loads(result.stdout)["triangles"] == lod["triangles"]
    result = subprocess.run(
        [ROOT / ".build/bake/amb_bake_native", "--inspect", tmp_path / "convex.bin"],
        check=True,
        capture_output=True,
        text=True,
    )
    assert json.loads(result.stdout)["points"] <= config["CONVEX_POINTS_MAX"]


def test_arbitrary_brep_capacity_and_thin_wall(tmp_path, config, native):
    source = """
import build123d as bd
PARAMS = {}
def build(p):
    outer = bd.Box(1, 1, 1, align=(bd.Align.CENTER,bd.Align.CENTER,bd.Align.MIN))
    inner = bd.Box(.98,.98,1, align=(bd.Align.CENTER,bd.Align.CENTER,bd.Align.MIN))
    inner = inner.translate((0,0,.02))
    return outer - inner
"""
    manifest = build(source, tmp_path, config, native)
    assert manifest["capacity_method"] == "voxel_flood"
    exact = 0.98 * 0.98 * 0.98
    assert exact * 0.88 < manifest["capacity"] <= exact
    assert manifest["volume"] == pytest.approx(1 - exact)


def test_arbitrary_solid_has_no_capacity(tmp_path, config, native):
    source = "import build123d as bd\ndef build(p): return bd.Box(1,1,1)\n"
    manifest = build(source, tmp_path, config, native)
    assert manifest["capacity"] == 0


@pytest.mark.parametrize("grid", [64, 256])
@pytest.mark.parametrize(
    ("size", "offset"),
    [
        ((0.14, 0.12, 0.16), (0, 0, 0)),
        ((0.14, 0.12, 0.16), (2.21, -1.73, 4.09)),
        ((1.4, 1.2, 1.6), (-2.21, 1.73, -4.09)),
        ((0.0014, 0.0012, 0.0016), (0.023, -0.019, 0.071)),
    ],
)
def test_scaled_translated_closed_solids_have_no_capacity(
    tmp_path, config, native, grid, size, offset
):
    config["CAPACITY_GRID"] = grid
    dimensions = ",".join(map(str, size))
    source = (
        "from build123d import Align,Box\n"
        f"def build(p):return Box({dimensions},align=(Align.CENTER,Align.CENTER,Align.MIN))"
        f".translate({offset!r})\n"
    )
    manifest = build(source, tmp_path, config, native)
    assert manifest["volume"] == pytest.approx(math.prod(size))
    assert manifest["capacity_method"] == "voxel_flood"
    assert manifest["capacity"] == 0


@pytest.mark.parametrize("grid", [64, 256])
@pytest.mark.parametrize("offset", [(0, 0, 0), (2.21, -1.73, 4.09)])
def test_small_translated_open_basin_retains_liquid(tmp_path, config, native, grid, offset):
    config["CAPACITY_GRID"] = grid
    source = f"""
from build123d import Align,Box
def build(p):
    align=(Align.CENTER,Align.CENTER,Align.MIN)
    outer=Box(.14,.12,.16,align=align)
    inner=Box(.136,.116,.16,align=align).translate((0,0,.003))
    return (outer-inner).translate({offset!r})
"""
    manifest = build(source, tmp_path, config, native)
    exact = 0.136 * 0.116 * 0.157
    assert exact * (0.8 if grid == 64 else 0.95) < manifest["capacity"] <= exact


def test_z_up_measurement_y_up_glb_and_nav(tmp_path, config, native):
    source = "from amb.shapes import box\ndef build(p): return box(1,2,3)\n"
    manifest = build(source, tmp_path, config, native)
    assert manifest["bounds"] == {"min": [-0.5, -1, 0], "max": [0.5, 1, 3]}
    assert manifest["supports"][0]["area"] == pytest.approx(2)
    assert manifest["supports"][0]["height"] == pytest.approx(3)
    assert len(manifest["supports"][0]["polygon"]) == 4
    glb = (tmp_path / "lod0.glb").read_bytes()
    size = struct.unpack_from("<I", glb, 12)[0]
    document = json.loads(glb[20 : 20 + size])
    positions = document["accessors"][0]
    assert positions["min"] == [-0.5, 0, -1]
    assert positions["max"] == [0.5, 3, 1]
    nav = (tmp_path / "mesh.bin").read_bytes()
    assert nav[:8] == b"AMBMESH1"
    vertices, indices = struct.unpack_from("<II", nav, 8)
    assert len(nav) == 16 + vertices * 12 + indices * 4


def test_parts_have_no_duplicate_volume_or_geometry(tmp_path, config, native):
    source = """
import build123d as bd
from amb.shapes import box
PARTS = {'leaf': {'solid':1, 'origin':[1,0,0]}}
SCRIPT = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
REPLICATED = ['invented.state']
def build(p): return bd.Compound([box(1,1,1), box(.2,.5,1).translate((1,0,0))])
"""
    manifest = build(source, tmp_path, config, native)
    child = json.loads((tmp_path / "parts/leaf/manifest.json").read_text())
    assert manifest["volume"] == pytest.approx(1)
    assert child["volume"] == pytest.approx(0.1)
    assert manifest["assembly"]["volume"] == pytest.approx(manifest["volume"] + child["volume"])
    assert child["bounds"]["min"][0] == pytest.approx(-0.1)
    assert manifest["parts"]["leaf"]["asset"] == child["hash"]
    assert child["default_script"] is None
    assert child["replicated"] == []
    assert manifest["replicated"] == ["invented.state"]


def test_logical_root_has_only_child_geometry(tmp_path, config, native):
    source = """
from amb.shapes import box
PARTS = {'whole': {'solid':0, 'origin':[0,0,0]}}
def build(p):return box(1,1,1)
"""
    manifest = build(source, tmp_path, config, native)
    assert manifest["volume"] == 0
    assert manifest["assembly"]["volume"] == pytest.approx(1)
    assert manifest["collision"]["static"] is None
    assert all(lod["triangles"] == 0 for lod in manifest["lods"])
    subprocess.run(
        [ROOT / ".build/bake/amb_bake_native", "--validate-glb", tmp_path / "lod0.glb"],
        check=True,
        capture_output=True,
    )


def test_triangle_budget_and_closed_solid_validation(tmp_path, config, native):
    config["TRIS_MAX"] = 1
    with pytest.raises(BakeError, match="triangle"):
        build(
            "from amb.shapes import box\ndef build(p):return box(1,1,1)\n", tmp_path, config, native
        )
    with pytest.raises(BakeError, match="closed solids"):
        build(
            "import build123d as bd\ndef build(p):return bd.Rectangle(1,1)\n",
            tmp_path,
            config,
            native,
        )
    with pytest.raises(BakeError, match="closed solids"):
        build(
            "import build123d as bd\n"
            "def build(p):return bd.Compound([bd.Box(1,1,1),"
            "bd.Edge.make_line((0,0,0),(2,0,0))])\n",
            tmp_path,
            config,
            native,
        )


@pytest.mark.parametrize(
    "expression", ["stair(1,.5,.3,5)", "stair(.5,.16,.3,5)", "stair(1,.16,.1,5)", "box(300,1,1)"]
)
def test_geometry_validation(expression, tmp_path, config, native):
    with pytest.raises(BakeError, match="bounds|Stair"):
        build(
            f"from amb.shapes import *\ndef build(p): return {expression}\n",
            tmp_path,
            config,
            native,
        )


def test_parameter_identity():
    source = "PARAMS={'a':(1,.5,2),'b':(2,1,3)}\ndef build(p): pass\n"
    first, _ = resolve(source, {"b": 2, "a": 1})
    second, _ = resolve(source, {"a": 1, "b": 2})
    assert asset_hash(source, first, "v1") == asset_hash(source, second, "v1")
    assert asset_hash(source, first, "v1") != asset_hash(source, first, "v2")
    assert asset_hash(source, {"a": 1}, "v1") == asset_hash(source, {"a": 1.0}, "v1")
    with pytest.raises(ValueError, match="outside"):
        resolve(source, {"a": 3})
    with pytest.raises(ValueError, match="Unknown"):
        resolve(source, {"c": 1})


def test_computed_parameter_declaration_runs_in_job(tmp_path, config, native):
    source = (
        "from amb.shapes import box\nPARAMS={'width':(1/2,.1,1)}\n"
        "def build(p):return box(p['width'],1,1)\n"
    )
    manifest = build(source, tmp_path, config, native)
    assert manifest["volume"] == pytest.approx(0.5)


def wait(pool, job):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        result = pool.status(job)
        if result["status"] not in ("queued", "baking"):
            return result
        time.sleep(0.01)
    pytest.fail("Bake job did not complete")


@pytest.fixture(scope="module")
def restricted_pool(tmp_path_factory):
    """This exercises real restrictions but does not claim the unavailable cgroup gate."""
    config = load_config()
    config["CAPACITY_GRID"] = 64
    config["BAKE_S"] = 3
    with BakePool(
        tmp_path_factory.mktemp("bake_assets"), config=config, require_cgroup=False
    ) as pool:
        yield pool


def test_strict_mode_requires_delegation(tmp_path):
    with pytest.raises(SandboxUnavailable, match="delegated"):
        BakePool(tmp_path)
    probe = cgroup_probe(Path("/sys/fs/cgroup"))
    if not probe["available"]:
        assert "errno=" in probe["error"]


def test_concurrent_cgroup_probes_own_distinct_directories(tmp_path, monkeypatch):
    # Real temporary directory lifetimes overlap. Only the virtual kernel
    # control-file write is substituted; this is not a delegation gate.
    barrier = threading.Barrier(2)
    parents = []
    write_text = Path.write_text

    def write_control(path, data, *args, **kwargs):
        if path.name != "memory.max":
            return write_text(path, data, *args, **kwargs)
        assert path.parent.is_dir()
        parents.append(path.parent)
        barrier.wait(timeout=5)
        return len(data)

    monkeypatch.setattr(Path, "write_text", write_control)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        futures = [executor.submit(cgroup_probe, tmp_path) for _ in range(2)]
        results = [future.result() for future in futures]
    assert results == [{"available": True}, {"available": True}]
    assert len(set(parents)) == 2
    assert list(tmp_path.iterdir()) == []


def test_warm_sandbox_real_bake_and_cache(restricted_pool):
    source = "from amb.shapes import box\ndef build(p): return box(1,2,3)\n"
    first = wait(restricted_pool, restricted_pool.submit(source))
    assert first["status"] == "done", first
    assert first["measured"]["volume"] == pytest.approx(6)
    assert first["sandbox"]["landlock_abi"] >= 3
    assert first["sandbox"]["restricted_host"]
    second = wait(restricted_pool, restricted_pool.submit(source))
    assert second["hash"] == first["hash"]
    assert second["cache_hit"]


@pytest.mark.parametrize(
    "operation",
    [
        "import socket; socket.socket()",
        "open('/etc/passwd').read()",
        "open('/app/bake/config.py','w').write('bad')",
        "import os; os.fork()",
    ],
)
def test_sandbox_denies_foreign_access_and_processes(restricted_pool, operation):
    source = operation + "\nfrom amb.shapes import box\ndef build(p):return box(1,1,1)\n"
    result = wait(restricted_pool, restricted_pool.submit(source))
    assert result["status"] == "failed", result


def test_job_deadline_and_worker_survives(restricted_pool):
    result = wait(restricted_pool, restricted_pool.submit("while True: pass\n"))
    assert result["status"] == "failed"
    assert result["errors"][0]["stage"] in ("timeout", "job")
    success = wait(
        restricted_pool,
        restricted_pool.submit("from amb.shapes import sphere\ndef build(p):return sphere(.2)\n"),
    )
    assert success["status"] == "done", success


def test_landlock_denies_other_job_data(restricted_pool):
    (restricted_pool.jobs_root / "foreign.txt").write_text("public test marker")
    source = (
        "open('/jobs/foreign.txt').read()\nfrom amb.shapes import box\n"
        "def build(p):return box(1,1,1)\n"
    )
    result = wait(restricted_pool, restricted_pool.submit(source))
    assert result["status"] == "failed"
    assert "PermissionError" in result["errors"][0]["message"]


def test_job_memory_limit(restricted_pool):
    source = (
        "bytearray(3*1024*1024*1024)\nfrom amb.shapes import box\ndef build(p):return box(1,1,1)\n"
    )
    result = wait(restricted_pool, restricted_pool.submit(source))
    assert result["status"] == "failed"
    assert "MemoryError" in result["errors"][0]["message"]


def test_supervisor_rejects_symlink_before_read(restricted_pool, tmp_path):
    outside = tmp_path / "foreign.json"
    outside.write_text('{"marker": "must not be read"}')
    directory = tmp_path / "asset"
    directory.mkdir()
    (directory / "manifest.json").symlink_to(outside)
    with pytest.raises(ValueError, match="escape|symlink"):
        restricted_pool._validate(directory, "irrelevant")
