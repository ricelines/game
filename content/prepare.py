"""Resolve an authored launch package and bake its actual immutable geometry.

This program produces data for native installation; it never operates a world.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import time
import tomllib
from copy import deepcopy
from pathlib import Path

from bake.parameters import canonical_params
from bake.pool import BakePool

ROOT = Path(__file__).resolve().parents[1]


def registry(directory: Path) -> dict:
    """Compile ordered authored names to the catalog's native registry DTO."""
    terms = json.loads((directory / "terms.json").read_text())
    names = {row["name"]: index for index, row in enumerate(terms, 1)}
    if len(names) != len(terms):
        raise ValueError("Duplicate authored registry term")
    for row in terms:
        row["id"] = names[row["name"]]
        row.setdefault("source", "state")
        row.setdefault("dimensions", [0] * 8)
        row.setdefault("conserved", False)
        row.setdefault("combine", "none")
        row.setdefault("owner", 1)
        row.setdefault("measurement", "baked")
        row.setdefault("compared", [])
        row.setdefault("distinction", "")
        for field in ("along", "paired", "priority"):
            row[field] = names[row[field]] if row.get(field) else 0
        for argument in row.setdefault("inputs", []):
            argument["term"] = names[argument["term"]] if argument.get("term") else 0
            argument.setdefault("optional", False)
    authored = json.loads((directory / "materials.json").read_text())
    materials = []
    for material, values in authored["values"].items():
        materials.append(
            {
                "id": names[material],
                "values": [
                    {
                        "term": names[term],
                        "value": {
                            "kind": "boolean" if isinstance(value, bool) else "number",
                            "value": value if isinstance(value, bool) else float(value),
                        },
                    }
                    for term, value in values.items()
                ],
            }
        )

    def resolve(value):
        if isinstance(value, dict):
            return {key: resolve(item) for key, item in value.items()}
        return names[value]

    mechanical = resolve(json.loads((directory / "mechanical.json").read_text()))
    return {
        "version": 1,
        "terms": terms,
        "materials": materials,
        "rules": [],
        "mechanical": mechanical,
    }


def select(document: dict, mode: str, stakes: bool) -> dict:
    """Select explicit authored records; membership never depends on agent purposes."""
    result = {}
    for name, value in document.items():
        if not isinstance(value, list):
            result[name] = deepcopy(value)
            continue
        result[name] = []
        for record in value:
            if mode not in record.get("starts", ["seeded", "forage"]):
                continue
            if "stakes" in record and record["stakes"] != stakes:
                continue
            result[name].append(
                {
                    key: deepcopy(item)
                    for key, item in record.items()
                    if key not in {"starts", "stakes"}
                }
            )
    result["land"]["lots"] = select_lots = []
    for lot in document["land"].get("lots", []):
        if mode in lot.get("starts", ["seeded", "forage"]):
            select_lots.append(
                {key: deepcopy(value) for key, value in lot.items() if key != "starts"}
            )
    return result


def validate_symbols(plan: dict) -> None:
    """Catch package mistakes before spending CAD time or writing a partial plan."""
    objects = {row["key"] for row in plan["objects"]}
    models = {row["key"] for row in plan["models"]}
    scripts = {row["key"] for row in plan["scripts"]}
    world_scripts = {row["script"] for row in plan.get("world_scripts", [])}
    players = {row["id"] for row in plan["profiles"]}
    groups = {row["id"] for row in plan["groups"]}

    def wallet(reference):
        if (
            not isinstance(reference, str)
            or not reference.startswith("p:")
            or not reference[2:].isdigit()
            or int(reference[2:]) not in players
        ):
            raise ValueError("Money belongs to a registered player's wallet")

    def account(reference):
        if reference == "p:0":
            return
        if reference in objects:
            return
        if isinstance(reference, str) and reference.startswith("group:"):
            if reference[6:].isdigit() and int(reference[6:]) in groups:
                return
            raise ValueError("Unknown group account")
        wallet(reference)

    for field, keys in (("objects", objects), ("models", models), ("scripts", scripts)):
        if len(keys) != len(plan[field]):
            raise ValueError(f"Duplicate {field} key")
    if len(world_scripts) != len(plan.get("world_scripts", [])):
        raise ValueError("Duplicate world script")
    if not world_scripts <= scripts:
        raise ValueError("Missing world script")
    for row in plan["objects"]:
        if row["owner"] not in players:
            raise ValueError("An object needs a registered non-root owner")
        if row.get("group", 0) and row["group"] not in groups:
            raise ValueError("Unknown object group")
        if row["model"] not in models:
            raise ValueError(f"Missing model {row['model']}")
    for row in plan["models"]:
        if row.get("group", 0) and row["group"] not in groups:
            raise ValueError("Unknown model group")
        if row.get("script") and row["script"] not in scripts:
            raise ValueError(f"Missing script {row['script']}")
    if plan["newcomer"]["body_model"] not in models:
        raise ValueError("Missing newcomer body model")
    for profile in plan["profiles"]:
        if "body" not in profile:
            continue
        if profile["body"] not in objects:
            raise ValueError(f"Missing starting body {profile['body']}")
        if "play" not in profile["modes"]:
            continue
        for choices in profile.get("body_installs", []):
            for choice in choices:
                for field in ("object", "avatar"):
                    endpoint = choice["scope"].get(field)
                    if endpoint and endpoint not in objects:
                        raise ValueError(f"Missing starting slot endpoint {endpoint}")
    for row in plan["relations"]:
        if row["from"] not in objects or row["to"] not in objects:
            raise ValueError(f"Missing relation endpoint: {row}")
    for row in plan["bootstrap"] + plan.get("script_state", []):
        if row["target"] == "world":
            if row.get("script") not in world_scripts:
                raise ValueError("Missing installed world script")
        elif row["target"] not in objects:
            raise ValueError(f"Missing target {row['target']}")

    def holder(reference):
        if isinstance(reference, str):
            wallet(reference)
        elif reference["on"] == "world":
            if reference["script"] not in world_scripts:
                raise ValueError(f"Missing installed world grant holder: {reference}")
        elif reference["script"] not in scripts or reference["on"] not in objects:
            raise ValueError(f"Missing installed grant holder: {reference}")

    if plan["land"]["way_group"] not in groups:
        raise ValueError("Unknown way group")
    for row in plan["land"].get("lots", []):
        if row["owner"] not in players:
            raise ValueError("Unknown lot owner")
    for row in plan["grants"]:
        if row["issuer"] not in players | {0}:
            raise ValueError("Unknown grant issuer")
        if "all_land" in row["scope"] and row["issuer"] != 0:
            raise ValueError("Only root gives all-land grants")
        holder(row["holder"])
        for field in ("object", "avatar"):
            endpoint = row["scope"].get(field)
            if endpoint and endpoint not in objects:
                raise ValueError(f"Missing grant endpoint {endpoint}")
        if "account" in row["scope"]:
            account(row["scope"]["account"])
    for row in plan["balances"]:
        if row["holder"] == "p:0":
            raise ValueError("Root never stores a balance")
        account(row["holder"])


def prepare(
    source: Path,
    assets: Path,
    *,
    mode: str,
    stakes: bool,
    restricted: bool,
    workers: int = 2,
    cgroup_root: Path | None = None,
    config: Path = ROOT / "config.toml",
) -> dict:
    original = source.read_bytes()
    plan = select(json.loads(original), mode, stakes)
    validate_symbols(plan)
    settings = tomllib.loads(config.read_text())
    for term, value in plan["world_settings"].items():
        if isinstance(value, dict) and "config" in value:
            section, key = value["config"].split(".")
            plan["world_settings"][term] = settings[section][key]
    plan["source"] = {
        "start_sha256": hashlib.sha256(original).hexdigest(),
        "mode": mode,
        "stakes": stakes,
    }
    plan["registry"] = registry(source.parent / "registry")
    term_ids = {row["name"]: row["id"] for row in plan["registry"]["terms"]}
    for row in plan.get("script_state", []):
        for entry in row["entries"]:
            if "key_term" in entry:
                entry["key"] = term_ids[entry.pop("key_term")]
    plan["world_settings"]["base.stakes"] = stakes
    for script in plan["scripts"]:
        script["source"] = (source.parent / script.pop("file")).read_text()
    tasks = list(enumerate(plan["models"]))
    chunks = [tasks[index::workers] for index in range(workers)]

    def bake_chunk(chunk):
        completed = []
        with BakePool(
            assets, config=settings["bake"], require_cgroup=not restricted, cgroup_root=cgroup_root
        ) as pool:
            for index, row in chunk:
                cad = (source.parent / row["file"]).read_text()
                started = time.perf_counter()
                identity = pool.submit(cad, row.get("params", {}))
                while (result := pool.status(identity))["status"] in {"queued", "baking"}:
                    time.sleep(0.01)
                if result["status"] != "done":
                    raise RuntimeError(f"Actual CAD bake failed for {row['key']}: {result}")
                completed.append((index, result, time.perf_counter() - started))
            return completed, pool.sandbox

    manifests = {}
    plan["asset_timings"] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
        futures = [executor.submit(bake_chunk, chunk) for chunk in chunks if chunk]
        for future in futures:
            completed, sandbox = future.result()
            plan["sandbox"] = sandbox
            for index, result, elapsed in completed:
                row = plan["models"][index]
                manifests[row["key"]] = result["measured"]
                row["asset"] = result["hash"]
                row["canonical_params"] = canonical_params(result["measured"]["params"])
                row.pop("file")
                row.pop("params", None)
                plan["asset_timings"].append(
                    {"model": row["key"], "seconds": elapsed, "cache_hit": result["cache_hit"]}
                )
    # Initialization is measured from the selected real variant, not a maker claim.
    for row in plan["objects"]:
        for term, value in row.get("state", {}).items():
            if isinstance(value, dict) and "asset_measure" in value:
                manifest = manifests[row["model"]]
                row["state"][term] = manifest[value["asset_measure"]] * value.get("scale", 1)
    plan["asset_timings"].sort(key=lambda row: row["model"])
    return plan


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--start", type=Path, default=ROOT / "content/start.json")
    parser.add_argument("--assets", type=Path, default=ROOT / "assets")
    parser.add_argument("--output", type=Path, default=ROOT / ".runtime/import.json")
    parser.add_argument("--mode", choices=("seeded", "forage"), default="seeded")
    parser.add_argument("--stakes", choices=("on", "off"), default="on")
    parser.add_argument("--workers", type=int, choices=range(1, 9), default=2)
    parser.add_argument("--restricted-host-bake", action="store_true")
    parser.add_argument("--cgroup-root", type=Path)
    parser.add_argument("--config", type=Path, default=ROOT / "config.toml")
    args = parser.parse_args()
    plan = prepare(
        args.start,
        args.assets,
        mode=args.mode,
        stakes=args.stakes == "on",
        restricted=args.restricted_host_bake,
        workers=args.workers,
        cgroup_root=args.cgroup_root,
        config=args.config,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(".pending")
    temporary.write_text(json.dumps(plan, indent=2, allow_nan=False) + "\n")
    temporary.replace(args.output)
    print(
        json.dumps(
            {
                "output": str(args.output),
                "objects": len(plan["objects"]),
                "models": len(plan["models"]),
                "sandbox": plan["sandbox"],
            }
        )
    )


if __name__ == "__main__":
    main()
