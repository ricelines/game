"""Measure the actual warm CAD pipeline; restrictions and missing gates stay explicit."""

import argparse
import json
import platform
import statistics
import time
import uuid
from pathlib import Path

from bake.config import ROOT, load_config
from bake.pool import BakePool


def await_job(pool, identity):
    while True:
        result = pool.status(identity)
        if result["status"] not in ("queued", "baking"):
            return result
        time.sleep(0.005)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--restricted-host",
        action="store_true",
        help="Explicitly run without delegated cgroups; not a pass of that gate",
    )
    parser.add_argument("--cgroup-root", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / ".cache/bake-benchmark.json")
    parser.add_argument("--repeats", type=int, default=5)
    args = parser.parse_args()
    config = load_config()
    cases = {
        "standard_box": (
            "from amb.shapes import box\nPARAMS={'width':(1,.5,2)}\n"
            "def build(p):return box(p['width'],1,1)\n"
        ),
        "standard_hollow": (
            "from amb.shapes import hollow_cylinder\nPARAMS={'radius':(.2,.1,.4)}\n"
            "def build(p):return hollow_cylinder(p['radius'],.5,.01,.02)\n"
        ),
        "arbitrary_capacity": """
import build123d as bd
PARAMS = {'width':(1,.5,2)}
def build(p):
    align = (bd.Align.CENTER,bd.Align.CENTER,bd.Align.MIN)
    outer = bd.Box(p['width'],1,1,align=align)
    inner = bd.Box(p['width']-.02,.98,1,align=align).translate((0,0,.02))
    return outer-inner
""",
    }
    started = time.perf_counter()
    with BakePool(
        ROOT / ".cache/benchmark-assets" / uuid.uuid4().hex,
        config=config,
        cgroup_root=args.cgroup_root,
        require_cgroup=not args.restricted_host,
    ) as pool:
        report = {
            "platform": platform.platform(),
            "python": platform.python_version(),
            "config": config,
            "sandbox": pool.sandbox,
            "startup_s": time.perf_counter() - started,
            "cases": {},
        }
        for name, source in cases.items():
            samples = []
            for iteration in range(args.repeats):
                parameter = "radius" if name == "standard_hollow" else "width"
                default = 0.2 if parameter == "radius" else 1
                values = {parameter: default + iteration * 0.001}
                began = time.perf_counter()
                result = await_job(pool, pool.submit(source, values))
                sample = {
                    "elapsed_s": time.perf_counter() - began,
                    "status": result["status"],
                    "cache_hit": result.get("cache_hit", False),
                    "timings": result.get("timings", {}),
                    "errors": result.get("errors", []),
                    "resources": result.get("resources", {}),
                }
                if result["status"] == "done":
                    manifest = result["measured"]
                    sample["geometry_timings"] = manifest.get("geometry_timings", {})
                    sample["lod_triangles"] = [lod["triangles"] for lod in manifest["lods"]]
                    sample["capacity"] = manifest["capacity"]
                    sample["asset_bytes"] = sum(
                        path.stat().st_size
                        for path in (pool.assets / result["hash"]).iterdir()
                        if path.is_file()
                    )
                samples.append(sample)
            durations = [sample["elapsed_s"] for sample in samples]
            ordered = sorted(durations)
            report["cases"][name] = {
                "count": len(samples),
                "p50_s": statistics.median(durations),
                "p95_s": ordered[min(len(ordered) - 1, int(0.95 * len(ordered)))],
                "p99_s": ordered[min(len(ordered) - 1, int(0.99 * len(ordered)))],
                "samples": samples,
            }
        began = time.perf_counter()
        cached = await_job(pool, pool.submit(cases["standard_box"], {"width": 1}))
        report["cache"] = {
            "elapsed_s": time.perf_counter() - began,
            "cache_hit": cached.get("cache_hit"),
            "status": cached["status"],
        }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps(
            {
                "report": str(args.output),
                "startup_s": report["startup_s"],
                "sandbox": report["sandbox"],
                "cases": {
                    name: {key: value for key, value in result.items() if key != "samples"}
                    for name, result in report["cases"].items()
                },
                "cache": report["cache"],
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
