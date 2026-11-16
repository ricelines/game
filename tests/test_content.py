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
    assert len(state["objects"]) == 358
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


def land_world(tmp_path, baked_plan, *, probe, steps=40, settings=None, edit=None):
    """Exercise the authored settlement against the actual native permission/store path."""
    plan = json.loads(json.dumps(baked_plan))
    for profile in plan["profiles"]:
        profile["key_file"] = str(tmp_path / "keys" / f"{profile['id']}.key")
    plan["world_settings"].update({"base.settlement_seconds": 0.1, "base.day_seconds": 1})
    plan["world_settings"].update(settings or {})
    script = next(row for row in plan["scripts"] if row["key"] == "land")
    ending = "return {kind='model',on={['base.start']=tick,['base.land_tick']=tick}}"
    assert ending in script["source"]
    script["source"] = script["source"].replace(ending, probe)
    if edit:
        edit(plan)
    path, report = tmp_path / "plan.json", tmp_path / "report.json"
    path.write_text(json.dumps(plan))
    native = subprocess.run(
        [
            str(ROOT / ".build/release/amb-server"),
            "--plan",
            str(path),
            "--config",
            str(ROOT / "config.toml"),
            "--profile",
            "development",
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
        timeout=40,
        check=False,
    )
    assert native.returncode == 0, (native.stdout, native.stderr)
    state = json.loads(report.read_text())
    assert state["script_errors"] == 0, (state["script_errors"], tmp_path / "world", native.stderr)
    return state


def test_land_values_rent_tier_burn_and_falling_offers(tmp_path, baked_plan):
    probe = """
local function check(self)
    tick(self)
    local a=value(self,{plot=900,bounds={8,50,24,82},port=0,port_x=0,fronts='street'})
    local b=value(self,{plot=901,bounds={8,82,24,114},port=0,port_x=0,fronts='back way'})
    local c=value(self,{plot=902,bounds={100008,50,100024,82},
        port=100,port_x=100000,fronts='street'})
    assert(math.abs(a-2440.01)<0.01 and math.abs(b-992.34)<0.01 and
        math.abs(c-1476.01)<0.01,'cell reference values')
    local day=self:now()
    local rows=self:act('engine.offers',nil,{}).offers
    local count=0
    for _, offer in rows do
        if offer.plot then
            count+=1
            local p=self:plot(offer.plot)
            local v=value(self,p)
            local first='initial_price:'..offer.plot
            if not self.state[first] then self.state[first]=offer.price end
            if day>=1 then assert(offer.price<self.state[first],'auction did not halve') end
            assert(offer.price>=v,'auction fell below floor')
        end
    end
    assert(count==4,'expected two offers per class')
    if day>=1 then
        assert(self:act('engine.balance',nil,{account='p:2'}).balance<1000,'rent was not paid')
        assert(self:act('engine.balance',nil,{account='p:0'}).balance==0,'rent was not burnt')
        self:set('base.charge',1)
    end
end
return {kind='model',on={['base.start']=check,['base.land_tick']=check}}
"""
    state = land_world(tmp_path, baked_plan, probe=probe)
    post = next(row for row in state["objects"] if row["position"] == [12, 49, 0])
    assert post["values"]["base.charge"] == 1


def lease_fixture(plan, *, cap=1000, rent=100, grace=0.3, probe=""):
    lease = next(row for row in plan["scripts"] if row["key"] == "lease")
    lease["source"] = (
        "local rule=(function()\n"
        + lease["source"]
        + "\nend)()\n"
        + """
local original_start=rule.on['base.start']
local original_tick=rule.on['base.lease_tick']
rule.on['base.start']=function(self)
    local terms={tenant='p:3',rent=RENT,grace_days=GRACE}
    assert(not pcall(rule.on['base.lease_terms'],self,nil,terms,'p:3'),
        'tenant set owner terms')
    rule.on['base.lease_terms'](self,nil,terms,'p:2')
    assert(not pcall(rule.on['base.lease_sign'],self,nil,{},'p:4'),
        'wrong tenant signed')
    rule.on['base.lease_sign'](self,nil,{},'p:3')
    original_start(self)
end
rule.on['base.lease_tick']=function(self)
    original_tick(self)
PROBE
end
return rule
""".replace("RENT", str(rent))
        .replace("GRACE", str(grace))
        .replace("PROBE", probe)
    )
    model = next(row for row in plan["models"] if row["key"] == "land_post").copy()
    model.update(key="test_lease", script="lease", author=2)
    plan["models"].append(model)
    obj = next(row for row in plan["objects"] if row["key"] == "public.land_post").copy()
    obj.update(key="test.lease", model="test_lease", owner=2, at=[30, 78, 0])
    plan["objects"].append(obj)
    goods = obj.copy()
    goods.update(key="test.tenant_goods", model="chair", owner=3, at=[32, 78, 0])
    plan["objects"].append(goods)
    holder = {"script": "lease", "on": "test.lease"}
    plan["grants"].extend(
        [
            {
                "issuer": 2,
                "holder": holder,
                "scope": {"plot_at": [24, 50]},
                "rights": ["build", "enter", "run_scripts", "return"],
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


def test_lease_paid_rights_caller_checks_and_separate_land_rent(tmp_path, baked_plan):
    probe = """
    if self:now()>=0.8 then
        assert(self.state.signed,'paid lease ended')
        local given=self:act('engine.list_grants',nil,{}).given
        local rights=false
        for _, grant in given do
            if grant.holder=='p:3' and grant.scope.plot==self.state.plot then
                rights=#grant.rights==3 and grant.parent~=nil
            end
        end
        assert(rights,'tenant lacks delegated build/entry/scripts')
        local tenant=self:act('engine.balance',nil,{account='p:3'}).balance
        local owner=self:act('engine.balance',nil,{account='p:2'}).balance
        assert(tenant<1000 and owner>1000,'lease rent did not reach owner')
        assert(owner<2000-tenant,'owner stopped paying land rent')
        self:set('base.charge',1)
    end
"""
    state = land_world(
        tmp_path,
        baked_plan,
        steps=30,
        probe="return {kind='model',on={['base.start']=tick,['base.land_tick']=tick}}",
        edit=lambda plan: lease_fixture(plan, probe=probe),
    )
    lease = next(row for row in state["objects"] if row["position"] == [30, 78, 0])
    assert lease["values"]["base.charge"] == 1


def test_lease_unpaid_revokes_build_and_returns_tenant_goods(tmp_path, baked_plan):
    probe = """
    if self:now()>=1 then
        assert(not self.state.signed,'unpaid lease did not end')
        assert(self:plot().owner=='p:2','eviction changed plot ownership')
        for _, grant in self:act('engine.list_grants',nil,{}).given do
            assert(grant.holder~='p:3','evicted tenant retains land grant')
        end
        for _, object in self:plot_objects(self.state.plot) do
            assert(self:object(object).owner~='p:3','tenant goods were not returned')
        end
        self:set('base.charge',1)
    end
"""
    state = land_world(
        tmp_path,
        baked_plan,
        steps=30,
        probe="return {kind='model',on={['base.start']=tick,['base.land_tick']=tick}}",
        edit=lambda plan: lease_fixture(plan, cap=1, probe=probe),
    )
    lease = next(row for row in state["objects"] if row["position"] == [30, 78, 0])
    assert lease["values"]["base.charge"] == 1


def test_lease_ends_cleanly_when_land_returns_to_pool(tmp_path, baked_plan):
    land_probe = """
local function abandon(self)
    tick(self)
    if self:now()>=0.4 and not self.state.abandoned then
        for _, id in self:plots() do
            local plot=self:plot(id)
            if plot.bounds[1]==24 and plot.bounds[2]==50 then
                assert(self:act('engine.abandon_plot',nil,{plot=id}).ok,'abandon lease plot')
                self.state.abandoned=true
            end
        end
    end
end
return {kind='model',on={['base.start']=abandon,['base.land_tick']=abandon}}
"""
    lease_probe = """
    if self:now()>=0.6 then
        assert(not self.state.signed,'pool lease remained signed')
        self:set('base.charge',1)
    end
"""
    state = land_world(
        tmp_path,
        baked_plan,
        steps=20,
        probe=land_probe,
        edit=lambda plan: lease_fixture(plan, probe=lease_probe),
    )
    lease = next(row for row in state["objects"] if row["position"] == [30, 78, 0])
    assert lease["values"]["base.charge"] == 1


def test_land_delinquency_holds_salvages_and_returns_third_party(tmp_path, baked_plan):
    def unpaid(plan):
        plan["grants"] = [
            row
            for row in plan["grants"]
            if not (
                row["issuer"] == 2 and row["holder"] == {"script": "land", "on": "public.land_post"}
            )
        ]
        third = next(row for row in plan["objects"] if row["model"] == "chair").copy()
        third.update(key="test.third_party", owner=3, at=[10, 80, 0])
        plan["objects"].append(third)

    probe = """
local function check(self)
    tick(self)
    if self:now()>=0.8 then
        local pooled=false
        for _, id in self:plots() do
            local plot=self:plot(id)
            if plot.bounds[1]==8 and plot.bounds[2]==50 then
                pooled=plot.owner=='p:0'
            end
        end
        assert(pooled,'delinquent lot was not pooled')
        for _, id in self:plots() do
            local p=self:plot(id)
            if p.bounds[1]==8 and p.bounds[2]==50 then
                assert(#self:plot_objects(id)==0,'third-party goods were not returned')
            end
        end
        assert((self.state.held_count or 0)>0,'owner goods were not held')
        local stock=0
        for key, amount in self.state do
            if type(key)=='number' then stock+=amount end
            assert(type(key)~='string' or string.sub(key,1,8)~='held_kg:',
                'held mass was not cleared after salvage')
        end
        assert(stock>0,'salvaged mass did not reach stock')
        self:set('base.charge',1)
    end
end
return {kind='model',on={['base.start']=check,['base.land_tick']=check}}
"""
    state = land_world(
        tmp_path,
        baked_plan,
        steps=24,
        probe=probe,
        edit=unpaid,
        settings={"base.rent_grace_days": 0.2, "base.rent_hold_days": 0.2},
    )
    post = next(row for row in state["objects"] if row["position"] == [12, 49, 0])
    assert post["values"]["base.charge"] == 1
    third = next(row for row in state["objects"] if row["position"] == [10, 80, 0])
    assert third["values"]["base.mass"] > 0  # Returned goods remain live, not salvaged.


def test_land_sale_resets_debt_replenishes_offers_and_charges_remote_owner(tmp_path, baked_plan):
    probe = """
local function check(self)
    tick(self)
    if not self.state.bought then
        for _, offer in self:act('engine.offers',nil,{}).offers do
            if offer.plot and self:plot(offer.plot).fronts=='street' then
                assert(self:act('engine.set_offer_price',nil,{offer=offer.id,price=0}).ok)
                assert(self:act('engine.accept',nil,{
                    offer=offer.id,max_price=0,beneficiary='p:3',payer='p:3'}).ok,'remote purchase')
                self.state.bought=offer.plot
                self.state.balance=self:act('engine.balance',nil,{account='p:3'}).balance
                return
            end
        end
    elseif not self.state.reset then
        assert(self.state['debt:'..self.state.bought]==0,'new owner inherited debt')
        local street=0
        for _, offer in self:act('engine.offers',nil,{}).offers do
            if offer.plot and self:plot(offer.plot).fronts=='street' then street+=1 end
        end
        assert(street==2,'sold lot was not replaced on offer')
        self.state.reset=true
    elseif self:now()>=0.8 then
        assert(self:act('engine.balance',nil,{account='p:3'}).balance<self.state.balance,
            'remote new owner was not charged rent')
        self:set('base.charge',1)
    end
end
return {kind='model',on={['base.start']=check,['base.land_tick']=check}}
"""
    state = land_world(tmp_path, baked_plan, steps=24, probe=probe)
    post = next(row for row in state["objects"] if row["position"] == [12, 49, 0])
    assert post["values"]["base.charge"] == 1


def test_land_tier_charges_concentrated_holdings_more(tmp_path, baked_plan):
    def compare_plan(plan, shared):
        plan["land"]["lots"] = [
            {"owner": 2, "at": [8, 50]},
            {"owner": 2 if shared else 3, "at": [24, 50]},
        ]
        plan["bootstrap"] = [
            row for row in plan["bootstrap"] if row["target"] == "public.land_post"
        ]
        plan["grants"] = [row for row in plan["grants"] if "plot_at" not in row["scope"]]

    paid = []
    for shared in (True, False):
        directory = tmp_path / str(shared)
        directory.mkdir()
        probe = """
local function check(self)
    tick(self)
    local a=self:act('engine.balance',nil,{account='p:2'}).balance
    local b=self:act('engine.balance',nil,{account='p:3'}).balance
    self:set('base.charge',2000-a-b)
end
return {kind='model',on={['base.start']=check,['base.land_tick']=check}}
"""
        state = land_world(
            directory,
            baked_plan,
            steps=24,
            probe=probe,
            settings={"base.land_tier_value": 3000},
            edit=lambda plan, shared=shared: compare_plan(plan, shared),
        )
        post = next(row for row in state["objects"] if row["position"] == [12, 49, 0])
        paid.append(post["values"]["base.charge"])
    assert paid[0] > paid[1] > 0
