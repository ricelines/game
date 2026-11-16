"""Real nested action continuation, permission and S7 attribution regression.

An ordinary authored model is added to a copied start plan for one fresh player.
The test uses the actual server execution stack over authenticated sockets; it
never substitutes an action dispatcher or mutates live tables.
"""

from __future__ import annotations

import argparse
import asyncio
import copy
import hashlib
import io
import json
import pathlib
import time

import zstandard
from game_elaboration import Evidence, Player, require
from game_restart_live import Native

SOURCE = """return {
 kind='model', sets={'base.charge'},
 actions={['base.switch']={right='use'},['base.open']={right='use'},['base.close']={right='use'}},
 on={['base.switch']=function(self,_,args)
  self.state.calls=(self.state.calls or 0)+1
  if args.on then
   local child=self:act('base.switch',self.handle,{on=false})
   self.state.child_ok=child.ok
   self.state.observed_calls=self.state.calls
   local bad=self:act('engine.pay',nil,{to='p:1',amount='wrong'})
   self.state.bad_input=bad.error=='bad_input'
   local paid=self:act('engine.pay',nil,{from='PLAYER',to='p:1',amount=1})
   self.state.pay_denied=paid.ok==false and paid.denied.need=='spend'
   self.state.returned=true
  end
 end,
 ['base.open']=function(self)
  self.state.attempts=(self.state.attempts or 0)+1
  self.state.tokens=self.state.tokens or 10
  self.state.initial_charge=self.state.initial_charge or self:get('base.charge')
  local made=self:act('engine.create',nil,{
   model='m:'..tostring(self:object().model),position=vector.create(0,6,0)})
  if not made.ok then self.state.build_denied=true;return end
  self.state.created=made.created[1]
  self.state.created_mass=self:measure_instance(made.created[1]).mass_kg
  self.state.tokens-=4
  self:set('base.charge',self:get('base.charge')-3)
  self.state.charge=self:get('base.charge')
  self.state.builds=(self.state.builds or 0)+1
  if self.state.builds==2 then error('after completed construction') end
 end,
 ['base.close']=function(self)
  self.state.before_retirement=true
  assert(self:act('engine.destroy',self.handle).ok)
  self.state.after_retirement=true
 end}
}
"""

OUTER_SOURCE = """return {kind='instance',actions={['base.recharge']={right='use'}},
 on={['base.recharge']=function(self,_,args)
  if args.amount==0 then
   self.state.fresh=true
   self.state.calls=(self.state.calls or 0)+1
   return
  end
  self.state.before_uninstall=true
  local ok=pcall(function()
   return self:act('base.feed',self.handle,{amount=self.script})
  end)
  self.state.retired_continued=ok
 end}}
"""

COORDINATOR_SOURCE = """return {kind='instance',actions={['base.feed']={right='use'}},
 on={['base.feed']=function(self,_,args)
  local script='s:'..tostring(args.amount)
  assert(self:act('engine.uninstall',self.handle,{script=script}).ok)
  assert(self:act('engine.install',self.handle,{script=script,choices={}}).ok)
  assert(self:act('base.recharge',self.handle,{amount=0}).ok)
  self.state.fresh_call_completed=true
 end}}
"""


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def prepare_plan(source, directory):
    plan = json.loads(source.read_text())
    for row in plan["profiles"]:
        row["key_file"] = str(directory / "keys" / f"{row['id']}.key")
    player = max(row["id"] for row in plan["profiles"]) + 1
    script_key, model_key, object_key = "test.action", "test.action_body", "test.action_player"
    plan["scripts"].append(
        dict(
            key=script_key,
            author=player,
            world_rule=False,
            source=SOURCE.replace("PLAYER", f"p:{player}"),
        )
    )
    model = copy.deepcopy(next(row for row in plan["models"] if row["key"] == "body"))
    model.update(key=model_key, script=script_key, author=player)
    model["text"] = dict(name="Action regression body", description="Ordinary authored test model.")
    plan["models"].append(model)
    body = copy.deepcopy(next(row for row in plan["objects"] if row["key"] == "resident.mira"))
    body.update(
        key=object_key,
        model=model_key,
        owner=player,
        maker=player,
        seed=False,
        at=[-15, -6, 0],
        preset=[],
    )
    plan["objects"].append(body)
    key = directory / "keys" / "action-player.key"
    plan["profiles"].append(
        dict(
            id=player,
            name="Action regression player",
            roles=[],
            modes=["play"],
            body=object_key,
            key_file=str(key),
        )
    )
    plan["balances"].append(dict(holder=f"p:{player}", amount=10))
    require(
        not any(row["issuer"] == player for row in plan["grants"]),
        "fresh action player must not receive hidden seed grants",
    )
    target = directory / "plan.json"
    target.write_text(json.dumps(plan, indent=2) + "\n")
    return player, key, target


def state(record, script):
    rows = next(row for row in record["script_states"] if row["script"] == script)["state"]
    return {row["key"]: row["value"] for row in rows}


async def nested_reinstallation(args, evidence, player, body):
    scripts = []
    for source in (OUTER_SOURCE, COORDINATOR_SOURCE):
        registered = await player.request("register_script", source=source)
        require(registered.get("ok"), "lifetime regression script registration failed")
        script = registered["script"]
        installed = await player.act("engine.install", body, script=script, choices=[])
        require(installed.get("ok"), "lifetime regression script installation failed")
        scripts.append(script)
    outer, coordinator = scripts
    for script, rights in ((outer, ["use"]), (coordinator, ["use", "edit"])):
        granted = await player.act(
            "engine.grant",
            holder=dict(script=script, on=body),
            scope=dict(object=body),
            rights=rights,
            expires_s=60,
        )
        require(granted.get("ok"), "lifetime regression explicit object grant failed")
    reply = await player.act("base.recharge", body, amount=1)
    inspected = await player.request("inspect", target=body)
    fresh = state(inspected, outer)
    helper = state(inspected, coordinator)
    require(
        fresh == {"fresh": True, "calls": 1} and helper.get("fresh_call_completed") is True,
        "retired outer invocation mutated its replacement state or the fresh nested call failed",
    )
    require(
        reply.get("effect") == "applied"
        and len(reply.get("script_errors", [])) == 1
        and "retired" in str(reply["script_errors"][0]),
        "the retired outer invocation resumed normally after uninstall/reinstall",
    )
    history = (await player.request("events", thing=body, detail=True, limit=1000))["events"]
    root = max(
        (
            row
            for row in history
            if row.get("kind") == "action"
            and row.get("name") == "base.recharge"
            and row.get("agent") is True
        ),
        key=lambda row: row["id"],
    )
    later = await player.act("base.recharge", body, amount=0)
    current = state(await player.request("inspect", target=body), outer)
    require(
        later.get("ok")
        and not later.get("script_errors")
        and current == {"fresh": True, "calls": 2},
        "retiring an old invocation poisoned the valid reinstalled script",
    )
    evidence.checks["nested_reinstall"] = dict(
        root=root["id"], outer=outer, coordinator=coordinator, reply=reply, state=fresh, later=later
    )


async def exercise(args, evidence, player_id, key):
    async with Player("fresh player", f"ws://127.0.0.1:{args.port}/ws", key, evidence) as player:
        body = player.welcome["body"]
        identity = await player.request("inspect", target=body)
        script = identity["scripts"][0]["script"]
        before = await player.act("engine.balance")
        require(before["balance"] == 10, "fresh player's actual wallet was not initialized")
        require(
            (await player.act("engine.list_grants"))["held"] == [], "fresh player has hidden grants"
        )

        async def attempt(expected_calls, child_ok):
            reply = await player.act("base.switch", body, on=True)
            require(
                reply.get("effect") == "applied" and not reply.get("script_errors"),
                "root action failed",
            )
            inspected = await player.request("inspect", target=body)
            current = state(inspected, script)
            require(
                current["calls"] == expected_calls
                and current["observed_calls"] == expected_calls
                and current["child_ok"] is child_ok
                and current["bad_input"] is True
                and current["pay_denied"] is True
                and current["returned"] is True,
                "actual Lua resumed before child completion or borrowed player permission",
            )
            history = (await player.request("events", thing=body, detail=True, limit=1000))[
                "events"
            ]
            root = max(
                (
                    row
                    for row in history
                    if row.get("kind") == "action"
                    and row.get("name") == "base.switch"
                    and row.get("actor") == f"p:{player_id}"
                ),
                key=lambda row: row["id"],
            )
            require(
                root["agent"] is True and root["cause"] is None and root["result"] == "applied",
                "live body-filtered feed lost the actual chosen action",
            )
            return dict(reply=reply, state=current, root=root["id"], child_ok=child_ok)

        denied = await attempt(1, False)
        granted = await player.act(
            "engine.grant",
            holder=dict(script=script, on=body),
            scope=dict(object=body),
            rights=["use"],
            expires_s=60,
        )
        require(granted.get("ok"), "owner could not explicitly grant its installed script Use")
        allowed = await attempt(3, True)
        require(
            (await player.act("engine.revoke", grant=granted["grant"])).get("ok"),
            "actual revoke failed",
        )
        revoked = await attempt(4, False)
        after = await player.act("engine.balance")
        require(
            after["balance"] == before["balance"], "script borrowed the player's Spend authority"
        )
        evidence.checks.update(
            player=f"p:{player_id}",
            body=body,
            script=script,
            denied_before_grant=denied,
            nested_after_grant=allowed,
            denied_after_revoke=revoked,
            wallet_before=before,
            wallet_after=after,
        )

        denied_build = await player.act("base.open", body)
        current = state(await player.request("inspect", target=body), script)
        require(
            not denied_build.get("created")
            and current["build_denied"] is True
            and current["tokens"] == 10
            and current.get("builds", 0) == 0,
            "denied nested Build allocated an object or spent authored stock",
        )
        async with Player(
            "plot owner", f"ws://127.0.0.1:{args.port}/ws", key.parent / "2.key", evidence
        ) as landlord:
            build = await landlord.act(
                "engine.grant",
                holder=dict(script=script, on=body),
                scope=dict(plot=1),
                rights=["build"],
                expires_s=60,
            )
            require(build.get("ok"), "landlord could not explicitly grant plot Build")
        construction = []
        for expected_builds in (1, 2):
            result = await player.act("base.open", body)
            current = state(await player.request("inspect", target=body), script)
            require(
                result.get("effect") == "applied"
                and len(result.get("created", [])) == 1
                and bool(result.get("script_errors")) == (expected_builds == 2)
                and current["builds"] == expected_builds
                and current["tokens"] == 10 - 4 * expected_builds
                and current["charge"] == current["initial_charge"] - 3 * expected_builds
                and current["created_mass"] > 0,
                "nested create failed to resume with its real handle or retain earlier Lua writes",
            )
            created = result["created"][0]
            product = await player.request("inspect", target=created)
            require(
                product.get("ok") and product["h"] == created,
                "a later handler failure retired the already-created native object",
            )
            construction.append(dict(reply=result, state=current, created=created))
        evidence.checks["construction"] = construction
        await nested_reinstallation(args, evidence, player, body)
        destroy = await player.act(
            "engine.grant",
            holder=dict(script=script, on=body),
            scope=dict(object=body),
            rights=["destroy"],
            expires_s=60,
        )
        require(destroy.get("ok"), "owner could not grant its installed script Destroy")
        retirement = await player.act("base.close", body)
        require(
            retirement.get("effect") == "applied" and retirement.get("script_errors"),
            "self-destruction was undone or resumed through a retired state row",
        )
        require(
            not (await player.request("inspect", target=body)).get("ok"),
            "actual self-destruction left the controlling object live",
        )
        evidence.checks["self_retirement"] = retirement


def validate_journal(path, evidence):
    with io.TextIOWrapper(zstandard.ZstdDecompressor().stream_reader(path.open("rb"))) as stream:
        records = [json.loads(line) for line in stream if line.strip()]
    checks = evidence.checks
    script = checks["script"]
    for name in ("denied_before_grant", "nested_after_grant", "denied_after_revoke"):
        attempt = checks[name]
        rows = [row for row in records if row.get("root") == attempt["root"]]
        root = next(row for row in rows if row["id"] == attempt["root"])
        child_ok = attempt["child_ok"]
        require(
            root["agent"] is True
            and root["cause"] is None
            and root["result"] == "applied"
            and sum(row.get("agent") is True for row in rows) == 1,
            "nested script/native attempts must not become extra chosen actions",
        )
        nested = [
            row
            for row in rows
            if row.get("kind") in {"action", "denial"} and row["id"] != root["id"]
        ]
        switch = [row for row in nested if row.get("name") == "base.switch"]
        require(
            len(nested) == 3
            and len(switch) == 1
            and switch[0]["result"] == ("applied" if child_ok else "denied")
            and all(row.get("actor") == script and row.get("agent") is False for row in nested),
            "real nested switch, malformed native and denied pay lost their actor/results",
        )
        require(
            all(
                row["kind"] == ("denial" if row["result"] == "denied" else "action")
                for row in nested
            ),
            "nested attempts must use denial records exactly when refused",
        )
        by_id = {row["id"]: row for row in rows}
        require(
            all(by_id[row["cause"]]["kind"] == "handler" for row in nested),
            "nested action causes must be the actual invoking handler records",
        )
        handlers = [row for row in rows if row.get("kind") == "handler"]
        require(
            len(handlers) == (2 if child_ok else 1),
            "denied child executed a handler or granted child did not",
        )
        bad = next(row for row in nested if row.get("response", {}).get("error") == "bad_input")
        require(
            bad["result"] == "denied"
            and bad["changed"] is False
            and bad["response"]["signature"].startswith("engine.pay("),
            "native schema failure must retain its signature and earlier parent effects",
        )
        attempt["records"] = rows
    errors = [row for row in records if row.get("status") in {"error", "cutoff"}]
    original_errors = [row for row in errors if row.get("actor") == script]
    lifetime = checks["nested_reinstall"]
    lifetime_errors = [row for row in errors if row.get("actor") == lifetime["outer"]]
    require(
        len(errors) == 3
        and len(original_errors) == 2
        and all(row.get("kind") == "handler" for row in errors)
        and any("after completed construction" in str(row) for row in original_errors)
        and any("retired" in str(row) for row in original_errors)
        and len(lifetime_errors) == 1
        and "retired" in str(lifetime_errors[0]),
        "intentional construction/retirement failures were lost or extra handler failures occurred",
    )
    lifetime_records = [row for row in records if row.get("root") == lifetime["root"]]
    require(
        [
            row["name"]
            for row in lifetime_records
            if row.get("kind") == "action" and row.get("agent") is False
        ]
        == ["base.feed", "engine.uninstall", "engine.install", "base.recharge"],
        "lifetime regression did not execute the real nested uninstall, reinstall and fresh call",
    )
    require(
        all(row["result"] == "applied" for row in lifetime_records if row.get("kind") == "action"),
        "fresh nested installation actions failed before the old invocation was retired",
    )
    creates = [
        row
        for row in records
        if row.get("kind") in {"action", "denial"} and row.get("name") == "engine.create"
    ]
    require(
        len(creates) == 3
        and [row["kind"] for row in creates] == ["denial", "action", "action"]
        and [row["result"] for row in creates] == ["denied", "applied", "applied"]
        and all(row.get("actor") == script and row.get("agent") is False for row in creates),
        "nested native construction attempts lost their true result, actor or nonchosen source",
    )
    checks["intentional_handler_errors"] = errors
    checks["journal_sha256"] = digest(path)


async def run(args, evidence):
    player, key, plan = prepare_plan(args.plan, evidence.directory)
    native_args = copy.copy(args)
    native_args.plan = plan
    native = Native(native_args, evidence.directory / "world", evidence.directory, "native")
    failed = False
    try:
        await native.start()
        await exercise(args, evidence, player, key)
    except Exception:
        failed = True
        raise
    finally:
        if native.process:
            try:
                state = await native.stop(script_errors=3)
            except Exception as problem:
                evidence.checks["shutdown_problem"] = str(problem)
                if not failed:
                    raise
            else:
                evidence.checks["native_shutdown"] = {
                    key: value for key, value in state.items() if key != "objects"
                }
    validate_journal(evidence.directory / "world" / "events.jsonl.zst", evidence)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--server", type=pathlib.Path, default=pathlib.Path(".build/debug/amb-server")
    )
    parser.add_argument(
        "--plan", type=pathlib.Path, default=pathlib.Path(".cache/content/seeded-plan.json")
    )
    parser.add_argument("--port", type=int, default=18081)
    parser.add_argument(
        "--output", type=pathlib.Path, default=pathlib.Path(".cache/integration/action-stack")
    )
    args = parser.parse_args()
    directory = args.output.resolve() / str(time.time_ns())
    evidence = Evidence(directory)
    report = dict(
        passed=False,
        artifacts=str(directory),
        executable_sha256=digest(args.server),
        source_plan_sha256=digest(args.plan),
    )
    try:
        asyncio.run(run(args, evidence))
        report["passed"] = True
    except Exception as problem:
        report["problem"] = str(problem)
    report["checks"] = evidence.checks
    report["elapsed_s"] = time.perf_counter() - evidence.started
    (directory / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    (directory / "transcript.json").write_text(json.dumps(evidence.events, indent=2) + "\n")
    print(json.dumps({key: value for key, value in report.items() if key != "checks"}))
    if not report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
