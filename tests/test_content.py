"""Checks over the authored package and real bake/validator artifacts."""

import json
import runpy
import subprocess
import sys
import tomllib

import pytest

from content.prepare import ROOT, prepare, registry, select, validate_symbols


@pytest.fixture(scope="module")
def authored():
    return json.loads((ROOT / "content/start.json").read_text())


def test_every_start_has_closed_references_and_private_connection_modes(authored):
    for mode in ("seeded", "forage"):
        for stakes in (True, False):
            plan = select(authored, mode, stakes)
            validate_symbols(plan)
            assert len(plan["profiles"]) == 8
            assert next(p for p in plan["profiles"] if p["id"] == 8)["modes"] == [
                "watch",
                "debug",
            ]
            assert {s["key"] for s in plan["scripts"]} & {"stakes", "stakes_free"} == {
                "stakes" if stakes else "stakes_free"
            }
            assert all("key" not in p and "key_file" in p for p in plan["profiles"])
            assert not any(o["model"] == "fish" for o in plan["objects"])
            assert not any(row["key"] == "fish" for row in plan["scripts"])
            assert sum(o["model"] == "school" for o in plan["objects"]) == (mode == "seeded")
            stock = [s for s in plan["script_state"] if s["script"] == "stakes"]
            assert len(stock) == (6 if mode == "seeded" and stakes else 0)


def test_registry_preserves_sources_dimensions_and_shared_pairs():
    data = registry(ROOT / "content/registry")
    terms = {t["name"]: t for t in data["terms"]}
    by_id = {t["id"]: t for t in data["terms"]}
    assert len(terms) == len(data["terms"])
    assert terms["base.mass"]["source"] == "measured"
    assert terms["base.liquid"]["conserved"]
    assert terms["base.liquid"]["combine"] == "none"
    assert terms["base.charge"]["dimensions"] == [2, 1, -2, 0, 0, 0, 0, 0]
    demand = terms["base.power_demand"]
    supply = by_id[demand["paired"]]
    assert supply["name"] == "base.power_supply" and supply["paired"] == demand["id"]
    assert by_id[data["mechanical"]["physics"]["transmission"]]["source"] == "intrinsic"
    for row in data["materials"]:
        values = {v["term"]: v["value"]["value"] for v in row["values"]}
        assert values[terms["base.density"]["id"]] > 0
        assert values[terms["base.embodied_energy"]["id"]] > 0


def test_school_uses_declared_parts_with_one_script(authored):
    plan = select(authored, "seeded", True)
    model = next(row for row in plan["models"] if row["key"] == "school")
    assert model["file"] == "models/school.py" and model["script"] == "school"
    assert not any(
        row.get("holder") == {"script": "school", "on": "cafe.school"} for row in plan["grants"]
    )
    assert not any(row["term"] == "aq.school" for row in plan["relations"])
    assert all(g["expires_s"] > 0 for g in plan["grants"])


def test_leases_and_starting_body_choices_have_real_scopes(authored):
    for stakes in (True, False):
        plan = select(authored, "seeded", stakes)
        assert all(plot["owner"] == 1 for plot in plan["plots"])
        rent = {"script": "rent", "on": "public.rent"}
        leases = [row for row in plan["grants"] if row.get("caller") == rent]
        assert len(leases) == 18
        for row in leases:
            roots = [
                root
                for root in plan["grants"]
                if root["holder"] == rent and root["scope"] == row["scope"]
            ]
            assert len(roots) == 1 and row["expires_s"] <= roots[0]["expires_s"]
            assert set(row["rights"]) <= set(roots[0]["rights"])
        assert sum(isinstance(row["holder"], str) for row in leases) == 6
        assert not any("avatar" in row["scope"] for row in plan["grants"])
        for profile in plan["profiles"]:
            if "body" not in profile:
                continue
            choices = [choice for group in profile["body_installs"] for choice in group]
            for plot in plan["plots"]:
                owner_scope = {"owned_on_plot": {"owner": profile["id"], "plot": plot["id"]}}
                assert {row["slot"] for row in choices if row["scope"] == owner_scope} == {
                    "use",
                    "take",
                    "move",
                }
            avatar = [row for row in choices if row["scope"] == {"avatar": profile["body"]}]
            assert {row["slot"] for row in avatar} == {"animate", "camera"}
            assert all(row["rights"] == [row["slot"]] and row["expires_s"] > 0 for row in choices)


def test_institution_account_has_an_explicit_script_allowance(authored):
    plan = select(authored, "seeded", True)
    assert all(row["holder"].startswith("p:") for row in plan["balances"])
    treasury_spend = [
        row
        for row in plan["grants"]
        if isinstance(row["holder"], dict)
        and row["holder"]["script"] == "treasury"
        and row["rights"] == ["spend"]
    ]
    assert len(treasury_spend) == 1
    assert treasury_spend[0]["scope"] == {"account": "public.treasury"}
    assert treasury_spend[0]["cap"] > 0 and treasury_spend[0]["expires_s"] > 0
    plan["balances"].append({"holder": "public.treasury", "amount": 100})
    validate_symbols(plan)
    plan["groups"] = [{"id": 1, "controller": 1, "name": "Test group"}]
    plan["balances"].append({"holder": "group:1", "amount": 25})
    validate_symbols(plan)
    plan["balances"].append({"holder": "group:999", "amount": 25})
    with pytest.raises(ValueError, match="Unknown group"):
        validate_symbols(plan)


@pytest.fixture(scope="module")
def baked_plan():
    # These are actual sandboxed CAD completions; an existing asset only skips
    # author execution after its GLB/Jolt/source/hash validation passes again.
    return prepare(
        ROOT / "content/start.json",
        ROOT / "assets",
        mode="seeded",
        stakes=True,
        restricted=True,
    )


def test_preparation_cli_probes_configured_cgroup_before_any_bake(tmp_path):
    output = tmp_path / "plan.json"
    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "content.prepare",
            "--cgroup-root",
            str(tmp_path / "not-delegated"),
            "--workers",
            "1",
            "--output",
            str(output),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode != 0
    assert "errno=2" in result.stderr
    assert "No delegated cgroup directory configured" not in result.stderr
    assert not output.exists()


def test_actual_plan_artifacts_and_measured_water(baked_plan):
    assert baked_plan["sandbox"]["restricted_host"] is True
    models = {m["key"]: m for m in baked_plan["models"]}
    objects = {o["key"]: o for o in baked_plan["objects"]}
    for row in models.values():
        directory = ROOT / "assets" / row["asset"]
        manifest = json.loads((directory / "manifest.json").read_text())
        assert manifest["hash"] == row["asset"]
        assert (directory / "lod0.glb").read_bytes()[:4] == b"glTF"
        assert (directory / "collision.bin").stat().st_size > 0
        assert row["canonical_params"] == json.dumps(
            manifest["params"], sort_keys=True, separators=(",", ":")
        )
    tank = json.loads((ROOT / "assets" / models["tank"]["asset"] / "manifest.json").read_text())
    assert tank["capacity"] == pytest.approx(3.96 * 1.96 * 1.97)
    assert objects["cafe.tank"]["state"]["base.liquid"] == pytest.approx(tank["capacity"] * 0.85)
    assert tank["bindings"]["container"]["shape"]["size"] == [3.96, 1.96, 1.97]


def test_actual_stair_deck_and_moving_part_ownership(baked_plan):
    models = {m["key"]: m for m in baked_plan["models"]}

    def manifest(model):
        return json.loads((ROOT / "assets" / models[model]["asset"] / "manifest.json").read_text())

    stair, deck, door = manifest("stair"), manifest("mezzanine"), manifest("door")
    assert stair["bounds"]["max"][2] == pytest.approx(3.2)
    assert deck["bounds"]["max"][2] + 2.98 == pytest.approx(3.2)
    assert door["parts"].keys() == {"leaf"}
    child = json.loads(
        (ROOT / "assets" / door["parts"]["leaf"]["asset"] / "manifest.json").read_text()
    )
    assert child["bindings"]["motion"]["mode"] == "kinematic"
    assert door["parts"]["leaf"]["position"] == [0, -0.25, 0]
    assert door["volume"] + child["volume"] == pytest.approx(door["assembly"]["volume"])
    assert manifest("body")["bindings"]["character"]["mass"] == "base.mass"
    school = manifest("school")
    assert len(school["parts"]) == 300
    assert set(school["parts"]) == {f"fish_{index:03}" for index in range(300)}
    for part in school["parts"].values():
        child = json.loads((ROOT / "assets" / part["asset"] / "manifest.json").read_text())
        assert child["volume"] > 0 and child["default_script"] is None
        assert child["replicated"] == ["aq.health", "aq.alive"]
        assert child["bindings"]["motion"]["mode"] == "kinematic"
        assert child["bindings"]["physics"]["collision_solid"] is False


def test_door_entire_authored_sweep_clears_actual_curved_shell():
    from build123d import Align, Box

    dome = runpy.run_path(str(ROOT / "content/models/dome.py"))
    door = runpy.run_path(str(ROOT / "content/models/door.py"))
    shell = dome["build"]({name: values[0] for name, values in dome["PARAMS"].items()})
    shell = shell.translate((0, 10, 0))
    leaf = list(door["build"]({}).solids())[1]
    size = leaf.bounding_box().size
    travel = door["BINDINGS"]["motion"]["travel"]
    # For this rectangular leaf, this box is the exact volume swept along X.
    sweep = Box(size.X + travel, size.Y, size.Z, align=(Align.CENTER, Align.CENTER, Align.MIN))
    pivot = door["PARTS"]["leaf"]["origin"]
    sweep = sweep.translate((travel / 2, -1.85 + pivot[1], 0))
    assert sum(solid.volume for solid in (shell & sweep).solids()) == pytest.approx(0, abs=1e-10)
    # The original centerline intersects the shell beyond the cut doorway.
    blocked = sweep.translate((0, -pivot[1], 0))
    assert sum(solid.volume for solid in (shell & blocked).solids()) > 0.001


def test_all_sources_pass_actual_luau_declaration_worker(tmp_path, baked_plan):
    worker = ROOT / ".build/release/server/react/amb_script_validate"
    assert worker.exists(), "Build the Release targets before the content gate"
    # Get reserved engine declarations from the real composition, rather than
    # maintaining a second copy of native signatures in this Python test.
    plan = json.loads(json.dumps(baked_plan))
    for profile in plan["profiles"]:
        profile["key_file"] = str(tmp_path / "keys" / f"{profile['id']}.key")
    plan_file = tmp_path / "plan.json"
    plan_file.write_text(json.dumps(plan))
    data_dir = tmp_path / "world"
    report = tmp_path / "report.json"
    native = subprocess.run(
        [
            str(ROOT / ".build/release/amb-server"),
            "--plan",
            str(plan_file),
            "--config",
            str(ROOT / "config.toml"),
            "--assets",
            str(ROOT / "assets"),
            "--data",
            str(data_dir),
            "--no-network",
            "--unpaced",
            "--steps",
            "4",
            "--report",
            str(report),
        ],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        check=False,
        timeout=30,
    )
    assert native.returncode == 0, (native.stdout, native.stderr)
    state = json.loads(report.read_text())
    assert state["script_errors"] == 0
    assert len(state["objects"]) == 361
    assert sum("aq.health" in row["values"] for row in state["objects"]) >= 300
    registries = list((data_dir / "registry").glob("*.json"))
    final = max(registries, key=lambda path: path.stat().st_mtime_ns)
    data = json.loads(final.read_text())
    pin = final.stem
    start = subprocess.run(
        [
            str(ROOT / ".build/release/tests/start_test"),
            str(plan_file),
            str(ROOT / "assets"),
            str(final),
        ],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        check=False,
        timeout=30,
    )
    assert start.returncode == 0, (start.stdout, start.stderr)
    assert {row["name"] for row in data["terms"]} >= {
        "engine.create",
        "engine.grasp",
        "engine.issue",
        "engine.grant",
    }
    config = tomllib.loads((ROOT / "config.toml").read_text())
    limits = config["scripts"]
    for source in sorted((ROOT / "content/scripts").glob("*.luau")):
        request = {
            "source": source.read_text(),
            "registry": data,
            "registry_pin": pin,
            "pins": {
                "luau": "0.747",
                "host_abi": "ambergris-host-1",
                "bake": config["bake"]["BAKE_VERSION"],
                "jolt": "5.6.0",
            },
            "limits": {
                "script_kb": limits["SCRIPT_KB"],
                "declaration_ms": limits["DECLARATION_MS"],
                "handler_ms": limits["HANDLER_MS"],
                "event_fields_max": limits["EVENT_FIELDS_MAX"],
            },
        }
        path = tmp_path / f"{source.stem}.json"
        path.write_text(json.dumps(request))
        result = subprocess.run(
            [str(worker), "--request", str(path), "--output", str(tmp_path / source.stem)],
            capture_output=True,
            text=True,
            check=False,
        )
        assert result.returncode == 0, (source.name, result.stdout, result.stderr)


def test_residents_supply_intentions_without_prepared_solutions():
    residents = json.loads((ROOT / "content/residents.json").read_text())["agents"]
    assert len(residents) == 6
    for row in residents:
        assert row["key_file"].endswith(".key") and "key" not in row
        assert row["persona"] and row["purpose"] and row["model"]
        assert "route" not in row and "source" not in row and "actions" not in row
