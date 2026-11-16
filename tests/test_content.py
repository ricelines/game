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


def test_root_owned_and_starting_body_grants_have_real_scopes(authored):
    for mode in ("seeded", "forage"):
        for stakes in (True, False):
            plan = select(authored, mode, stakes)
            validate_symbols(plan)
            assert plan["groups"] == [
                {"id": 1, "controller": 0, "name": "architects", "members": [1]}
            ]
            assert plan["land"] == {
                "way_group": 1,
                "lots": (
                    [
                        {"owner": owner, "at": at}
                        for owner, at in {
                            2: [8, 50],
                            3: [24, 50],
                            4: [40, 50],
                            5: [56, 50],
                            6: [-16, 50],
                            7: [-32, 50],
                        }.items()
                    ]
                    if mode == "seeded"
                    else []
                ),
            }
            assert not any(
                "caller" in row or "owned_on_plot" in row["scope"] for row in plan["grants"]
            )
            for profile in plan["profiles"]:
                if "body" not in profile:
                    continue
                choices = [choice for group in profile["body_installs"] for choice in group]
                owner_scope = {"owned": f"p:{profile['id']}"}
                assert {row["slot"] for row in choices if row["scope"] == owner_scope} == {
                    "use",
                    "take",
                    "move",
                }
                assert {
                    row["slot"] for row in choices if row["scope"] == {"avatar": profile["body"]}
                } == {"animate", "camera"}
                assert all(
                    row["rights"] == [row["slot"]] and row["expires_s"] > 0 for row in choices
                )
                build = [
                    r
                    for r in plan["grants"]
                    if isinstance(r["holder"], dict)
                    and r["holder"]["on"] == profile["body"]
                    and r["rights"] == ["build"]
                ]
                assert len(build) == (2 if mode == "seeded" else 0)
                if build:
                    own_plot = next(
                        row["at"] for row in plan["land"]["lots"] if row["owner"] == profile["id"]
                    )
                    assert all(
                        row["issuer"] == profile["id"] and row["scope"] == {"plot_at": own_plot}
                        for row in build
                    )
                owned = [
                    r
                    for r in plan["grants"]
                    if isinstance(r["holder"], dict)
                    and r["holder"]["on"] == profile["body"]
                    and r["rights"] == ["edit", "destroy"]
                ]
                assert len(owned) == 1 and owned[0]["scope"] == owner_scope


def test_power_buyer_has_an_explicit_root_allowance(authored):
    plan = select(authored, "seeded", True)
    buyer = {"script": "power_buyer", "on": "public.power_buyer"}
    grant = next(r for r in plan["grants"] if r["holder"] == buyer)
    assert grant == {
        "issuer": 0,
        "holder": buyer,
        "scope": {"account": "p:0"},
        "rights": ["spend"],
        "cap": 100000,
        "period_s": 86400,
        "expires_s": 31536000,
    }
    allowance = next(r for r in plan["grants"] if r["holder"] == "p:1")
    assert allowance["issuer"] == 0 and allowance["scope"] == {"account": "p:0"}
    assert allowance["cap"] == 1000000 and allowance["period_s"] == 604800
    plan["balances"].append({"holder": "public.power_buyer", "amount": 100})
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
    assert len(state["objects"]) == 360
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


def test_power_buyer_pays_suppliers_from_root_within_its_cap(tmp_path, baked_plan):
    plan = json.loads(json.dumps(baked_plan))
    for profile in plan["profiles"]:
        profile["key_file"] = str(tmp_path / "keys" / f"{profile['id']}.key")
    plan["world_settings"].update({"base.settlement_seconds": 0.1, "base.purchase_per_joule": 0.01})
    grant = next(
        r
        for r in plan["grants"]
        if isinstance(r["holder"], dict) and r["holder"]["script"] == "power_buyer"
    )
    grant["cap"] = 5
    source = next(r for r in plan["scripts"] if r["key"] == "power_buyer")
    source["source"] = (
        "local rule=(function()\n"
        + source["source"]
        + "\nend)()\n"
        + """
local settle=rule.on['base.settle']
rule.on['base.settle']=function(self,...)
    settle(self,...)
    if self:now()>=3 then
        local balance=self:act('engine.balance',nil,{account='p:3'}).balance
        assert(balance==1005,'supplier receives the capped root payment')
        assert(self:act('engine.balance',nil,{account='p:0'}).balance==0,'root holds no money')
        self:set('base.charge',5)
    end
end
return rule
"""
    )
    path, report = tmp_path / "plan.json", tmp_path / "report.json"
    path.write_text(json.dumps(plan))
    result = subprocess.run(
        [
            str(ROOT / ".build/release/amb-server"),
            "--plan",
            str(path),
            "--config",
            str(ROOT / "config.toml"),
            "--assets",
            str(ROOT / "assets"),
            "--data",
            str(tmp_path / "world"),
            "--no-network",
            "--unpaced",
            "--steps",
            "80",
            "--report",
            str(report),
        ],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    assert result.returncode == 0, (result.stdout, result.stderr)
    state = json.loads(report.read_text())
    assert state["script_errors"] == 0
    position = next(row["at"] for row in plan["objects"] if row["key"] == "public.power_buyer")
    buyer = next(row for row in state["objects"] if row["position"] == position)
    assert buyer["values"]["base.charge"] == 5


def test_body_pours_into_a_model_without_container_bindings(tmp_path, baked_plan):
    plan = json.loads(json.dumps(baked_plan))
    for profile in plan["profiles"]:
        profile["key_file"] = str(tmp_path / "keys" / f"{profile['id']}.key")
    cup_model = next(row for row in plan["models"] if row["key"] == "cup")
    receiver_model = {**cup_model, "key": "test.unbound_receiver"}
    plan["models"].append(receiver_model)
    manifest = json.loads((ROOT / "assets" / cup_model["asset"] / "manifest.json").read_text())
    assert not manifest["bindings"]
    body = next(row for row in plan["objects"] if row["key"] == "resident.mira")
    cup = next(row for row in plan["objects"] if row["model"] == "cup")
    for key, model, dx, quantity in (
        ("test.pour_source", "cup", 0.4, 0.0001),
        ("test.pour_receiver", receiver_model["key"], -0.4, 0),
    ):
        plan["objects"].append(
            {
                **cup,
                "key": key,
                "model": model,
                "owner": 2,
                "at": [body["at"][0] + dx, body["at"][1] + 0.5, 0],
                "flags": ["dynamic"],
                "state": {"base.liquid": quantity},
            }
        )
    source = (ROOT / "content/scripts/body.luau").read_text()
    source = (
        "local body=(function()\n"
        + source
        + "\nend)()\n"
        + """
local start=body.on['base.start']
body.on['base.start']=function(self,sender,args)
    start(self,sender,args)
    if args and args.source then
        assert(self:measure_instance(args.receiver).capacity_m3>0)
        assert(next(self:asset(args.receiver).bindings)==nil)
        body.on['base.hold'](self,args.source)
        body.on['base.pour'](self,args.receiver,{amount=0.00002})
    end
end
return body
"""
    )
    next(row for row in plan["scripts"] if row["key"] == "body")["source"] = source
    plan["bootstrap"].append(
        {
            "target": body["key"],
            "event": "base.start",
            "payload": {
                "source": {"object": "test.pour_source"},
                "receiver": {"object": "test.pour_receiver"},
            },
        }
    )
    plan_path, report = tmp_path / "plan.json", tmp_path / "report.json"
    plan_path.write_text(json.dumps(plan))
    result = subprocess.run(
        [
            str(ROOT / ".build/release/amb-server"),
            "--plan",
            str(plan_path),
            "--config",
            str(ROOT / "config.toml"),
            "--assets",
            str(ROOT / "assets"),
            "--data",
            str(tmp_path / "world"),
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
        timeout=30,
        check=False,
    )
    assert result.returncode == 0, (result.stdout, result.stderr)
    state = json.loads(report.read_text())
    assert state["script_errors"] == 0
    receiver_id = max(row["model"] for row in state["objects"])
    receiver = next(row for row in state["objects"] if row["model"] == receiver_id)
    assert receiver["values"]["base.liquid"] == pytest.approx(0.00002)
