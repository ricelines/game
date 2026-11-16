"""Actual live HTTP/CLI CAD variants and two-owner definition revision.

This is a manual integration regression, not autonomous model evidence. Credentials
are consumed only for authentication and never enter the retained transcript.
"""

import argparse
import asyncio
import hashlib
import json
import math
import sys
import time
from pathlib import Path

from aiohttp.test_utils import TestClient, TestServer
from net_live import Replica
from websockets.asyncio.client import connect

from bake.pool import BakePool
from gateway.authoring import Authoring
from gateway.credentials import read_key_file
from gateway.http import create_app
from gateway.session import Sessions

SOURCE = """from build123d import Align, Box, Compound
from time import sleep
PARAMS = {'width': (0.12, 0.08, 0.3)}
MATERIAL = 'base.wood'
PARTS = {'peg': {'solid': 1, 'origin': [0, 0, 0.12]}}
def build(p):
    sleep(2) # Controlled integration delay; geometry is still actually executed and baked.
    base = Box(p['width'], 0.12, 0.12, align=(Align.CENTER, Align.CENTER, Align.MIN))
    peg = Box(0.025, 0.025, 0.025, align=(Align.CENTER, Align.CENTER, Align.MIN))
    peg = peg.translate((0,0,0.12))
    return Compound(children=[base, peg])
"""
REVISION = """from build123d import Align, Box, Compound
from time import sleep
PARAMS = {'width': (0.14, 0.08, 0.3)}
MATERIAL = 'base.wood'
PARTS = {'peg': {'solid': 1, 'origin': [0, 0, 0.16]},
         'tip': {'solid': 2, 'origin': [0, 0, 0.185]}}
def build(p):
    sleep(2) # Controlled integration delay; geometry is still actually executed and baked.
    base = Box(p['width'], 0.12, 0.16, align=(Align.CENTER, Align.CENTER, Align.MIN))
    peg = Box(0.025, 0.025, 0.025, align=(Align.CENTER, Align.CENTER, Align.MIN))
    peg = peg.translate((0,0,0.16))
    tip = Box(0.02, 0.02, 0.02, align=(Align.CENTER, Align.CENTER, Align.MIN))
    tip = tip.translate((0,0,0.185))
    return Compound(children=[base, peg, tip])
"""


MASS_WATCHER = """return {kind='model', watches={'base.mass'}, on={
 ['base.mass']=function(self)
   self.state.mass_notices=(self.state.mass_notices or 0)+1
   self.state.observed_mass=self:get('base.mass')
 end,
}}
"""


def script_state(record, script):
    rows = next(row for row in record["script_states"] if row["script"] == script)["state"]
    return {row["key"]: row["value"] for row in rows}


class Watcher:
    """Actual independent S6 decoder, never an observation/geometry fixture."""

    def __init__(self, url, key_file):
        self.url, self.key_file = url, key_file
        self.replica = Replica()
        self.bytes = 0
        self.pending = {}
        self.next_query = 0

    async def open(self):
        self.socket = await connect(self.url, max_size=None)
        await self.socket.send(
            json.dumps(
                {
                    "t": "hello",
                    "version": 1,
                    "key": read_key_file(self.key_file),
                    "mode": "debug",
                    "binary": True,
                }
            )
        )
        await self.socket.send(json.dumps({"t": "view", "pos": [0, 0, 5]}))
        self.reader = asyncio.create_task(self.receive())
        return self

    async def receive(self):
        async for raw in self.socket:
            if isinstance(raw, bytes):
                self.bytes += len(raw)
                self.replica.apply(raw)
            else:
                message = json.loads(raw)
                request = self.pending.pop(message.get("id"), None)
                if request:
                    request.set_result(message)

    async def query(self, kind, **fields):
        self.next_query += 1
        result = asyncio.get_running_loop().create_future()
        self.pending[self.next_query] = result
        await self.socket.send(json.dumps({"t": kind, "id": self.next_query, **fields}))
        return await asyncio.wait_for(result, 15)

    async def expect(self, objects):
        until = time.monotonic() + 15
        wanted = {}
        for row in objects:
            slot, generation = map(int, row["h"][2:].split("."))
            wanted[(generation << 32) | slot] = row["measured"]["asset"]
        while time.monotonic() < until:
            if self.reader.done():
                self.reader.result()
            received = {
                row["handle"]: row["asset"].hex()
                for cell in self.replica.cells.values()
                for row in cell.values()
            }
            if all(received.get(handle) == asset for handle, asset in wanted.items()):
                return {"matched_roots": len(wanted), "bytes": self.bytes}
            await asyncio.sleep(0.025)
        raise AssertionError("Connected actual watcher missed an authoritative asset update")

    async def close(self):
        await self.socket.close()
        await self.reader


def require(value, message):
    if not value:
        raise AssertionError(message)


class Journey:
    def __init__(self, client, output, keys, sessions):
        self.client, self.output, self.keys, self.sessions = client, output, keys, sessions
        self.report = {
            "kind": "manual actual native/gateway/bake integration",
            "requests": [],
            "checks": {},
            "status": "running",
        }
        output.parent.mkdir(parents=True, exist_ok=True)

    def checkpoint(self):
        path = self.output.with_suffix(".partial")
        path.write_text(json.dumps(self.report, indent=2) + "\n")
        path.replace(self.output)

    async def request(self, actor, path, body=None, *, method="POST"):
        headers = {
            "Authorization": "Bearer " + self.keys[actor],
            "X-Ambergris-Session": "live-authoring-" + actor,
        }
        async with self.client.request(method, path, json=body, headers=headers) as response:
            result = await response.json()
            self.report["requests"].append(
                {
                    "actor": actor,
                    "path": path,
                    "request": body,
                    "status": response.status,
                    "response": result,
                }
            )
            self.checkpoint()
            return result

    async def tool(self, actor, name, **arguments):
        if name == "look":
            # This manual geometry regression needs full rows to compare assets;
            # session compaction is exercised separately by gateway tests.
            session = await self.sessions.get(self.keys[actor], "live-authoring-" + actor)
            session.forget_observation()
        return await self.request(actor, "/tools/" + name, arguments)

    async def act(self, actor, action, *, target=None, **arguments):
        envelope = {"action": action, "args": arguments}
        if target:
            envelope["target"] = target
        return await self.tool(actor, "do", **envelope)

    async def approach(self, actor, target, point=None):
        if point is None:
            goods = await self.tool(actor, "inspect", target=target)
            point = list(goods["position"])
            point[1] -= 0.6
            point[2] = 0
        started = await self.act(actor, "base.move_to", point=point)
        require(started.get("effect") == "started", "normal approach did not start")
        until = time.monotonic() + 60
        while time.monotonic() < until:
            observed = await self.tool(actor, "look", limit=1)
            if observed["self"]["intent"] is None:
                goods = await self.tool(actor, "inspect", target=target)
                require(
                    math.dist(observed["self"]["at"], goods["position"]) < 1.6,
                    "normal approach stopped outside edit reach",
                )
                return
            await asyncio.sleep(0.1)
        raise AssertionError("normal approach timed out")

    async def open_entrance(self):
        # This authored seed fixture requires walking through its ordinary door.
        observed = await self.tool("author", "look", limit=500)
        doors = [
            row["h"]
            for row in observed["things"]
            if row.get("says", {}).get("name") == "Sliding entrance"
        ]
        require(len(doors) == 1, "actual LOOK did not expose the seed entrance")
        await self.approach("author", doors[0], [0, -2.7, 0])
        opened = await self.act("author", "base.open", target=doors[0])
        require(not opened.get("script_errors"), "normal entrance opening failed")
        until = time.monotonic() + 10
        while time.monotonic() < until:
            door = await self.tool("author", "inspect", target=doors[0])
            if door["values"]["base.open_fraction"] > 0.99:
                return
            await asyncio.sleep(0.1)
        raise AssertionError("actual entrance did not open")

    async def job(self, actor, path, body, while_pending=None):
        result = await self.request(actor, path, body)
        require("job" in result, "actual authoring submission did not return a job")
        if while_pending:
            status = await self.request(actor, "/bake/jobs/" + result["job"], method="GET")
            require(
                status["status"] in {"queued", "baking"}, "controlled actual bake was not pending"
            )
            await while_pending()
            status = await self.request(actor, "/bake/jobs/" + result["job"], method="GET")
            require(
                status["status"] in {"queued", "baking"},
                "ordinary inputs did not finish while the actual bake was in flight",
            )
        until = time.monotonic() + 120
        while time.monotonic() < until:
            result = await self.request(actor, "/bake/jobs/" + result["job"], method="GET")
            if result["status"] not in {"queued", "baking", "registering"}:
                return result
            await asyncio.sleep(0.05)
        raise AssertionError("actual sandbox/native registration timed out")

    async def place(self, actor, model, params=None):
        plots = (await self.tool(actor, "inspect", target="land"))["land"]
        owner = (await self.tool(actor, "inspect", target="self"))["owner"]
        bounds = next(plot["bounds"] for plot in plots if plot["owner"] == owner)
        position = [bounds[0] + 0.6, bounds[1] + 0.6, 0.05]
        arguments = {"model": model, "position": position}
        if params is not None:
            arguments["params"] = params
        result = await self.act(actor, "base.place", **arguments)
        require(result.get("ok") and result.get("created"), "normal authored placement failed")
        created = [await self.tool(actor, "inspect", target=h) for h in result["created"]]
        roots = [row for row in created if row.get("parent") is None and row.get("model") == model]
        require(len(roots) == 1, "ordinary creation did not return one authoritative root")
        return roots[0]["h"], created


async def run(options):
    marker = "\n# Fresh manual integration run " + str(time.time_ns()) + "\n"
    source, revision = SOURCE + marker, REVISION + marker
    keys = {"author": read_key_file(options.author_key), "other": read_key_file(options.other_key)}
    pool = await asyncio.to_thread(BakePool, options.assets, require_cgroup=False)
    sessions = Sessions(options.world)
    authoring = Authoring(pool)
    async with TestClient(TestServer(create_app(sessions, authoring))) as client:
        test = Journey(client, options.output, keys, sessions)
        watcher = await Watcher(options.world, options.watch_key).open()
        try:
            broken = (
                "from build123d import Box\n"
                "def build(p):\n    raise ValueError('Actual restricted CAD failure proof')\n"
                + marker
            )
            failed = await test.job(
                "author",
                "/bake/models",
                {
                    "source": broken,
                    "text": {"name": "Failure proof", "description": "Not a fabricated model"},
                },
            )
            require(failed["status"] == "failed", "actual restricted CAD unexpectedly succeeded")
            require(failed.get("record_id"), "actual CAD failure has no native record receipt")
            receipt = failed["record_id"]
            actual = await watcher.query("events", since=receipt - 1, limit=100)
            recorded = [row for row in actual["events"] if row.get("job") == failed["job"]]
            require(len(recorded) == 1, "failed job did not produce exactly one actual S7 record")
            event = recorded[0]
            require(
                event["id"] == receipt
                and event["of"] == "model"
                and event["result"] == "denied"
                and event["changed"] is False
                and event["agent"] is True
                and event["actor"] == "p:2"
                and event["source_hash"] == hashlib.sha256(broken.encode()).hexdigest()
                and "model" not in event
                and "script" not in event,
                "actual failed CAD event invented an identity or lost chosen-attempt attribution",
            )
            test.report["failed_cad_publication"] = {"job": failed, "event": event}
            invalid_actions = [
                ("base.make_material", "self", {"material": "base.wood", "kg": "wrong"}),
                ("novel.missing_action", "self", {"value": 123}),
                ("base.make_material", "o:broken", {"material": "base.wood", "kg": 0.001}),
            ]
            denied = []
            for action, target, args in invalid_actions:
                prior = await watcher.query("events", limit=1)
                since = max((row["id"] for row in prior["events"]), default=0)
                reply = await test.tool("author", "do", action=action, target=target, args=args)
                require(
                    reply.get("error") == "bad_input" and reply.get("problem"),
                    "accepted malformed action did not return its concrete problem",
                )
                actual = await watcher.query("events", since=since, limit=100)
                attempts = [
                    row
                    for row in actual["events"]
                    if row.get("actor") == "p:2" and row.get("name") == action
                ]
                require(len(attempts) == 1, "accepted malformed DO was omitted or double-recorded")
                attempt = attempts[0]
                require(
                    attempt["result"] == "denied"
                    and attempt["changed"] is False
                    and attempt["agent"] is True
                    and attempt["args"] == args
                    and attempt["raw_target"] == target,
                    "malformed DO lost the actual chosen request or changed world state",
                )
                denied.append({"response": reply, "event": attempt})
            test.report["accepted_malformed_actions"] = denied
            if options.failed_cad_only:
                test.report["checks"] = {
                    "failed_cad_publication": True,
                    "accepted_malformed_actions": len(denied),
                    "sandbox": pool.sandbox,
                }
                test.report["status"] = "passed"
                return

            async def ordinary_inputs():
                observed, changed = await asyncio.gather(
                    test.tool("author", "look"),
                    test.act("other", "base.make_material", material="base.wood", kg=0.001),
                )
                require(
                    observed.get("things") is not None and changed.get("ok"),
                    "unrelated ordinary queries/actions stalled or failed during actual CAD",
                )
                test.report["ordinary_inputs_while_baking"] = {
                    "step": observed.get("step"),
                    "action": changed,
                }

            initial = await test.job(
                "author",
                "/bake/models",
                {
                    "source": source,
                    "params": {},
                    "text": {
                        "name": "Live parametric assembly",
                        "description": "Manual integration artifact, not autonomous invention.",
                    },
                },
                while_pending=ordinary_inputs,
            )
            require(initial.get("status") == "done", "real base CAD registration failed")
            model = initial["model"]
            uploaded = await test.request("author", "/bake/scripts", {"source": MASS_WATCHER})
            require(uploaded.get("ok"), "actual measured-mass watcher upload failed")
            script = uploaded["script"]
            require(
                (await test.act("author", "engine.model_script", model=model, script=script)).get(
                    "ok"
                ),
                "normal author model-script installation failed",
            )
            variant = await test.job(
                "author", "/bake/variants", {"model": model, "params": {"width": 0.2}}
            )
            require(
                variant.get("status") == "done" and variant.get("params", 0) > 0,
                "existing model did not acquire a nonzero ready parameter block",
            )
            require(
                variant["model"] == model, "variant registration created a new root model identity"
            )
            await test.open_entrance()
            first, first_assembly = await test.place("author", model)
            second, second_assembly = await test.place("other", model, variant["params"])
            await asyncio.gather(test.approach("author", first), test.approach("other", second))
            require(
                (
                    await test.act(
                        "author",
                        "base.reconfigure",
                        target=first,
                        model=model,
                        params=variant["params"],
                    )
                ).get("ok"),
                "ordinary reconfiguration to the ready block failed",
            )
            changed = await test.tool("author", "inspect", target=first)
            require(
                math.isclose(changed["measured"]["params"]["width"], 0.2),
                "reconfiguration did not change actual measured geometry",
            )
            require(
                (
                    await test.act(
                        "author", "base.reconfigure", target=first, model=model, params=0
                    )
                ).get("ok"),
                "ordinary return to model defaults failed",
            )
            await asyncio.sleep(1)
            before = [await test.tool("author", "inspect", target=h) for h in [first, second]]
            require(
                before[0]["owner"] != before[1]["owner"], "test roots do not have distinct owners"
            )
            require(
                before[0]["measured"]["params"] != before[1]["measured"]["params"],
                "test roots do not exercise two actual parameter sets",
            )
            baseline_states = [
                script_state(before[0], script),
                script_state(await test.tool("other", "inspect", target=second), script),
            ]
            streamed_before = await watcher.expect(before)

            async def concurrent_reconfigure():
                changed = await asyncio.gather(
                    test.act(
                        "author",
                        "base.reconfigure",
                        target=first,
                        model=model,
                        params=variant["params"],
                    ),
                    test.act("other", "base.reconfigure", target=second, model=model, params=0),
                )
                require(
                    all(row.get("ok") for row in changed),
                    "independent owner reconfigurations failed during definition bake",
                )
                returned = await asyncio.gather(
                    test.act("author", "base.reconfigure", target=first, model=model, params=0),
                    test.act(
                        "other",
                        "base.reconfigure",
                        target=second,
                        model=model,
                        params=variant["params"],
                    ),
                )
                require(
                    all(row.get("ok") for row in returned),
                    "independent owner reconfiguration returns failed during definition bake",
                )
                test.report["concurrent_reconfigure_while_edit_baking"] = changed + returned

            staging_since = (await watcher.query("events", limit=1))["newest"]
            proposed = await test.job(
                "author",
                "/bake/edits",
                {"model": model, "source": revision},
                while_pending=concurrent_reconfigure,
            )
            require(proposed.get("effect") == "proposal_ready", "actual proposal staging failed")
            require(
                proposed["progress"] == {"completed": 2, "total": 2},
                "definition edit did not bake exactly the two distinct current parameter sets",
            )
            staged = await test.tool("author", "inspect", target=proposed["proposal"])
            require(
                staged["source_hash"] == hashlib.sha256(revision.encode()).hexdigest()
                and json.loads(staged["canonical_params"]) == {"width": 0.14}
                and len(staged["variants"]) == 1
                and json.loads(staged["variants"][0]["canonical_params"]) == {"width": 0.2},
                "completed edit staging exposed a proposal before every required variant was ready",
            )
            staged_events = await watcher.query("events", since=staging_since, limit=4096)
            require(
                not staged_events["truncated"] and staged_events["events_omitted"] == 0,
                "actual S7 retention cannot prove the complete edit-staging choice count",
            )
            registrations = [
                event
                for event in staged_events["events"]
                if event.get("model") == proposed["proposal"]
                and event.get("name") in {"register_model", "register_variant"}
            ]
            require(len(registrations) == 1, "one edit staged multiple native registration choices")
            registration = registrations[0]
            require(
                registration["id"] == proposed["record_id"]
                and registration["kind"] == "create"
                and registration["of"] == "model"
                and registration.get("agent", True) is True
                and registration["actor"] == "p:2"
                and registration["name"] == "register_model"
                and registration["changed"] is True
                and registration["result"] == "applied"
                and registration["cause"] is None
                and registration["root"] == registration["id"],
                "batched edit staging lost its single actual chosen create or native receipt",
            )
            test.report["edit_staging_choice"] = {
                "since": staging_since,
                "proposal": staged,
                "registration": registration,
                "registrations": len(registrations),
            }
            revision_args = {
                "model": model,
                "proposal": proposed["proposal"],
            }
            denied = await test.act("other", "base.revise_model", **revision_args)
            require(
                denied.get("ok") is False and denied.get("denied", {}).get("need") == "edit",
                "another player revised a foreign model definition",
            )
            still = [await test.tool("author", "inspect", target=h) for h in [first, second]]
            require(
                [row["measured"]["asset"] for row in still]
                == [row["measured"]["asset"] for row in before],
                "denied edit partially changed instances",
            )
            applied = await test.act("author", "base.revise_model", **revision_args)
            require(
                applied.get("effect") == "applied",
                "actual author's ordinary revision action failed",
            )
            after = [await test.tool("author", "inspect", target=h) for h in [first, second]]
            for left, right in zip(before, after, strict=True):
                require(
                    right["h"] == left["h"] and right["model"] == model,
                    "definition revision replaced stable root/model identity",
                )
                require(
                    right["owner"] == left["owner"], "definition revision borrowed or changed owner"
                )
                require(
                    right["measured"]["asset"] != left["measured"]["asset"],
                    "definition edit failed to update every actual instance",
                )
                expected_params = (
                    {"width": 0.14} if left["h"] == first else left["measured"]["params"]
                )
                require(
                    right["measured"]["params"] == expected_params,
                    "default0 did not adopt new defaults or an explicit parameter value changed",
                )
                require(
                    math.dist(right["position"], left["position"]) < 0.03,
                    "definition revision did not preserve settled root pose",
                )
            watched = [
                script_state(after[0], script),
                script_state(await test.tool("other", "inspect", target=second), script),
            ]
            for before_state, after_state, current in zip(
                baseline_states, watched, after, strict=True
            ):
                require(
                    after_state.get("mass_notices", 0) >= before_state.get("mass_notices", 0) + 3,
                    "measured-mass watcher missed concurrent reconfigure/revision changes",
                )
                require(
                    math.isclose(
                        after_state["observed_mass"], current["values"]["base.mass"], rel_tol=1e-6
                    ),
                    "watcher did not read current geometry's actual measured mass",
                )
            streamed_after = await watcher.expect(after)
            committed_step = (await test.tool("author", "look", limit=1))["step"]
            repeated = await test.act("author", "base.revise_model", **revision_args)
            require(
                repeated.get("ok") is True and repeated.get("reason") == "no_effect",
                "repeating the current definition should have no effect",
            )
            definition = await test.tool("author", "inspect", target=model)
            require(
                definition["source_hash"] == hashlib.sha256(revision.encode()).hexdigest(),
                "public current definition retained the old source",
            )
            cli = await asyncio.create_subprocess_exec(
                sys.executable,
                "-m",
                "gateway.cli",
                "--gateway",
                str(client.make_url("/")),
                "--key-file",
                str(options.author_key),
                "--session",
                "live-authoring-author",
                "inspect",
                model,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            raw, diagnostics = await cli.communicate()
            require(cli.returncode == 0, "actual CLI inspection failed: " + diagnostics.decode())
            cli_reply = json.loads(raw)
            require(
                cli_reply.get("source_hash") == definition["source_hash"],
                "CLI did not observe the actual committed definition",
            )
            retained_parts = []
            for assembly in (first_assembly, second_assembly):
                for child in (row for row in assembly if row.get("parent") is not None):
                    current = await test.tool("author", "inspect", target=child["h"])
                    require(
                        current.get("ok") and current["parent"] == child["parent"],
                        "matching named child handle was not preserved",
                    )
                    retained_parts.append(current)
            require(len(retained_parts) == 2, "fixture did not exercise two retained named parts")
            bad = revision.replace("0.08, 0.3", "0.08, 0.19")
            failed = await test.job("author", "/bake/edits", {"model": model, "source": bad})
            require(
                failed["status"] == "failed", "invalid required parameter set was silently clamped"
            )
            unchanged = await test.tool("author", "inspect", target=model)
            require(
                unchanged["source_hash"] == definition["source_hash"],
                "failed required CAD execution changed the live definition",
            )
            test.report["checks"] = {
                "committed_step": committed_step,
                "watcher_script": script,
                "watch_states_before": baseline_states,
                "watch_states_after": watched,
                "connected_S6_before": streamed_before,
                "connected_S6_after": streamed_after,
                "ready_variant": variant["params"],
                "model": model,
                "two_owners": [row["owner"] for row in after],
                "two_parameter_sets": True,
                "new_source_default_adopted": True,
                "retained_named_parts": retained_parts,
                "ordinary_reconfigure": True,
                "ordinary_revision": True,
                "foreign_author_denied": True,
                "repeated_definition_no_effect": True,
                "invalid_required_set_no_commit": True,
                "actual_cli_current_definition": cli_reply["source_hash"],
                "before": before,
                "after": after,
                "initial_created_assemblies": [first_assembly, second_assembly],
                "revision_result": applied,
                "sandbox": pool.sandbox,
            }
            test.report["status"] = "passed"
        except Exception as error:
            test.report.update({"status": "failed", "problem": str(error)})
            raise
        finally:
            test.checkpoint()
            await watcher.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--world", default="ws://127.0.0.1:18080/ws")
    parser.add_argument("--author-key", type=Path, default=Path(".runtime/keys/mira.key"))
    parser.add_argument("--other-key", type=Path, default=Path(".runtime/keys/sol.key"))
    parser.add_argument("--watch-key", type=Path, default=Path(".runtime/keys/spectator.key"))
    parser.add_argument("--assets", type=Path, default=Path("assets"))
    parser.add_argument("--failed-cad-only", action="store_true")
    parser.add_argument(
        "--output", type=Path, default=Path(".cache/integration/live-authoring-report.json")
    )
    options = parser.parse_args()
    try:
        asyncio.run(run(options))
    except Exception as error:
        print(json.dumps({"passed": False, "problem": str(error)}))
        return 1
    print(json.dumps({"passed": True, "evidence": str(options.output)}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
