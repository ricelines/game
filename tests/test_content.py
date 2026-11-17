"""Checks over the authored package and real bake/validator artifacts."""

import asyncio
import io
import json
import math
import re
import runpy
import socket
import subprocess
import sys
import tomllib
from types import SimpleNamespace

import pytest
import zstandard

from content.prepare import ROOT, prepare, registry, select, validate_symbols


@pytest.fixture(scope="module")
def authored():
    return json.loads((ROOT / "content/start.json").read_text())


def test_every_start_has_closed_references_and_private_connection_modes(authored):
    for mode in ("seeded", "forage"):
        for stakes in (True, False):
            plan = select(authored, mode, stakes)
            validate_symbols(plan)
            assert plan["newcomer"]["body_model"] == "body"
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
                        for owner, at in [
                            (2, [8, 50]),
                            (2, [24, 50]),
                            (3, [40, 50]),
                            (4, [56, 50]),
                            (5, [-16, 50]),
                            (6, [-32, 50]),
                            (7, [-48, 50]),
                        ]
                    ]
                    if mode == "seeded"
                    else []
                ),
            }
            assert not any(
                "caller" in row or "owned_on_plot" in row["scope"] for row in plan["grants"]
            )
            newcomer = plan["newcomer"]["body_installs"]
            for profile in plan["profiles"]:
                if "body" not in profile:
                    continue
                normalized = json.loads(json.dumps(profile["body_installs"]))
                for group in normalized:
                    for choice in group:
                        scope = choice["scope"]
                        if "avatar" in scope:
                            scope["avatar"] = "$body"
                        if "owned" in scope:
                            scope["owned"] = "$player"
                assert normalized == newcomer
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
                own_plots = [
                    row["at"] for row in plan["land"]["lots"] if row["owner"] == profile["id"]
                ]
                assert len(build) == 2 * len(own_plots)
                for at in own_plots:
                    assert {
                        row["holder"]["script"]
                        for row in build
                        if row["issuer"] == profile["id"] and row["scope"] == {"plot_at": at}
                    } == {"body", "stakes" if stakes else "stakes_free"}
                owned = [
                    r
                    for r in plan["grants"]
                    if isinstance(r["holder"], dict)
                    and r["holder"]["on"] == profile["body"]
                    and r["rights"] == ["edit", "destroy"]
                ]
                assert len(owned) == 1 and owned[0]["scope"] == owner_scope


def test_root_architect_allowance_and_balance_holders(authored):
    plan = select(authored, "seeded", True)
    allowance = next(r for r in plan["grants"] if r["holder"] == "p:1")
    assert allowance["issuer"] == 0 and allowance["scope"] == {"account": "p:0"}
    assert allowance["cap"] == 1000000 and allowance["period_s"] == 604800
    plan["balances"].append({"holder": "public.circuit", "amount": 100})
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
    assert tank["cavity"]["min"] == pytest.approx([-1.98, -0.98, 0.03])
    assert tank["cavity"]["max"] == pytest.approx([1.98, 0.98, 2.0])
    assert tank["bindings"]["container"] == {}


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
    frame_volume = (2 * 0.12 * 2.6 + 2.48 * 0.12) * 0.22
    assert door["volume"] + child["volume"] == pytest.approx(frame_volume + 2.4 * 0.08 * 2.4)
    body = manifest("body")
    assert "mass" not in body["bindings"]["character"]
    assert body["size"] == pytest.approx([0.70, 0.35, 1.75])
    assert "radius" not in body["bindings"]["character"]
    assert "height" not in body["bindings"]["character"]
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
            "--profile",
            "development",
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
    assert "land tax:" in land_report(state)["writing"]
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
            "--profile",
            "development",
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


def land_world(tmp_path, baked_plan, *, probe="", steps=40, settings=None, edit=None, timeout=40):
    """Exercise the authored settlement against the actual native permission/store path."""
    plan = json.loads(json.dumps(baked_plan))
    for profile in plan["profiles"]:
        profile["key_file"] = str(tmp_path / "keys" / f"{profile['id']}.key")
    plan["world_settings"].update({"base.settlement_seconds": 0.1, "base.day_seconds": 1})
    plan["world_settings"].update(settings or {})
    script = next(row for row in plan["scripts"] if row["key"] == "land")
    ending = "\nreturn {kind='world',actions="
    assert ending in script["source"]
    script["source"] = script["source"].replace(ending, "\n" + probe + ending)
    if edit:
        edit(plan)
    path, report = tmp_path / "plan.json", tmp_path / "report.json"
    path.write_text(json.dumps(plan))
    native = subprocess.run(
        [
            str(ROOT / ".build/release/amb-server"),
            "--profile",
            "development",
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
            str(steps),
            "--report",
            str(report),
        ],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )
    assert native.returncode == 0, (native.stdout, native.stderr)
    state = json.loads(report.read_text())
    assert state["script_errors"] == 0, (state["script_errors"], tmp_path / "world", native.stderr)
    return state


def lease_fixture(plan, *, cap=1000, rights=None):
    model = next(row for row in plan["models"] if row["key"] == "breaker").copy()
    model.update(key="test_lease", script="lease", author=2)
    plan["models"].append(model)
    obj = next(row for row in plan["objects"] if row["model"] == "breaker").copy()
    obj.update(key="test.lease", model="test_lease", owner=2, at=[30, 78, 0], state={}, settings={})
    plan["objects"].append(obj)
    goods = obj.copy()
    goods.update(key="test.tenant_goods", model="chair", owner=3, at=[32, 78, 0])
    plan["objects"].append(goods)
    for key, at in (("resident.mira", [30, 77, 0]), ("resident.sol", [31, 77, 0])):
        next(row for row in plan["objects"] if row["key"] == key)["at"] = at
    holder = {"script": "lease", "on": "test.lease"}
    plan["grants"].extend(
        [
            {
                "issuer": 2,
                "holder": holder,
                "scope": {"plot_at": [24, 50]},
                "rights": rights or ["build", "enter", "run_scripts", "return"],
                "cap": 0,
                "period_s": 0,
                "expires_s": 31536000,
            },
            {
                "issuer": 3,
                "holder": holder,
                "scope": {"account": "p:3"},
                "rights": ["spend"],
                "cap": cap,
                "period_s": 86400,
                "expires_s": 31536000,
            },
        ]
    )
    plan["bootstrap"].append({"target": "test.lease", "event": "base.start", "payload": None})


async def live_land_world(tmp_path, baked_plan, exercise, *, edit=None, settings=None):
    from game_elaboration import Evidence, Player
    from game_restart_live import Native

    plan = json.loads(json.dumps(baked_plan))
    for profile in plan["profiles"]:
        profile["key_file"] = str(tmp_path / "keys" / f"{profile['id']}.key")
    plan["world_settings"].update({"base.settlement_seconds": 0.1, "base.day_seconds": 1})
    plan["world_settings"].update(settings or {})
    if edit:
        edit(plan)
    path = tmp_path / "plan.json"
    path.write_text(json.dumps(plan))
    with socket.socket() as port:
        port.bind(("127.0.0.1", 0))
        number = port.getsockname()[1]
    args = SimpleNamespace(
        server=ROOT / ".build/release/amb-server",
        plan=path,
        config=ROOT / "config.toml",
        profile="development",
        port=number,
    )
    evidence = Evidence(tmp_path / "evidence")
    native = Native(args, tmp_path / "world", tmp_path, "native")
    await native.start()
    try:
        async with (
            Player(
                "Owner", f"ws://127.0.0.1:{number}/ws", tmp_path / "keys/2.key", evidence
            ) as owner,
            Player(
                "Tenant", f"ws://127.0.0.1:{number}/ws", tmp_path / "keys/3.key", evidence
            ) as tenant,
        ):
            await exercise(owner, tenant)
    finally:
        # Deliberate caller/signature refusals are retained as real script error records.
        if native.process and native.process.poll() is None:
            native.process.terminate()
            await asyncio.to_thread(native.process.wait, 30)
        for log in native.logs:
            log.close()
        evidence.save("completed")
    assert native.process.returncode == 0
    state = json.loads(native.report.read_text())
    return state


async def lease_handle(player):
    view = await player.request("look", limit=1000)
    for row in view["things"] + view["far"]:
        if any("base.lease_sign" in action for action in row.get("actions", [])):
            return row["h"]
    raise AssertionError("lease was not visible")


@pytest.mark.parametrize("case", ["paid", "unpaid", "missing_return", "pool"])
def test_lease_dispatch_rights_signing_and_eviction(tmp_path, baked_plan, case):
    async def exercise(owner, tenant):
        handle = await lease_handle(owner)
        # The owner has edit; the tenant has use, but has no edit authority.
        terms = {"tenant": "p:3", "rent": 100, "grace_days": 0.2}
        denied = await tenant.act("base.lease_terms", handle, **terms)
        assert not denied["ok"] and denied["denied"]["need"] == "edit"
        assert (
            await owner.act(
                "engine.grant",
                holder="p:3",
                scope={"object": handle},
                rights=["edit"],
                expires_s=60,
            )
        )["ok"]
        wrong_terms = await tenant.act("base.lease_terms", handle, **terms)
        assert any("Only the plot owner" in error for error in wrong_terms["script_errors"])
        set_terms = await owner.act("base.lease_terms", handle, **terms)
        assert set_terms["ok"] and not set_terms.get("script_errors")
        wrong = await owner.act("base.lease_sign", handle)
        assert any("Only the named tenant" in error for error in wrong["script_errors"])
        signed = await tenant.act("base.lease_sign", handle)
        if case == "missing_return":
            assert any("land rights" in error for error in signed["script_errors"])
            return
        assert signed["ok"] and not signed.get("script_errors")
        started = (await owner.request("look", limit=1))["step"]
        if case == "pool":
            plot = await owner.request("inspect", target="plot")
            assert (await owner.act("engine.abandon_plot", plot=plot["plot"]))["ok"]
        await owner.wait_until_step(started + 20)
        grants = (await tenant.act("engine.list_grants"))["held"]
        delegated = [
            row for row in grants if row.get("parent") and row.get("scope", {}).get("plot")
        ]
        if case in {"unpaid", "pool"}:
            assert not delegated
            if case == "pool":
                placed = await owner.act("engine.place_existing", handle, position=[20, 76, 0])
                assert placed["ok"]
                await owner.wait_until_step((await owner.request("look", limit=1))["step"] + 2)
                again = await owner.act("base.lease_sign", handle)
            else:
                again = await tenant.act("base.lease_sign", handle)
            assert any("Only the named tenant" in error for error in again["script_errors"])
            if case == "unpaid":
                inventory = await tenant.act("engine.inventory")
                assert inventory["inventory"]
                plot = await owner.request("inspect", target="plot")
                assert plot["owner"] == "p:2"
        else:
            assert any(set(row["rights"]) == {"build", "enter", "run_scripts"} for row in delegated)
            assert (await tenant.act("engine.balance"))["balance"] < 1000
            assert (await owner.act("engine.balance"))["balance"] > 1000

    state = asyncio.run(
        live_land_world(
            tmp_path,
            baked_plan,
            exercise,
            edit=lambda plan: lease_fixture(
                plan,
                cap=1 if case == "unpaid" else 1000,
                rights=["build", "enter", "run_scripts"] if case == "missing_return" else None,
            ),
        )
    )
    assert state["script_errors"] == (3 if case in {"missing_return", "unpaid", "pool"} else 2)


def land_events(tmp_path):
    with (tmp_path / "world/events.jsonl.zst").open("rb") as stream:
        with zstandard.ZstdDecompressor().stream_reader(stream) as reader:
            return [json.loads(line) for line in io.TextIOWrapper(reader)]


def land_report(state):
    return next(row for row in state["world_scripts"] if row["space"] == "land")


def land_probe_result(state):
    return land_report(state)["state"]["test_result"]


def instrument_land(plan, probe):
    script = next(row for row in plan["scripts"] if row["key"] == "land")
    ending = "\nreturn {kind='world',actions="
    assert ending in script["source"]
    script["source"] = script["source"].replace(ending, "\n" + probe + ending)


def land_player_fixture(plan, *, cap=1000000, architect=False):
    plan["bootstrap"] = [row for row in plan["bootstrap"] if row["target"] == "world"]
    next(row for row in plan["balances"] if row["holder"] == "p:2")["amount"] = 1000000
    if architect:
        next(row for row in plan["profiles"] if row["id"] == 2)["roles"] = ["architect"]
    for grant in plan["grants"]:
        if grant["holder"] == {"script": "land", "on": "world"} and grant["issuer"] == 2:
            grant["cap"] = cap


async def owned_land(player):
    lots, cursor = [], 0
    while True:
        page = await player.request("inspect", target="land", owner="self", cursor=cursor)
        lots.extend(page["land"])
        if not page["more"]:
            return lots
        cursor = page["next_cursor"]


async def wait_for_land_rules(player):
    async with asyncio.timeout(30):
        while True:
            view = await player.request("inspect", target="land", limit=1)
            if any(rule["writing"] for rule in view["rules"]):
                return
            await asyncio.sleep(0.05)


async def land_quote(player, plot):
    start = len(player.events)
    result = await player.act("base.land_quote", plot=plot)
    assert result["ok"] and not result.get("script_errors")
    await player.wait_until_step((await player.request("look", limit=1))["step"] + 2)
    text = next(
        event["text"]
        for event in player.events[start:]
        if event.get("text", "").startswith(f"plot {plot}: tax ")
        or event.get("text") == f"plot {plot}: exempt"
    )
    if text.endswith("exempt"):
        return {"text": text, "bill": 0}
    match = re.search(r"tax ([\d.]+) a day .*bill (\d+) for ([\d.]+) days, due day ([\d.]+)", text)
    assert match, text
    return {
        "text": text,
        "tax": float(match[1]),
        "bill": int(match[2]),
        "period": float(match[3]),
        "due": float(match[4]),
    }


def land_cell_value(plot, settings):
    """Independent cell-by-cell oracle for the small land regression fixtures."""
    x0, y0, x1, y1 = map(int, plot["bounds"])
    front = plot["fronts"] == "street"
    return math.fsum(
        settings["base.land_base"]
        + math.exp(-abs(x + 0.5 - plot["port_x"]) / settings["base.land_along"])
        / (1 + settings["base.land_port_falloff"] * abs(plot["port"]))
        * (
            settings["base.land_front"]
            * math.exp(-(abs(y + 0.5) - 50.5) / settings["base.land_depth"])
            if front
            else settings["base.land_back"]
        )
        for x in range(x0, x1)
        for y in range(y0, y1)
    )


def test_land_reference_values_initial_date_and_writing(tmp_path, baked_plan):
    references = {(8, 50): 2440.01, (8, 82): 992.34}

    async def exercise(owner, tenant):
        lots = await owned_land(owner)
        assert len(lots) == len(references)
        for lot in lots:
            expected = references[tuple(lot["bounds"][:2])]
            quote = await land_quote(owner, lot["plot"])
            assert f"value {expected:.2f}" in quote["text"]
            assert quote["bill"] == math.ceil(expected * 0.01 * 30)
            now = (await owner.request("look", limit=1))["step"] / 20
            assert now < quote["due"] <= now + 7

    def edit(plan):
        land_player_fixture(plan)
        plan["land"]["lots"] = [{"owner": 2, "at": list(at)} for at in references]
        plan["grants"] = [row for row in plan["grants"] if "plot_at" not in row["scope"]]
        instrument_land(
            plan,
            """
local original_boot=boot
boot=function(self)
    original_boot(self)
    if not self.state.boot then
        -- The reference rectangle keeps the origin lot's offset at port 100;
        -- it is a valuation example, not a lot in the surveyed 72 m layout.
        self.state.test_result=value({bounds={100008,50,100024,82},
            port=100,port_x=100000,fronts='street'},applied(self))
    end
end
""",
        )

    state = asyncio.run(live_land_world(tmp_path, baked_plan, exercise, edit=edit))
    assert state["script_errors"] == 0
    assert land_probe_result(state) == pytest.approx(1476.01, abs=0.005)
    writing = land_report(state)["writing"]
    assert "land tax:" in writing
    assert "due day" not in writing


@pytest.mark.parametrize("exempt", [False, True])
def test_land_courtesy_notice_has_no_amount_and_exempt_lots_have_no_bill(
    tmp_path, baked_plan, exempt
):
    state = land_world(
        tmp_path,
        baked_plan,
        steps=12,
        settings={"base.land_notice_days": 0.2, "base.land_exempt_value": 100000 if exempt else 0},
    )
    assert state["script_errors"] == 0
    events = land_events(tmp_path)
    notices = [row for row in events if row.get("name") == "notify"]
    abandoned = [row for row in events if row.get("name") == "engine.abandon_plot"]
    if exempt:
        assert not notices and not abandoned
    else:
        assert notices and abandoned
        for notice in notices:
            match = re.search(r"^plot (\d+): .*due on day ([\d.]+)", notice["text"])
            assert match, notice["text"]
            # The only numbers are the lot and its deadline, never a quoted amount.
            assert re.findall(r"\d+(?:\.\d+)?", notice["text"]) == list(match.groups())
            lapse = next(row for row in abandoned if row["args"]["plot"] == int(match[1]))
            deadline_step = math.ceil(float(match[2]) * 20)
            assert notice["step"] < deadline_step
            assert deadline_step <= lapse["step"] <= deadline_step + 1


@pytest.mark.parametrize("limited", [False, True])
def test_land_full_bill_burns_money_and_preserves_existing_days(tmp_path, baked_plan, limited):
    async def exercise(owner, tenant):
        lot = (await owned_land(owner))[0]
        initial = await land_quote(owner, lot["plot"])
        assert initial["bill"] == 733 and initial["period"] == 30
        assert "rate 0.0100 x value 2440.01" in initial["text"]
        before = (await owner.act("engine.balance"))["balance"]
        if not limited:
            partial = await owner.act("base.land_pay", plot=lot["plot"], amount=initial["bill"] - 1)
            assert partial.get("script_errors")
            assert (await land_quote(owner, lot["plot"]))["due"] == initial["due"]
        paid = await owner.act("base.land_pay", plot=lot["plot"], amount=initial["bill"])
        assert paid["ok"] and not paid.get("script_errors")
        after = await land_quote(owner, lot["plot"])
        if limited:
            assert after["due"] == initial["due"]
            assert (await owner.act("engine.balance"))["balance"] == before
        else:
            assert after["due"] == pytest.approx(initial["due"] + 30)
            assert (await owner.act("engine.balance"))["balance"] == before - initial["bill"]
            early = await owner.act("base.land_pay", plot=lot["plot"], amount=initial["bill"])
            assert early.get("script_errors")
            assert (await land_quote(owner, lot["plot"]))["due"] == after["due"]
            assert (await owner.act("engine.balance"))["balance"] == before - initial["bill"]
        assert (await owner.act("engine.balance", account="p:0"))["balance"] == 0

    def edit(plan):
        land_player_fixture(plan, cap=1 if limited else 1000000)

    state = asyncio.run(live_land_world(tmp_path, baked_plan, exercise, edit=edit))
    assert state["script_errors"] == (0 if limited else 2)


@pytest.mark.parametrize("limited", [False, True])
def test_land_auto_attempts_full_bills_at_due_before_abandonment(tmp_path, baked_plan, limited):
    async def exercise(owner, tenant):
        lots = await owned_land(owner)
        quotes = {lot["plot"]: await land_quote(owner, lot["plot"]) for lot in lots}
        assert len(quotes) == 2
        before = (await owner.act("engine.balance"))["balance"]
        result = await owner.act("base.land_auto", on=True)
        assert result["ok"] and not result.get("script_errors")
        due = min(quote["due"] for quote in quotes.values())
        await owner.wait_until_step(math.ceil((due - 1) * 20))
        assert (await owner.act("engine.balance"))["balance"] == before
        await owner.wait_until_step(
            math.ceil(max(quote["due"] for quote in quotes.values()) * 20) + 2
        )
        for plot, quote in quotes.items():
            record = await owner.request("inspect", target=f"plot:{plot}")
            assert record["owner"] == ("p:0" if limited else "p:2")
            if not limited:
                assert (await land_quote(owner, plot))["due"] == pytest.approx(quote["due"] + 6)
        expected = 0 if limited else sum(quote["bill"] for quote in quotes.values())
        assert (await owner.act("engine.balance"))["balance"] == before - expected

    def edit(plan):
        land_player_fixture(plan, cap=1 if limited else 1000000)

    state = asyncio.run(
        live_land_world(
            tmp_path,
            baked_plan,
            exercise,
            edit=edit,
            settings={"base.land_notice_days": 4, "base.land_period_days": 6},
        )
    )
    assert state["script_errors"] == 0


def test_land_lapse_returns_all_goods_on_only_that_lot(tmp_path, baked_plan):
    async def exercise(owner, tenant):
        lots = await owned_land(owner)
        unpaid = next(row for row in lots if row["bounds"][:2] == [8, 50])
        saved = next(row for row in lots if row["plot"] != unpaid["plot"])
        deadline = (await land_quote(owner, unpaid["plot"]))["due"]
        bill = (await land_quote(owner, saved["plot"]))["bill"]
        assert (await owner.act("base.land_pay", plot=saved["plot"], amount=bill))["ok"]
        await owner.wait_until_step(math.ceil(deadline * 20) - 5)
        assert (await owner.request("inspect", target=f"plot:{unpaid['plot']}"))["owner"] == "p:2"
        await owner.wait_until_step(math.ceil(deadline * 20) + 2)
        assert (await owner.request("inspect", target=f"plot:{unpaid['plot']}"))["owner"] == "p:0"
        assert (await owner.request("inspect", target=f"plot:{saved['plot']}"))["owner"] == "p:2"
        for player, name in ((owner, "Owner lapse marker"), (tenant, "Third-party lapse marker")):
            returned = await player.request("inspect", target="inventory")
            assert any(
                row.get("says", {}).get("name") == name
                for row in returned["inventory"] + returned["mailbox"]
            )

    def edit(plan):
        land_player_fixture(plan)
        chair = next(row for row in plan["objects"] if row["model"] == "chair")
        model = next(row for row in plan["models"] if row["key"] == "chair")
        for owner, name, x in ((2, "Owner lapse marker", 10), (3, "Third-party lapse marker", 11)):
            key = f"test.lapse.{owner}"
            plan["models"].append({**model, "key": key, "text": {**model["text"], "name": name}})
            plan["objects"].append(
                {**chair, "key": key, "model": key, "owner": owner, "at": [x, 80, 0]}
            )

    state = asyncio.run(
        live_land_world(
            tmp_path, baked_plan, exercise, edit=edit, settings={"base.land_notice_days": 4}
        )
    )
    assert state["script_errors"] == 0
    events = land_events(tmp_path)
    assert not any(row.get("name") == "engine.destroy" for row in events)


def test_land_split_join_and_sale_preserve_bill_dates(tmp_path, baked_plan):
    async def exercise(owner, tenant):
        lot = (await owned_land(owner))[0]
        initial = await land_quote(owner, lot["plot"])
        assert (await owner.act("base.land_pay", plot=lot["plot"], amount=initial["bill"]))["ok"]
        paid = await land_quote(owner, lot["plot"])
        split = await owner.act("engine.split_plot", plot=lot["plot"], x=lot["bounds"][0] + 8)
        assert split["ok"]
        parent, child = split["plots"]
        for plot in (parent, child):
            assert (await land_quote(owner, plot))["due"] == paid["due"]
        assert (await owner.act("engine.join_plots", plots=[parent, child]))["ok"]
        other = next(row for row in await owned_land(owner) if row["plot"] != parent)
        assert (await owner.act("engine.join_plots", plots=[parent, other["plot"]]))["ok"]
        joined = await land_quote(owner, parent)
        assert initial["due"] < joined["due"] < paid["due"]
        offer = await owner.act("engine.offer_plot", plot=parent, price=0, buyer="p:3")
        assert offer["ok"]
        assert (await tenant.act("engine.accept", offer=offer["offer"], max_price=0))["ok"]
        assert (await land_quote(tenant, parent))["due"] == joined["due"]

    def edit(plan):
        land_player_fixture(plan)
        instrument_land(
            plan,
            """
local original_changed=changed
changed=function(self,sender,event)
    local da,db=self.state['due:'..event.plot],self.state['due:'..event.other]
    local ta,tb=self.state['tax:'..event.plot],self.state['tax:'..event.other]
    original_changed(self,sender,event)
    if event.op=='join' and da and db then
        assert(math.abs(self.state['due:'..event.plot]-(ta*da+tb*db)/(ta+tb))<0.000001,
            'join did not keep the tax-weighted bill date')
    end
end
""",
        )

    state = asyncio.run(live_land_world(tmp_path, baked_plan, exercise, edit=edit))
    assert state["script_errors"] == 0


def test_land_one_handler_split_then_join_preserves_both_paid_dates(tmp_path, baked_plan):
    async def exercise(owner, tenant):
        await wait_for_land_rules(owner)
        lots = {tuple(row["bounds"][:2]): row for row in await owned_land(owner)}
        a, b = lots[8, 50], lots[24, 50]
        before_a, before_b = await land_quote(owner, a["plot"]), await land_quote(owner, b["plot"])
        balance = (await owner.act("engine.balance"))["balance"]
        result = await owner.act("base.land_pay", plot=a["plot"], amount=before_a["bill"])
        assert result["ok"] and not result.get("script_errors")
        assert (await owner.act("engine.balance"))["balance"] == balance - before_a["bill"]
        after_a, after_b = await land_quote(owner, a["plot"]), await land_quote(owner, b["plot"])
        child = {**a, "bounds": [16, 50, 24, 82]}
        child_value = land_cell_value(child, baked_plan["world_settings"])
        b_value = land_cell_value(b, baked_plan["world_settings"])
        # Both pieces have the same rate and public-entry discount, so these
        # independently summed values give their exact relative tax weights.
        paid_date = before_a["due"] + 30
        expected = (child_value * paid_date + b_value * before_b["due"]) / (child_value + b_value)
        assert after_a["due"] == pytest.approx(paid_date, rel=0, abs=1e-6)
        assert after_b["due"] == pytest.approx(expected, rel=0, abs=1e-6)
        final = {row["plot"]: row for row in await owned_land(owner)}
        assert set(final) == {a["plot"], b["plot"]}
        assert final[a["plot"]]["bounds"] == [8, 50, 16, 82]
        assert final[b["plot"]]["bounds"] == [16, 50, 40, 82]

    def edit(plan):
        land_player_fixture(plan)
        plan["land"]["lots"] = [{"owner": 2, "at": [x, 50]} for x in (8, 24)]
        plan["grants"] = [row for row in plan["grants"] if "plot_at" not in row["scope"]]
        instrument_land(
            plan,
            """
local original_pay=pay
pay=function(self,sender,args,caller)
    original_pay(self,sender,args,caller)
    local neighbor
    for _,id in self:plots(caller) do
        if id~=args.plot then neighbor=id end
    end
    local split=self:act('engine.split_plot',nil,{plot=args.plot,x=16})
    assert(split.ok)
    assert(self:act('engine.join_plots',nil,{plots={neighbor,split.plots[2]}}).ok)
end
""",
        )

    state = asyncio.run(live_land_world(tmp_path, baked_plan, exercise, edit=edit))
    assert state["script_errors"] == 0
    actions = [
        row
        for row in land_events(tmp_path)
        if row.get("kind") == "action"
        and row.get("name") in {"base.land_pay", "engine.split_plot", "engine.join_plots"}
    ]
    assert len(actions) == 3 and len({row["root"] for row in actions}) == 1


def test_land_one_handler_split_then_abandon_restores_pool_and_sale(tmp_path, baked_plan):
    observed = {}

    async def exercise(owner, tenant):
        await wait_for_land_rules(owner)
        lot = (await owned_land(owner))[0]
        initial = await land_quote(owner, lot["plot"])
        balance = (await owner.act("engine.balance"))["balance"]
        result = await owner.act("base.land_pay", plot=lot["plot"], amount=initial["bill"])
        assert result["ok"] and not result.get("script_errors")
        assert (await owner.act("engine.balance"))["balance"] == balance - initial["bill"]
        await owner.wait_until_step((await owner.request("look", limit=1))["step"] + 4)
        assert (await land_quote(owner, lot["plot"]))["due"] == pytest.approx(
            initial["due"] + 30, rel=0, abs=1e-6
        )
        plots, cursor = [], 0
        while True:
            page = await owner.request("inspect", target="land", cursor=cursor)
            plots.extend(page["land"])
            if not page["more"]:
                break
            cursor = page["next_cursor"]
        child = next(row for row in plots if row["bounds"] == [16, 50, 24, 82])
        assert child["owner"] == "p:0"
        offers = (await owner.act("engine.offers"))["offers"]
        assert any(offer.get("plot") == child["plot"] for offer in offers)
        assert [row["plot"] for row in await owned_land(owner)] == [lot["plot"]]
        observed.update(plots=plots, child=child["plot"])

    def edit(plan):
        land_player_fixture(plan)
        plan["land"]["lots"] = [{"owner": 2, "at": [8, 50]}]
        plan["grants"] = [row for row in plan["grants"] if "plot_at" not in row["scope"]]
        instrument_land(
            plan,
            """
local original_pay=pay
pay=function(self,sender,args,caller)
    original_pay(self,sender,args,caller)
    local split=self:act('engine.split_plot',nil,{plot=args.plot,x=16})
    assert(split.ok)
    assert(self:act('engine.abandon_plot',nil,{plot=split.plots[2]}).ok)
end
""",
        )

    state = asyncio.run(
        live_land_world(
            tmp_path, baked_plan, exercise, edit=edit, settings={"base.land_on_offer": 32}
        )
    )
    assert state["script_errors"] == 0
    saved = land_report(state)["state"]
    assert f"due:{observed['child']}" not in saved
    for class_name in ("street", "back"):
        plots = [
            plot
            for plot in observed["plots"]
            if ("street" if plot["fronts"] == "street" else "back") == class_name
        ]
        expected = math.fsum(land_cell_value(plot, baked_plan["world_settings"]) for plot in plots)
        pool = math.fsum(
            land_cell_value(plot, baked_plan["world_settings"])
            for plot in plots
            if plot["owner"] == "p:0"
        )
        assert saved[f"sum_v:{class_name}"] == pytest.approx(expected, rel=0, abs=1e-6)
        assert saved[f"pool_v:{class_name}"] == pytest.approx(pool, rel=0, abs=1e-6)
    actions = [
        row
        for row in land_events(tmp_path)
        if row.get("kind") == "action"
        and row.get("name") in {"base.land_pay", "engine.split_plot", "engine.abandon_plot"}
    ]
    assert len(actions) == 3 and len({row["root"] for row in actions}) == 1


def test_land_join_during_boot_gives_uncached_source_its_initial_notice(tmp_path, baked_plan):
    def edit(plan):
        land_player_fixture(plan)
        plan["land"]["lots"] = [
            {"owner": 2, "at": [8 + x * 16, 50 + j * 72 + y * 32]}
            for j in range(10)
            for x in range(4)
            for y in range(2)
        ]
        plan["grants"] = [row for row in plan["grants"] if "plot_at" not in row["scope"]]

    probe = """
local original_boot=boot
boot=function(self)
    if self.state.boot>1 and not self.state.test_joined then
        local previous
        for _,id in self:plots('p:2') do
            if not self.state['v:'..id] then
                assert(previous,'fixture needs one cached source')
                local a,b=self:plot(previous),self:plot(id)
                assert(a.bounds[3]==b.bounds[1] and a.bounds[2]==b.bounds[2]
                    and a.bounds[4]==b.bounds[4],'fixture needs adjacent sources')
                self.state.test_x,self.state.test_y=a.bounds[1],a.bounds[2]
                self.state.test_original_due=self.state['due:'..previous]
                self.state.test_parent,self.state.test_other=previous,id
                assert(not self.state['due:'..id],'second source was already assessed')
                assert(self:act('engine.join_plots',nil,{plots={previous,id}}).ok)
                self.state.test_joined=true
                break
            end
            previous=id
        end
    end
    original_boot(self)
end
local original_changed=changed
changed=function(self,sender,event)
    if event.op=='join' and event.plot==self.state.test_parent then
        self.state.test_join_time=self:now()
    end
    original_changed(self,sender,event)
end
"""
    state = land_world(tmp_path, baked_plan, edit=edit, probe=probe, steps=12)
    saved = land_report(state)["state"]
    assert saved["test_joined"] and "boot" not in saved
    x, y = saved["test_x"], saved["test_y"]
    a = {
        "bounds": [x, y, x + 16, y + 32],
        "fronts": "street" if y == 50 else "back way",
        "port": 0,
        "port_x": 0,
    }
    b = {**a, "bounds": [x + 16, y, x + 32, y + 32]}
    va, vb = (land_cell_value(plot, baked_plan["world_settings"]) for plot in (a, b))
    old_due, new_due = saved["test_original_due"], saved["test_join_time"] + 7
    assert old_due < new_due
    expected = (va * old_due + vb * new_due) / (va + vb)
    assert saved[f"due:{int(saved['test_parent'])}"] == pytest.approx(expected, rel=0, abs=1e-9)
    assert f"due:{int(saved['test_other'])}" not in saved
    assert sum(key.startswith("tax:") for key in saved) == 79


def test_land_legislation_waits_for_notice_without_moving_due_dates(tmp_path, baked_plan):
    async def exercise(owner, tenant):
        lot = (await owned_land(owner))[0]
        initial = await land_quote(owner, lot["plot"])
        assert (await owner.act("base.land_pay", plot=lot["plot"], amount=initial["bill"]))["ok"]
        paid = await land_quote(owner, lot["plot"])
        requested = (await owner.request("look", limit=1))["step"]
        assert (await owner.act("engine.world_setting", values={"base.land_street_rate": 0.02}))[
            "ok"
        ]
        await owner.wait_until_step(requested + 4)
        rules = (await owner.request("inspect", target="land", limit=1))["rules"]
        writing = "\n".join(rule["writing"] for rule in rules)
        announced = re.search(
            r"land_street_rate: current 0.01, coming 0.02 on day ([\d.]+)", writing
        )
        assert announced, writing
        effective = float(announced[1])
        assert effective >= requested / 20 + 4
        early = await land_quote(owner, lot["plot"])
        assert early["tax"] == initial["tax"] and early["due"] == paid["due"]
        await owner.wait_until_step(math.ceil(effective * 20) + 4)
        changed = await land_quote(owner, lot["plot"])
        assert changed["tax"] == pytest.approx(initial["tax"] * 2, abs=0.02)
        assert changed["due"] == paid["due"]

    state = asyncio.run(
        live_land_world(
            tmp_path,
            baked_plan,
            exercise,
            edit=lambda plan: land_player_fixture(plan, architect=True),
            settings={"base.land_notice_days": 4},
        )
    )
    assert state["script_errors"] == 0


@pytest.mark.parametrize("count", [600, 1200])
def test_land_large_world_bounds_each_handler_reads(tmp_path, baked_plan, count):
    async def exercise(owner, tenant):
        await wait_for_land_rules(owner)
        assert (await owner.act("engine.world_setting", values={"base.land_street_rate": 0.02}))[
            "ok"
        ]
        async with asyncio.timeout(30):
            while not any(
                event.get("text") == "TEST_LAND_REPLAN_COMPLETE" for event in owner.events
            ):
                await asyncio.sleep(0.05)

    def edit(plan):
        land_player_fixture(plan, architect=True)
        plan["land"]["lots"] = [
            {"owner": 2, "at": [i * 72 + 8 + x * 16, 50 + j * 72 + y * 32]}
            for i in range((count + 79) // 80)
            for j in range(10)
            for x in range(4)
            for y in range(2)
        ][:count]
        plan["grants"] = [row for row in plan["grants"] if "plot_at" not in row["scope"]]
        instrument_land(plan, probe)

    probe = """
local original_read=read
read=function(self,id)
    self.state.test_reads=(self.state.test_reads or 0)+1
    assert(self.state.test_reads<=BATCH,'plot record budget exceeded')
    return original_read(self,id)
end
local original_boot=boot
boot=function(self)
    self.state.test_reads=0
    original_boot(self)
    self.state.test_boot_reads=(self.state.test_boot_reads or 0)+self.state.test_reads
    if not self.state.boot then
        assert(self.state.test_boot_reads>=COUNT,'initial pass did not read all lots')
    end
end
local original_tick=tick
tick=function(self)
    self.state.test_reads=0
    original_tick(self)
    if self.state['applied:street_rate']==0.02 and not self.state.test_done then
        self.state.test_replan_reads=(self.state.test_replan_reads or 0)+self.state.test_reads
        if not self.state.replan then
            assert(self.state.test_replan_reads>=COUNT,'legislation pass did not read all lots')
            self.state.test_done=true
            self.state.test_result=COUNT
            assert(self:act('engine.notify',nil,
                {player='p:2',text='TEST_LAND_REPLAN_COMPLETE'}).ok)
        end
    else
        assert(self.state.test_reads==0,'ordinary settlement read plot records')
    end
end
""".replace("COUNT", str(count))
    state = asyncio.run(
        live_land_world(
            tmp_path,
            baked_plan,
            exercise,
            edit=edit,
            settings={"base.land_notice_days": 0.2, "base.land_exempt_value": 100000},
        )
    )
    assert state["script_errors"] == 0
    assert land_probe_result(state) == count


def test_forage_land_offers_halve_at_one_day_and_reach_floor(tmp_path):
    plan = prepare(
        ROOT / "content/start.json", ROOT / "assets", mode="forage", stakes=True, restricted=True
    )
    probe = """
local original=tick
tick=function(self)
    original(self)
    if self.state.boot then return end
    local count=0
    for _,class in {'street','back'} do
      for index=1,(self.state['offered:'..class] or 0) do
        local id=self.state['offered:'..class..':'..index]
        count+=1
        local age=self:now()-self.state['age:'..id]
        local v=self.state['v:'..id]
        if math.abs(age-1)<0.025 then
            assert(self.state['price:'..id]==math.ceil(1.5*v),'wrong one-day price')
            self.state.test_half=true
        elseif age>=1.8 then
            assert(self.state['price:'..id]==math.ceil(v),'wrong floor price')
            assert(self.state.test_half,'one-day quote was not checked')
            self.state.test_result=1
        end
      end
    end
    assert(count==4)
end
"""
    state = land_world(tmp_path, plan, probe=probe, steps=44)
    assert land_probe_result(state) == 1


def test_land_remote_player_purchase_burns_posted_price(tmp_path, baked_plan):
    async def exercise(owner, tenant):
        offers = (await owner.act("engine.offers"))["offers"]
        offer = None
        for candidate in offers:
            if "plot" in candidate:
                lot = await owner.request("inspect", target=f"plot:{candidate['plot']}")
                if lot["dist_m"] > 30:
                    offer = candidate
                    break
        assert offer is not None
        before = (await owner.act("engine.balance"))["balance"]
        view = await owner.request("look", limit=1)
        assert (await owner.act("engine.accept", offer=offer["id"], max_price=offer["price"]))["ok"]
        assert before - (await owner.act("engine.balance"))["balance"] == offer["price"]
        assert (await owner.act("engine.balance", account="p:0"))["balance"] == 0
        await owner.wait_until_step(view["step"] + 6)
        assert (await owner.request("inspect", target=f"plot:{offer['plot']}"))["owner"] == "p:2"
        quote = await land_quote(owner, offer["plot"])
        before = (await owner.act("engine.balance"))["balance"]
        paid = await owner.act("base.land_pay", plot=offer["plot"], amount=quote["bill"])
        assert paid["ok"] and not paid.get("script_errors")
        assert (await owner.act("engine.balance"))["balance"] == before - quote["bill"]
        assert (await owner.act("engine.balance", account="p:0"))["balance"] == 0
        assert (await land_quote(owner, offer["plot"]))["due"] == pytest.approx(quote["due"] + 30)

    def edit(plan):
        land_player_fixture(plan)
        instrument_land(
            plan,
            """
local original_changed=changed
changed=function(self,sender,event)
    original_changed(self,sender,event)
    if event.op=='owner' and event.old_owner=='p:0' and event.owner~='p:0' then
        local s=applied(self,settings(self))
        assert(math.abs(self.state['due:'..event.plot]-self:now()-s.notice_days*s.day)<0.000001,
            'pool purchase did not receive its initial notice period')
        self.state.test_result=1
    end
end
""",
        )

    state = asyncio.run(
        live_land_world(
            tmp_path, baked_plan, exercise, edit=edit, settings={"base.land_halving_days": 1e12}
        )
    )
    assert state["script_errors"] == 0 and land_probe_result(state) == 1


@pytest.mark.parametrize("discount", [0.5, 1])
def test_land_entry_permission_changes_retax_immediately(tmp_path, baked_plan, discount):
    async def exercise(owner, tenant):
        lot = (await owned_land(owner))[0]
        initial = await land_quote(owner, lot["plot"])
        assert (
            await owner.act("engine.plot_permissions", plot=lot["plot"], everyone=["run_scripts"])
        )["ok"]
        # Bulk payment uses cached tax, so a quote cannot mask a missing permission event.
        closed_bill = 733
        if discount == 0.5:
            other = next(row for row in await owned_land(owner) if row["plot"] != lot["plot"])
            closed_bill += (await land_quote(owner, other["plot"]))["bill"]
        paid = await owner.act("base.land_pay", amount=closed_bill)
        assert paid["ok"] and not paid.get("script_errors")
        closed = await land_quote(owner, lot["plot"])
        assert "closed" in closed["text"]
        assert closed["tax"] == pytest.approx(24.4001, abs=0.01)
        if discount == 0.5:
            assert closed["tax"] == pytest.approx(initial["tax"] * 2, abs=0.02)
            assert closed["due"] == pytest.approx(initial["due"] + 30)
        else:
            assert initial["bill"] == 0 and closed["bill"] > 0
            now = (await owner.request("look", limit=1))["step"] / 20
            assert now + 30 < closed["due"] <= now + 37
        assert (
            await owner.act(
                "engine.plot_permissions", plot=lot["plot"], everyone=["enter", "run_scripts"]
            )
        )["ok"]
        reopened = await land_quote(owner, lot["plot"])
        assert reopened["bill"] == initial["bill"]
        if discount == 0.5:
            assert reopened["due"] == closed["due"]

    state = asyncio.run(
        live_land_world(
            tmp_path,
            baked_plan,
            exercise,
            edit=land_player_fixture,
            settings={"base.land_open_discount": discount},
        )
    )
    assert state["script_errors"] == 0


@pytest.mark.parametrize("limited", [False, True])
def test_land_bulk_bill_pays_eighty_lots_without_reads_or_new_timers(tmp_path, baked_plan, limited):
    async def exercise(owner, tenant):
        await owner.wait_until_step((await owner.request("look", limit=1))["step"] + 8)
        amount = next(
            int(event["text"].removeprefix("TEST_BULK_BILL "))
            for event in owner.events
            if event.get("text", "").startswith("TEST_BULK_BILL ")
        )
        assert amount > 80
        before = (await owner.act("engine.balance"))["balance"]
        result = await owner.act("base.land_pay", amount=amount)
        assert result["ok"] and not result.get("script_errors")
        assert (await owner.act("engine.balance"))["balance"] == before - (0 if limited else amount)
        if not limited:
            await owner.wait_until_step(100)

    def edit(plan):
        land_player_fixture(plan, cap=1 if limited else 1000000)
        plan["land"]["lots"] = [
            {"owner": 2, "at": [i * 72 + 8 + x * 16, 50 + j * 72 + y * 32]}
            for i in range(2)
            for j in range(5)
            for x in range(4)
            for y in range(2)
        ]
        plan["grants"] = [row for row in plan["grants"] if "plot_at" not in row["scope"]]
        instrument_land(
            plan,
            """
local original_boot=boot
boot=function(self)
    original_boot(self)
    if self.state.boot then return end
    local s=applied(self,settings(self))
    local total=0
    assert(#self:plots('p:2')==80)
    for _,id in self:plots('p:2') do total+=math.ceil(s.period_days*self.state['tax:'..id]) end
    assert(self:act('engine.notify',nil,{player='p:2',text='TEST_BULK_BILL '..total}).ok)
end
local original_read=read
read=function(self,id)
    assert(not self.state.test_bulk_running,'bulk payment read a plot record')
    self.state.test_reads=(self.state.test_reads or 0)+1
    return original_read(self,id)
end
local original_schedule=schedule
schedule=function(self,id,...)
    assert(not self.state.test_bulk_running,'bulk payment set a timer')
    return original_schedule(self,id,...)
end
local original_pay=pay
pay=function(self,sender,args,caller)
    local s=applied(self,settings(self))
    local before,timers={},{}
    for _,id in self:plots(caller) do
        before[id]=self.state['due:'..id]
        timers[id]=self.state['scheduled:'..id]
    end
    self.state.test_bulk_running=true
    original_pay(self,sender,args,caller)
    self.state.test_bulk_running=false
    for id,date in before do
        assert(self.state['due:'..id]==date+(if LIMITED then 0 else s.period_days*s.day),
            'bulk bill changed the wrong date')
        assert(self.state['scheduled:'..id]==timers[id],'bulk payment replaced an existing timer')
    end
    self.state.test_paid=true
end
local original_due=due
due=function(self,sender,id)
    local paid=self.state.test_paid and not LIMITED and self.state['tax:'..id]
    self.state.test_reads=0
    original_due(self,sender,id)
    assert(self.state.test_reads<=1,'one due timer read multiple records')
    if paid then
        assert(self.state['due:'..id]>self:now(),'early timer lost the paid bill date')
        self.state.test_replanned=(self.state.test_replanned or 0)+1
        self.state.test_result=-self.state.test_replanned
    end
end
""".replace("LIMITED", "true" if limited else "false"),
        )

    state = asyncio.run(
        live_land_world(
            tmp_path,
            baked_plan,
            exercise,
            edit=edit,
            settings={"base.land_notice_days": 4, "base.land_period_days": 6},
        )
    )
    assert state["script_errors"] == 0
    payments = [row for row in land_events(tmp_path) if row.get("name") == "payment"]
    assert len(payments) == (0 if limited else 1)
    if not limited:
        assert payments[0]["to"] == "p:0" and payments[0]["amount"] > 80
        assert land_probe_result(state) == -80


def test_returned_aquarium_is_data_until_placed_again(tmp_path, baked_plan):
    async def exercise(owner, tenant):
        await owner.wait_until_step(12)
        inventory = (await owner.act("engine.inventory"))["inventory"]
        stored = {}
        for handle in inventory:
            obj = await owner.request("inspect", target=handle)
            stored[obj["says"]["name"]] = handle
        school = stored["Living school"]
        tank = stored["Living display"]
        assert (await owner.act("engine.place_existing", tank, position=[16, 70, 0.85]))["ok"]
        assert (await owner.act("engine.place_existing", school, position=[16, 70, 0]))["ok"]
        step = (await owner.request("look", limit=1))["step"]
        await owner.wait_until_step(step + 8)

    def edit(plan):
        for row in plan["objects"]:
            if row["key"] in {"cafe.tank", "cafe.school"}:
                row["owner"] = 2
        script = next(row for row in plan["scripts"] if row["key"] == "land")
        script["source"] = """
return {kind='world',on={
 ['base.start']=function(self) self:after(0.2,'base.land_tick') end,
 ['base.land_tick']=function(self)
    for _,id in self:plots() do
        local p=self:plot(id)
        if p.bounds[1]==24 and p.bounds[2]==50 then
            assert(self:act('engine.abandon_plot',nil,{plot=id}).ok)
        end
    end
 end}}
"""

    state = asyncio.run(live_land_world(tmp_path, baked_plan, exercise, edit=edit))
    assert state["script_errors"] == 0
    rows = land_events(tmp_path)
    swimming = [row for row in rows if row.get("name") == "aq.swim"]
    placed = [row for row in rows if row.get("name") == "engine.place_existing"]
    returned = next(row for row in rows if row.get("name") == "engine.abandon_plot")
    assert len(placed) == 2 and swimming
    assert not any(returned["step"] < row["step"] < placed[-1]["step"] for row in swimming)
    assert any(row["step"] > placed[-1]["step"] for row in swimming)


def test_land_join_exempt_pieces_starts_a_fresh_notice_period(tmp_path, baked_plan):
    async def exercise(owner, tenant):
        lot = (await owned_land(owner))[0]
        initial = await land_quote(owner, lot["plot"])
        assert initial["bill"] > 0
        split = await owner.act("engine.split_plot", plot=lot["plot"], x=lot["bounds"][0] + 8)
        assert split["ok"]
        for plot in split["plots"]:
            assert (await land_quote(owner, plot))["bill"] == 0
        before = (await owner.request("look", limit=1))["step"] / 20
        assert (await owner.act("engine.join_plots", plots=split["plots"]))["ok"]
        joined = await land_quote(owner, split["plots"][0])
        after = (await owner.request("look", limit=1))["step"] / 20
        assert joined["bill"] == initial["bill"]
        assert before + 7 <= joined["due"] <= after + 7
        assert joined["due"] > initial["due"]

    state = asyncio.run(
        live_land_world(
            tmp_path,
            baked_plan,
            exercise,
            edit=land_player_fixture,
            settings={"base.land_exempt_value": 2000},
        )
    )
    assert state["script_errors"] == 0


def test_plot_changed_dispatches_to_land_and_lease_but_not_build_only(tmp_path, baked_plan):
    def edit(plan):
        lease_fixture(plan)
        # Recipients stay placed when the target plot is abandoned.
        next(row for row in plan["objects"] if row["key"] == "test.lease")["at"] = [12, 49, 0]
        model = next(row for row in plan["models"] if row["key"] == "test_lease").copy()
        model.update(key="test_builder", script="builder")
        plan["models"].append(model)
        obj = next(row for row in plan["objects"] if row["key"] == "test.lease").copy()
        obj.update(key="test.builder", model="test_builder")
        plan["objects"].append(obj)
        for script, obj_key, rights in (
            ("lease", "test.lease", ["land_return"]),
            ("builder", "test.builder", ["land_build"]),
        ):
            plan["grants"].append(
                {
                    "issuer": 0,
                    "holder": {"script": script, "on": obj_key},
                    "scope": {"all_land": True},
                    "rights": rights,
                    "cap": 0,
                    "period_s": 0,
                    "expires_s": 31536000,
                }
            )
        lease = next(row for row in plan["scripts"] if row["key"] == "lease")
        observer = """
        ['base.plot_changed']=function(self,_,event)
            assert(event.owner=='p:2' or event.owner=='p:3' or event.owner=='p:0')
            assert(event.old_owner=='p:2' or event.old_owner=='p:3')
            self:act('engine.edit_text',self.handle,{text={writing=string.format(
                'LEASE_EVENT %s %d %d %s %s %g %g',event.op,event.plot,event.other,
                event.owner,event.old_owner,event.area,event.other_area)}})
        end,
"""
        lease["source"] = lease["source"].replace("    on={", "    on={" + observer)
        plan["scripts"].append(
            {
                **lease,
                "key": "builder",
                "source": """return {kind='instance',slots={'build'},on={
                ['base.plot_changed']=function(self)
                    error('build-only script received plot event')
                end
            }}""",
            }
        )
        plan["bootstrap"] = [
            row for row in plan["bootstrap"] if row["target"] in {"world", "test.lease"}
        ]

    probe = """
local original_changed=changed
changed=function(self,sender,event)
    original_changed(self,sender,event)
    self.state.test_events=(self.state.test_events or 0)+1
    self:act('engine.edit_text',self.handle,{text={writing=string.format(
        'LAND_EVENT %s %d %d %s %s %g %g',event.op,event.plot,event.other,
        event.owner,event.old_owner,event.area,event.other_area)}})
end
local original_tick=tick
tick=function(self)
    original_tick(self)
    if self.state.boot then return end
    local stage=self.state.test_stage or 0
    if stage==0 then
        local id=self:plots('p:2')[1]
        local plot=self:plot(id)
        self.state.test_parent=id
        local result=self:act('engine.split_plot',nil,{plot=id,x=plot.bounds[1]+4})
        assert(result.ok)
        self.state.test_child=result.plots[2]
    elseif stage==1 then
        assert(self:act('engine.join_plots',nil,{plots={self.state.test_parent,self.state.test_child}}).ok)
    elseif stage==2 then
        local offer=self:act('engine.offer_plot',nil,{plot=self.state.test_parent,price=0})
        assert(offer.ok)
        assert(self:act('engine.accept',nil,{offer=offer.offer,max_price=0,beneficiary='p:3',payer='p:3'}).ok)
    elseif stage==3 then
        assert(self:act('engine.abandon_plot',nil,{plot=self.state.test_parent}).ok)
    elseif stage==4 then
        assert(self.state.test_events==4,'land did not receive exactly four events')
        self.state.test_result=4
    end
    self.state.test_stage=stage+1
end
"""
    state = land_world(tmp_path, baked_plan, probe=probe, edit=edit, steps=22)
    assert land_probe_result(state) == 4
    writing = [
        row["args"]["text"]["writing"]
        for row in land_events(tmp_path)
        if row.get("name") == "engine.edit_text"
    ]
    land = [line.removeprefix("LAND_EVENT ") for line in writing if line.startswith("LAND_EVENT ")]
    lease = [
        line.removeprefix("LEASE_EVENT ") for line in writing if line.startswith("LEASE_EVENT ")
    ]
    assert land == lease and len(land) == 4
    parsed = [line.split() for line in land]
    parent, child = parsed[0][1:3]
    assert parsed == [
        ["split", parent, child, "p:2", "p:2", "128", "384"],
        ["join", parent, child, "p:2", "p:2", "128", "384"],
        ["owner", parent, "0", "p:3", "p:2", "512", "0"],
        ["owner", parent, "0", "p:0", "p:3", "512", "0"],
    ]


def test_newly_surveyed_pool_lots_reach_land_and_lease(tmp_path, baked_plan):
    def edit(plan):
        lease_fixture(plan)
        next(row for row in plan["objects"] if row["key"] == "test.lease")["at"] = [12, 49, 0]
        plan["grants"].append(
            {
                "issuer": 0,
                "holder": {"script": "lease", "on": "test.lease"},
                "scope": {"all_land": True},
                "rights": ["land_return"],
                "cap": 0,
                "period_s": 0,
                "expires_s": 31536000,
            }
        )
        for row in plan["balances"]:
            if row["holder"] == "p:2":
                row["amount"] = 1000000
        for row in plan["grants"]:
            if row["issuer"] == 2 and "spend" in row["rights"]:
                row["cap"] = 1000000
        lease = next(row for row in plan["scripts"] if row["key"] == "lease")
        lease["source"] = lease["source"].replace(
            "    on={",
            """    on={
        ['base.plot_changed']=function(self,_,event)
            if event.owner=='p:0' and event.old_owner=='p:0' then
                assert(event.op=='owner' and event.other==0 and event.other_area==0)
                assert(event.area==512)
                self:act('engine.edit_text',self.handle,{text={writing='LEASE_POOL '..event.plot}})
            end
        end,
""",
        )
        plan["bootstrap"] = [
            row for row in plan["bootstrap"] if row["target"] in {"world", "test.lease"}
        ]

    probe = """
local original_changed=changed
changed=function(self,sender,event)
    original_changed(self,sender,event)
    if event.owner=='p:0' and event.old_owner=='p:0' then
        local id=event.plot
        assert(self:plot(id).owner=='p:0')
        assert(not self.state['due:'..id],'pool lot got a tax timer')
        self.state.test_new=(self.state.test_new or 0)+1
        self:act('engine.edit_text',self.handle,{text={writing='LAND_POOL '..id}})
    end
end
local original_tick=tick
tick=function(self)
    original_tick(self)
    if self.state.boot or self.state.test_done then return end
    if (self.state.test_new or 0)>=8 then
        local s=applied(self,settings(self))
        local total,pool={street=0,back=0},{street=0,back=0}
        for _,id in self:plots() do
            local plot=read(self,id)
            if plot then
                local class=self.state['class:'..id]
                total[class]+=value(plot,s)
                if plot.owner=='p:0' then pool[class]+=value(plot,s) end
            end
        end
        for _,class in {'street','back'} do
            assert(math.abs(total[class]-self.state['sum_v:'..class])<0.000001)
            assert(math.abs(pool[class]-self.state['pool_v:'..class])<0.000001)
        end
        self.state.test_done=true
        self.state.test_result=8
    else
        local id=self.state['offered:street:1']
        assert(id,'no street lot on offer')
        assert(self:act('engine.accept',nil,{offer=self.state['offer:'..id],
            max_price=self.state['price:'..id],beneficiary='p:2',payer='p:2'}).ok)
    end
end
"""
    state = land_world(tmp_path, baked_plan, edit=edit, probe=probe, steps=30)
    assert land_probe_result(state) == 8
    writing = [
        row["args"]["text"]["writing"]
        for row in land_events(tmp_path)
        if row.get("name") == "engine.edit_text"
    ]
    land = [line.removeprefix("LAND_POOL ") for line in writing if line.startswith("LAND_POOL ")]
    lease = [line.removeprefix("LEASE_POOL ") for line in writing if line.startswith("LEASE_POOL ")]
    assert land == lease and len(land) == 8


def test_land_offer_limit_changes_cancel_and_reopen_quotes(tmp_path, baked_plan):
    async def exercise(owner, tenant):
        await wait_for_land_rules(owner)
        initial = (await owner.act("engine.offers"))["offers"]
        assert len([offer for offer in initial if "plot" in offer]) == 4
        assert (await owner.act("engine.world_setting", values={"base.land_on_offer": 0}))["ok"]
        await owner.wait_until_step((await owner.request("look", limit=1))["step"] + 8)
        assert not any("plot" in offer for offer in (await owner.act("engine.offers"))["offers"])
        assert not (await owner.request("inspect", target="land", for_sale=True))["land"]
        assert (await owner.act("engine.world_setting", values={"base.land_on_offer": 2}))["ok"]
        await owner.wait_until_step((await owner.request("look", limit=1))["step"] + 8)
        reopened = (await owner.act("engine.offers"))["offers"]
        assert len([offer for offer in reopened if "plot" in offer]) == 4
        lots = (await owner.request("inspect", target="land", for_sale=True))["land"]
        assert sum(lot["fronts"] == "street" for lot in lots) == 2
        assert sum(lot["fronts"] != "street" for lot in lots) == 2

    state = asyncio.run(
        live_land_world(
            tmp_path,
            baked_plan,
            exercise,
            edit=lambda plan: land_player_fixture(plan, architect=True),
            settings={"base.land_notice_days": 0.1, "base.land_exempt_value": 100000},
        )
    )
    assert state["script_errors"] == 0


def test_native_bad_inputs_return_signatures_without_changing_money_or_grants(tmp_path, baked_plan):
    async def exercise(owner, tenant):
        owner_balance = (await owner.act("engine.balance"))["balance"]
        tenant_balance = (await tenant.act("engine.balance"))["balance"]
        grants = await owner.request("inspect", target="grants")
        given = {row["id"] for row in grants["given"]}
        invalid = [
            ("engine.pay", {"to": "p:3", "amount": amount}) for amount in (-1, 0.5, True, 1 << 63)
        ]
        invalid.extend(
            [
                ("engine.pay", {"to": "p:3"}),
                ("engine.pay", {"to": "p:3", "amount": 1, "unexpected": True}),
                (
                    "engine.grant",
                    {
                        "holder": "p:3",
                        "scope": {"account": "p:2"},
                        "rights": [],
                        "cap": 10,
                    },
                ),
                (
                    "engine.grant",
                    {
                        "holder": "p:3",
                        "scope": {"owned_on_plot": {"owner": "p:2"}},
                        "rights": ["use"],
                    },
                ),
                ("engine.group_create", {"name": " \t"}),
            ]
        )
        for action, arguments in invalid:
            result = await owner.act(action, **arguments)
            assert result["ok"] is False and result["error"] == "bad_input", result
            assert result["signature"].startswith(action + "(") and result["problem"], result
        assert (await owner.act("engine.balance"))["balance"] == owner_balance
        assert (await tenant.act("engine.balance"))["balance"] == tenant_balance
        assert {
            row["id"] for row in (await owner.request("inspect", target="grants"))["given"]
        } == given
        assert (await owner.act("engine.pay", to="p:3", amount=1))["ok"]
        assert (await owner.act("engine.balance"))["balance"] == owner_balance - 1
        assert (await tenant.act("engine.balance"))["balance"] == tenant_balance + 1

    state = asyncio.run(live_land_world(tmp_path, baked_plan, exercise, edit=land_player_fixture))
    assert state["script_errors"] == 0
