"""Actual authored-world stairs, optics, power chain and door sweep acceptance.

All state changes use authenticated player inputs. Avatar motion is ordinary
navigation; no position editing, fixture state injection or native debug mutation.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import math
import pathlib
import time

from game_elaboration import Evidence, Player, require
from game_restart_live import Native


class World:
    def __init__(self, evidence, summary, terminal):
        self.evidence, self.summary, self.terminal = evidence, summary, terminal
        self.objects = {}
        self.traces = {}

    async def look(self, player, limit=1000):
        observed = await player.request("look", limit=limit)
        for row in observed.get("things", []) + observed.get("far", []):
            self.objects.setdefault(row["h"], {}).update(row)
        return observed

    async def find(self, player, name):
        await self.look(player)
        found = [
            handle
            for handle, row in self.objects.items()
            if row.get("says", {}).get("name") == name
        ]
        require(found, f"actual observation has not exposed {name}")
        return found[0]

    async def motion(self, player, point, label, timeout):
        started = await player.act("base.move_to", point=point)
        require(started.get("effect") == "started", f"{label}: navigation did not start")
        intent = started["intent_id"]
        trace = self.traces[label] = []
        until = time.perf_counter() + timeout
        while time.perf_counter() < until:
            observed = await self.look(player, 1)
            own = observed["self"]
            trace.append({"step": observed["step"], "position": own["at"], "intent": own["intent"]})
            if own["intent"] is None:
                return observed, intent
            await asyncio.sleep(0.05)
        raise AssertionError(f"{label}: normal navigation timed out; actual trace retained")

    def terminal_event(self, player, intent, kind, label):
        if self.terminal:
            require(
                any(
                    row.get("kind") == kind and row.get("intent") == intent for row in player.events
                ),
                f"{label}: own-player {kind} push missing for completed intent",
            )

    async def move(self, player, point, label, timeout=60):
        observed, intent = await self.motion(player, point, label, timeout)
        require(
            math.dist(observed["self"]["at"], point) < 0.65,
            f"{label}: normal navigation stopped before its actual goal",
        )
        self.terminal_event(player, intent, "arrived", label)
        return observed

    async def fraction(self, player, door, predicate, label, timeout=10):
        until = time.perf_counter() + timeout
        while time.perf_counter() < until:
            row = await player.request("inspect", target=door)
            value = row["values"]["base.open_fraction"]
            if predicate(value):
                return value
            await asyncio.sleep(0.05)
        raise AssertionError(f"{label}: authored door did not reach requested fraction")


async def stairs(world, mira):
    door = await world.find(mira, "Sliding entrance")
    observed, intent = await world.motion(
        mira, [24, 57, 0], "closed entrance blocks navigation", 10
    )
    require(observed["self"]["at"][1] < 50.7, "avatar passed through closed physical glass leaf")
    world.terminal_event(mira, intent, "blocked", "closed entrance blocks navigation")
    await world.move(mira, [22, 50.3, 0], "entrance approach")
    require((await mira.act("base.open", door)).get("ok"), "actual entrance open denied")
    await world.fraction(mira, door, lambda x: x >= 0.99, "entrance opening")
    await world.move(mira, [24, 57, 0], "enter through actual doorway")
    stair = await world.find(mira, "Mezzanine stair")
    stair_row = await mira.request("inspect", target=stair)
    model = await mira.request("inspect", target=stair_row["model"])
    bounds = model["manifest"]["bounds"]
    await world.move(mira, [24, 65, 3.2], "ascend stairs onto mezzanine")
    trace = world.traces["ascend stairs onto mezzanine"]
    elevated = [row["position"] for row in trace if 0.4 < row["position"][2] < 2.8]
    require(len(elevated) >= 5, "stair ascent lacks physically sampled intermediate heights")
    lo, hi = bounds["min"], bounds["max"]
    origin = stair_row["position"]
    require(
        all(
            origin[0] + lo[0] - 0.4 <= p[0] <= origin[0] + hi[0] + 0.4
            and origin[1] + lo[1] - 0.4 <= p[1] <= origin[1] + hi[1] + 0.4
            for p in elevated
        ),
        "avatar ascended outside the actual baked stair footprint",
    )
    mezzanine = await world.find(mira, "Mezzanine deck")
    await world.move(mira, [24, 57, 0], "descend actual stairs")
    world.summary["stairs"] = {
        "passed": True,
        "stair": stair,
        "baked_bounds": bounds,
        "intermediate_samples": len(elevated),
        "max_height": max(row["position"][2] for row in trace),
    }
    return door, mezzanine


async def power(world, mira, sol, ivo, architect):
    breaker = await world.find(mira, "Breaker")
    pump = await world.find(mira, "Aeration pump")
    tank = await world.find(mira, "Living display")
    breaker_before = await sol.request("inspect", target=breaker)
    pump_before = await ivo.request("inspect", target=pump)
    sources = {}
    for player, original, filename, ratio in (
        (sol, breaker_before, "breaker", "0.95"),
        (ivo, pump_before, "pump", "0.9"),
    ):
        source = pathlib.Path(f"content/scripts/{filename}.luau").read_text()
        source = source.replace(
            "local function update(self)",
            "local function update(self)\n"
            "    self.state.authored_updates=(self.state.authored_updates or 0)+1",
            1,
        )
        if filename == "breaker":
            source = source.replace(
                "math.min(delivered, demand))", f"math.min(delivered, demand)*{ratio})"
            )
        else:
            source = source.replace("'aq.aeration', flow)", f"'aq.aeration', flow*{ratio})")
        registered = await player.request("register_script", source=source)
        require(registered.get("ok"), f"{filename}: actual author source registration failed")
        script = registered["script"]
        rebound = await architect.act("engine.model_script", model=original["model"], script=script)
        require(rebound.get("ok"), f"{filename}: model author could not adopt contributed script")
        metadata = await player.request("inspect", target=script)
        require(
            metadata["author"] == player.welcome["player"], "authored source credit was reassigned"
        )
        sources[filename] = {
            "script": script,
            "author": metadata["author"],
            "hash": metadata["hash"],
        }
    await world.move(sol, [30.6, 56, 0], "engineer approaches actual breaker")
    require((await sol.act("base.switch", breaker, on=True)).get("ok"), "breaker on denied")
    before = await ivo.request("inspect", target=tank)
    require(before["values"]["aq.aeration"] > 0, "powered authored pump has no actual aeration")
    require((await sol.act("base.switch", breaker, on=False)).get("ok"), "breaker off denied")
    off = await ivo.request("inspect", target=tank)
    require(
        abs(off["values"]["aq.aeration"]) < 1e-12,
        "power interruption did not withdraw pump aeration",
    )
    await asyncio.sleep(2)
    depleted = await ivo.request("inspect", target=tank)
    require(
        depleted["values"]["aq.oxygen"] < off["values"]["aq.oxygen"] - 1e-6,
        "actual closed-form oxygen failed to drain while unpowered",
    )
    require((await sol.act("base.switch", breaker, on=True)).get("ok"), "breaker restore denied")
    restored = await ivo.request("inspect", target=tank)
    require(restored["values"]["aq.aeration"] > 0, "pump aeration failed after power restoration")
    await asyncio.sleep(2)
    recovered = await ivo.request("inspect", target=tank)
    require(
        recovered["values"]["aq.oxygen"] > restored["values"]["aq.oxygen"] + 1e-6,
        "actual oxygen failed to recover after authored power chain restoration",
    )
    history = await ivo.request("events", thing=tank, detail=True, limit=1000)
    credited = [
        row
        for row in history["events"]
        if "p:3" in row.get("authors", []) and "p:4" in row.get("authors", [])
    ]
    require(credited, "actual causal records did not retain both independent script authors")
    world.summary["power_chain"] = {
        "passed": True,
        "breaker": breaker,
        "pump": pump,
        "tank": tank,
        "sources": sources,
        "powered_aeration": before["values"]["aq.aeration"],
        "off_aeration": off["values"]["aq.aeration"],
        "oxygen_off": [off["values"]["aq.oxygen"], depleted["values"]["aq.oxygen"]],
        "oxygen_on": [restored["values"]["aq.oxygen"], recovered["values"]["aq.oxygen"]],
        "multi_author_records": credited[-5:],
    }


async def optics(world, mira, door, mezzanine):
    await world.move(mira, [24, 49.8, 0], "outside glass entrance")
    require((await mira.act("base.close", door)).get("ok"), "close for glass observation denied")
    await world.fraction(mira, door, lambda x: x <= 0.01, "glass leaf closed")
    leaf = (await mira.request("inspect", target=door))["relations"]["base.attached_to"]["in"][0]
    observed = await world.look(mira)
    visible = {row["h"]: row for row in observed.get("things", [])}
    targets = [
        h
        for h, row in visible.items()
        if world.objects[h].get("says", {}).get("name") == "Ceramic cup"
        and leaf in row.get("through", [])
    ]
    require(targets, "actual object behind closed transparent entrance is not visible")
    target = targets[0]
    leaf_record = await mira.request("inspect", target=leaf)
    require(
        leaf_record["values"]["base.transparent"] > 0, "entrance leaf lacks measured transmission"
    )
    floor = await mira.request("inspect", target=mezzanine)
    require(floor["values"]["base.transparent"] == 0, "occluder material is not opaque")
    original = {"position": floor["position"], "rotation": floor["rotation"]}
    require(
        (await mira.act("engine.give", mezzanine, recipient="p:2")).get("ok"),
        "own slab collection failed",
    )
    placed = await mira.act(
        "engine.place_existing",
        mezzanine,
        position=[24, 50.4, 0],
        rotation=[math.sqrt(0.5), 0, 0, math.sqrt(0.5)],
    )
    require(placed.get("effect") == "applied", "ordinary existing-item opaque placement failed")
    opaque = await world.look(mira)
    hidden = target not in {row["h"] for row in opaque.get("things", []) + opaque.get("far", [])}
    farthest = max(
        (row["dist_m"] for row in opaque.get("things", []) + opaque.get("far", [])), default=0
    )
    require(
        hidden and (len(opaque["things"]) < 1000 or farthest > visible[target]["dist_m"] + 0.1),
        "opaque sight comparison was occluded or incomplete without sufficient scan coverage",
    )
    require(
        (await mira.act("engine.give", mezzanine, recipient="p:2")).get("ok"),
        "opaque slab retrieval failed",
    )
    require(
        (await mira.act("engine.place_existing", mezzanine, **original)).get("ok"),
        "original mezzanine restoration failed",
    )
    restored = await world.look(mira)
    require(
        target in {row["h"] for row in restored.get("things", [])},
        "removing opaque occluder did not restore actual sight",
    )
    world.summary["optics"] = {
        "passed": True,
        "target": target,
        "glass_leaf": leaf,
        "transmission": leaf_record["values"]["base.transparent"],
        "opaque_item": mezzanine,
        "opaque_returned_near": len(opaque["things"]),
        "requested_near_limit": 1000,
        "opaque_scan_farthest_m": farthest,
    }


async def obstruction(world, mira, door):
    await world.move(mira, [22, 50.3, 0], "door sweep approach")
    require((await mira.act("base.open", door)).get("ok"), "door opening before obstruction denied")
    await world.fraction(mira, door, lambda x: x >= 0.99, "door opens before obstruction")
    chair = await world.find(mira, "Café chair")
    require(
        (await mira.act("engine.give", chair, recipient="p:2")).get("ok"),
        "own chair collection failed",
    )
    require(
        (await mira.act("engine.place_existing", chair, position=[24, 51.15, 0])).get("ok"),
        "actual chair placement failed",
    )
    require((await mira.act("base.close", door)).get("ok"), "authored close action denied")
    await asyncio.sleep(2.5)
    first = await mira.request("inspect", target=door)
    fraction = first["values"]["base.open_fraction"]
    require(
        0.05 < fraction < 0.95, "actual chair did not stop sliding leaf at an intermediate pose"
    )
    await asyncio.sleep(0.6)
    second = await mira.request("inspect", target=door)
    require(
        abs(second["values"]["base.open_fraction"] - fraction) < 1e-6,
        "authored door kept moving after physical sweep obstruction",
    )
    heard = first.get("events", []) + second.get("events", [])
    heard += (await world.look(mira, 1)).get("events", [])
    require(
        "obstruct" in json.dumps(heard).lower(),
        "authored obstruction sound not observed through actual hearing",
    )
    require(
        (await mira.act("engine.give", chair, recipient="p:2")).get("ok"),
        "obstructing chair removal failed",
    )
    require(
        (await mira.act("base.close", door)).get("ok"), "retry closing after chair removal failed"
    )
    await world.fraction(mira, door, lambda x: x <= 0.01, "door closes after chair removal")
    world.summary["door_obstruction"] = {
        "passed": True,
        "chair": chair,
        "stopped_fraction": fraction,
        "sound": heard,
    }


async def run(args, artifacts, evidence, summary):
    native = Native(args, artifacts / "world", artifacts, "native")
    world = World(evidence, summary, args.require_terminal_events)
    try:
        await native.start()
        async with (
            Player("Mira", args.url, args.mira_key_file, evidence) as mira,
            Player("Sol", args.url, args.sol_key_file, evidence) as sol,
            Player("Ivo", args.url, args.ivo_key_file, evidence) as ivo,
            Player("Architect", args.url, args.architect_key_file, evidence) as architect,
        ):
            if "stairs" in args.gates:
                door, mezzanine = await stairs(world, mira)
            else:
                door = await world.find(mira, "Sliding entrance")
                mezzanine = None
            if "power" in args.gates:
                if "stairs" not in args.gates:
                    await world.move(mira, [22, 50.3, 0], "power entrance approach")
                    require(
                        (await mira.act("base.open", door)).get("ok"),
                        "actual entrance open for power gate denied",
                    )
                    await world.fraction(mira, door, lambda x: x >= 0.99, "power entrance opening")
                await power(world, mira, sol, ivo, architect)
            if "optics" in args.gates:
                mezzanine = mezzanine or await world.find(mira, "Mezzanine deck")
                await optics(world, mira, door, mezzanine)
            if "door" in args.gates:
                await obstruction(world, mira, door)
        state = await native.stop()
        summary.update({"passed": True, "final_step": state["step"], "script_errors": 0})
    finally:
        (artifacts / "navigation-traces.json").write_text(json.dumps(world.traces, indent=2) + "\n")
        if native.process and native.process.poll() is None:
            await native.stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--server", type=pathlib.Path, default=pathlib.Path(".build/release/amb-server")
    )
    parser.add_argument(
        "--plan", type=pathlib.Path, default=pathlib.Path(".cache/content/seeded-plan.json")
    )
    parser.add_argument("--port", type=int, default=18081)
    parser.add_argument("--mira-key-file", default=".runtime/keys/mira.key")
    parser.add_argument("--sol-key-file", default=".runtime/keys/sol.key")
    parser.add_argument("--ivo-key-file", default=".runtime/keys/ivo.key")
    parser.add_argument("--architect-key-file", default=".runtime/keys/architect.key")
    parser.add_argument("--require-terminal-events", action="store_true")
    parser.add_argument(
        "--gates",
        nargs="+",
        choices=["stairs", "power", "optics", "door"],
        default=["stairs", "power", "optics", "door"],
    )
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        default=pathlib.Path(".cache/integration/spatial-report.json"),
    )
    args = parser.parse_args()
    args.url = f"ws://127.0.0.1:{args.port}/ws"
    artifacts = args.output.parent / f"spatial-artifacts-{time.time_ns()}"
    evidence = Evidence(artifacts)
    summary = {
        "passed": False,
        "artifacts": str(artifacts),
        "terminal_pushes_required": args.require_terminal_events,
        "gates": args.gates,
    }
    try:
        asyncio.run(run(args, artifacts, evidence, summary))
    except Exception as problem:
        summary["problem"] = str(problem)
    args.output.write_text(json.dumps(summary, indent=2) + "\n")
    (artifacts / "transcript.json").write_text(json.dumps(evidence.events, indent=2) + "\n")
    print(
        json.dumps(
            {
                key: value
                for key, value in summary.items()
                if key not in {"power_chain", "optics", "door_obstruction"}
            }
        )
    )
    if not summary["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
