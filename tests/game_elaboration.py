"""Live S12 regression against the actual native game and paired fixture children.

Run on a fresh authored world. Private keys are consumed only for authentication;
hello packets and key paths are never included in evidence or diagnostics.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import math
import pathlib
import stat
import time

from websockets.asyncio.client import connect


def require(condition, message):
    if not condition:
        raise AssertionError(message)


class Evidence:
    def __init__(self, directory):
        self.directory = pathlib.Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.started = time.perf_counter()
        self.events = []
        self.checks = {}

    def note(self, player, direction, message):
        if message.get("t") in {"catalog", "metadata"}:
            message = {
                "t": message["t"],
                "counts": {
                    key: len(value) for key, value in message.items() if isinstance(value, list)
                },
            }
        self.events.append(
            {
                "at_s": round(time.perf_counter() - self.started, 6),
                "player": player,
                "direction": direction,
                "message": message,
            }
        )

    def save(self, status, problem=None):
        report = {
            "status": status,
            "checks": self.checks,
            "elapsed_s": round(time.perf_counter() - self.started, 3),
        }
        if problem:
            report["problem"] = problem
        (self.directory / "elaboration-report.json").write_text(json.dumps(report, indent=2) + "\n")
        (self.directory / "elaboration-transcript.json").write_text(
            json.dumps(self.events, indent=2) + "\n"
        )


class Player:
    def __init__(self, label, url, key_file, evidence):
        self.label, self.url, self.key_file, self.evidence = label, url, key_file, evidence
        self.next_id = 1
        self.pending = {}
        self.arrivals = []
        self.events = []
        self.reader = None

    async def __aenter__(self):
        path = pathlib.Path(self.key_file)
        require(stat.S_IMODE(path.stat().st_mode) == 0o600, "credential file must be private")
        key = path.read_text().strip()
        self.ws = await connect(self.url, max_size=None, compression=None, ping_interval=None)
        await self.ws.send(
            json.dumps({"t": "hello", "version": 1, "key": key, "mode": "play", "binary": False})
        )
        del key
        self.welcome = json.loads(await asyncio.wait_for(self.ws.recv(), 20))
        require(self.welcome.get("t") == "welcome", "native authentication did not welcome player")
        self.evidence.note(self.label, "received", self.welcome)
        self.reader = asyncio.create_task(self.receive())
        return self

    async def __aexit__(self, *_):
        await self.ws.close()
        if self.reader:
            with contextlib.suppress(asyncio.CancelledError):
                await self.reader

    async def receive(self):
        try:
            async for payload in self.ws:
                require(isinstance(payload, str), "binary-disabled player received binary frame")
                message = json.loads(payload)
                self.evidence.note(self.label, "received", message)
                require(message.get("t") != "bye", "native player session was closed or replaced")
                request = message.get("id")
                if request in self.pending:
                    future = self.pending.pop(request)
                    self.arrivals.append(request)
                    if not future.done():
                        future.set_result(message)
                elif message.get("t") == "event":
                    self.events.append(message)
        except Exception as problem:
            for future in self.pending.values():
                if not future.done():
                    future.set_exception(problem)
            raise

    async def send(self, operation, **fields):
        request = self.next_id
        self.next_id += 1
        message = {"t": operation, "id": request, **fields}
        future = asyncio.get_running_loop().create_future()
        self.pending[request] = future
        self.evidence.note(self.label, "sent", message)
        await self.ws.send(json.dumps(message))
        return request, future

    async def request(self, operation, **fields):
        _, future = await self.send(operation, **fields)
        return await asyncio.wait_for(future, 120)

    async def act(self, action, target=None, **args):
        fields = {"action": action, "args": args}
        if target:
            fields["target"] = target
        return await self.request("do", **fields)


def rule_source(vacuous):
    tests = []
    for enabled in [True] if vacuous else [True, False, True, False]:
        expected = 77 if enabled else 13
        initial = expected if vacuous else 0
        tests.append(
            "{given={subject='target',state={['base.charge']=" + str(initial) + "}},"
            "when={action={term='base.switch',target='target',args={on="
            + str(enabled).lower()
            + "}}},"
            "expect={subject='target',term='base.charge',equals=" + str(expected) + "}}"
        )
    return (
        "return {kind='rule',applies_to={material='base.wood'},tests={"
        + ",".join(tests)
        + "},on={['base.switch']=function(self,_,args) "
        "self:set('base.charge',args.on and 77 or 13) end}}"
    )


async def inspect_rule(player, script):
    inspected = await player.request("inspect", target=script)
    require(inspected.get("ok") is True, "registered candidate cannot be inspected")
    return inspected["declaration"]


async def enter_cafe(player, observation, evidence):
    rows = observation.get("things", []) + observation.get("far", [])
    doors = [row["h"] for row in rows if row.get("says", {}).get("name") == "Sliding entrance"]
    require(len(doors) == 1, "fresh observation must identify the actual entrance")
    door = doors[0]
    arrivals = []

    async def move(point):
        require((await player.act("base.move_to", point=point)).get("ok"), "approach move denied")
        until = time.perf_counter() + 20
        while time.perf_counter() < until:
            current = await player.request("look", limit=1)
            if current["self"]["intent"] is None:
                require(
                    math.dist(current["self"]["at"], point) < 0.65,
                    "ordinary navigation stopped before the S12 approach goal",
                )
                arrivals.append(current["self"]["at"])
                return
            await asyncio.sleep(0.05)
        raise AssertionError("ordinary navigation did not finish the S12 approach")

    await move([22, 50.3, 0])
    require((await player.act("base.open", door)).get("ok"), "actual entrance open denied")
    until = time.perf_counter() + 10
    while time.perf_counter() < until:
        if (await player.request("inspect", target=door))["values"]["base.open_fraction"] >= 0.99:
            break
        await asyncio.sleep(0.05)
    else:
        raise AssertionError("actual entrance did not open for the S12 approach")
    await move([24, 57, 0])
    evidence.checks["target_approach"] = {"door": door, "arrivals": arrivals}


async def run(args, evidence):
    async with (
        Player("Mira", args.url, args.mira_key_file, evidence) as mira,
        Player("Rue", args.url, args.rue_key_file, evidence) as rue,
    ):
        observed = await mira.request("look", limit=1000)
        floor = args.target
        if not floor:
            rows = observed.get("things", []) + observed.get("far", [])
            matches = [row["h"] for row in rows if row.get("says", {}).get("name") == "Café floor"]
            require(
                len(matches) == 1, "fresh live observation must identify exactly one Café floor"
            )
            floor = matches[0]
        target = await mira.request("inspect", target=floor)
        require(target.get("ok") is True, "live original target inspection failed")
        if not args.target:
            await enter_cafe(mira, observed, evidence)
        evidence.checks["target"] = floor
        evidence.checks["requester_body"] = mira.welcome["body"]
        gap = await mira.act("base.switch", floor, on=True)
        require(
            gap.get("reason") == "not_modeled" and gap.get("effect") == "none",
            "fresh wood-floor switch must produce an actual not-modeled result",
        )
        history = await mira.request("events", thing=floor, detail=True, limit=1000)
        source_gaps = [
            row
            for row in history["events"]
            if row.get("kind") == "gap" and row.get("name") == "base.switch"
        ]
        require(source_gaps, "actual not-modeled action did not record its derived source gap")
        require(
            all(row.get("agent") is False for row in source_gaps),
            "derived gaps must not create additional chosen agent events",
        )
        source_gap = source_gaps[-1]
        source_action = next(
            (row for row in history["events"] if row.get("id") == source_gap["cause"]), None
        )
        require(
            source_action
            and source_action.get("kind") == "action"
            and source_action.get("actor") == mira.welcome["player"]
            and source_action.get("agent") is True
            and source_action.get("result") == "not_modeled"
            and source_gap["root"] == source_action["root"] == source_action["id"],
            "source gap must retain the actual chosen player attempt as its cause and root",
        )
        evidence.checks["source_action_classification"] = {
            "action": source_action["id"],
            "gap": source_gap["id"],
            "root": source_gap["root"],
            "chosen_action": source_action["agent"],
            "chosen_gap": source_gap["agent"],
        }
        requested = await mira.act("engine.request_model", note="Live S12 switch regression")
        require(requested.get("ok") is True, "request_model failed to retain actual gap")
        request = requested["request"]
        listed = await rue.act("engine.requests", page_size=1000)
        original = next((row for row in listed.get("requests", []) if row["id"] == request), None)
        require(
            original and original["target"] == floor and original["requester"] == "p:2",
            "Rue list must retain original target and requesting player",
        )
        require(
            (await rue.act("engine.claim", request=request)).get("ok") is True,
            "actual elaborator claim failed",
        )
        evidence.checks["request"] = request
        evidence.checks["capture"] = original["capture"]

        rejected = await rue.request("register_script", source=rule_source(True))
        require(rejected.get("ok") is True, "vacuous candidate must compile as immutable source")
        rejected_id = rejected["script"]
        rejected_answer = await rue.act(
            "engine.answer", request=request, kind="rule", script=rejected_id
        )
        require(
            rejected_answer.get("ok") is False
            and rejected_answer.get("error") == "fixture_rejected",
            "vacuous candidate must be rejected by actual paired gameplay fixtures",
        )
        pairs = rejected_answer.get("fixture", {}).get("tests", [])
        require(
            pairs
            and all(pair["candidate"]["passed"] and pair["baseline"]["passed"] for pair in pairs),
            "rejection must demonstrate actual vacuous baseline",
        )
        inactive = await inspect_rule(rue, rejected_id)
        require(
            not inactive["approved"] and not inactive["active"],
            "rejected code must remain unapproved and inactive",
        )
        denied = await rue.act("engine.rule_activation", script=rejected_id, active=True)
        require(denied.get("ok") is False, "manual enable must not bypass fixture rejection")
        still_gap = await mira.act("base.switch", floor, on=True)
        require(still_gap.get("reason") == "not_modeled", "rejected rule changed live dispatch")
        evidence.checks["vacuous_rejected"] = {
            "script": rejected_id,
            "fixture": rejected_answer["fixture"],
        }

        accepted = await rue.request("register_script", source=rule_source(False))
        require(accepted.get("ok") is True, "actual good candidate source registration failed")
        script = accepted["script"]
        before = await inspect_rule(rue, script)
        require(
            not before["approved"] and not before["active"],
            "registration alone must not activate a world rule",
        )
        answer_id, answer_future = await rue.send(
            "do",
            action="engine.answer",
            args={"request": request, "kind": "rule", "script": script},
        )
        queued_id, queued_future = await rue.send("do", action="engine.balance", args={})
        query_id, query_future = await rue.send("inspect", target=script)
        started = time.perf_counter()
        other = await mira.act("engine.balance")
        other_elapsed = time.perf_counter() - started
        require(
            other.get("ok") is True and not answer_future.done(),
            "another player's actual input must complete while paired fixture answer is pending",
        )
        pending_declaration = (await asyncio.wait_for(query_future, 30))["declaration"]
        require(
            not answer_future.done()
            and not queued_future.done()
            and not pending_declaration["approved"]
            and not pending_declaration["active"],
            "read query must bypass parked mutation FIFO while rule remains inactive",
        )
        answer = await asyncio.wait_for(answer_future, 120)
        queued = await asyncio.wait_for(queued_future, 30)
        require(
            answer.get("ok") is True and queued.get("ok") is True,
            "verified answer and subsequent queued mutation must complete",
        )
        require(
            rue.arrivals.index(query_id)
            < rue.arrivals.index(answer_id)
            < rue.arrivals.index(queued_id),
            "same-session mutations overtook fixture completion",
        )
        pairs = answer["fixture"]["tests"]
        require(
            len(pairs) == 4
            and all(pair["candidate"]["passed"] for pair in pairs)
            and any(not pair["baseline"]["passed"] for pair in pairs)
            and all(not run.get("error") for pair in pairs for run in pair.values()),
            "actual gameplay candidate/baseline acceptance criterion was not met",
        )
        after = await inspect_rule(rue, script)
        require(after["approved"] and after["active"], "accepted candidate not activated live")
        for _ in range(20):
            if any(
                event.get("kind") == "answered" and event.get("request") == request
                for event in mira.events
            ):
                break
            await asyncio.sleep(0.025)
        require(
            any(
                event.get("kind") == "answered" and event.get("request") == request
                for event in mira.events
            ),
            "original requester missed answered notification",
        )
        retry = await mira.act("base.switch", floor, on=True)
        current = await mira.request("inspect", target=floor)
        require(
            retry.get("effect") == "applied" and current["values"]["base.charge"] == 77,
            "original failed action must now execute approved authored behavior on original target",
        )
        evidence.checks["accepted"] = {
            "script": script,
            "fixture": answer["fixture"],
            "activated": after["active"],
        }
        evidence.checks["ordering"] = {
            "answer_id": answer_id,
            "queued_mutation_id": queued_id,
            "bypass_query_id": query_id,
            "response_order": rue.arrivals,
            "other_player_rtt_ms": round(other_elapsed * 1000, 3),
            "other_player_completed_while_pending": True,
        }
        evidence.checks["notification"] = next(
            event
            for event in mira.events
            if event.get("kind") == "answered" and event.get("request") == request
        )
        evidence.checks["retry"] = {
            "effect": retry["effect"],
            "charge": current["values"]["base.charge"],
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="ws://127.0.0.1:18080/ws")
    parser.add_argument("--mira-key-file", default=".runtime/keys/mira.key")
    parser.add_argument("--rue-key-file", default=".runtime/keys/rue.key")
    parser.add_argument("--evidence", default=".cache/integration")
    parser.add_argument("--target", help="Optional complete handle instead of live floor discovery")
    args = parser.parse_args()
    evidence = Evidence(args.evidence)
    try:
        asyncio.run(run(args, evidence))
    except Exception as problem:
        evidence.save("failed", str(problem))
        raise SystemExit(f"Live S12 regression failed: {problem}") from None
    evidence.save("passed")
    print(
        json.dumps(
            {
                "status": "passed",
                "evidence": str(evidence.directory),
                "checks": list(evidence.checks),
                "elapsed_s": round(time.perf_counter() - evidence.started, 3),
            }
        )
    )


if __name__ == "__main__":
    main()
