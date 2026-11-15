"""Real socket-authored navigation, compound custody, trade and rule recovery gate.

Clone a completed immutable snapshot/catalog from the live elaboration gate; no
private keys are copied. Only this script's separate native processes are stopped.
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import math
import pathlib
import shutil
import signal
import socket
import subprocess
import time

from game_elaboration import Evidence, Player, inspect_rule, require


def listening(port):
    with socket.socket() as connection:
        connection.settimeout(0.1)
        return connection.connect_ex(("127.0.0.1", port)) == 0


class Native:
    def __init__(self, args, data, artifacts, label):
        self.args, self.data, self.label = args, data, label
        self.directory = artifacts / label
        self.directory.mkdir()
        self.report = self.directory / "state.json"
        self.logs = []
        self.process = None

    async def start(self):
        require(not listening(self.args.port), "separate restart test port is already occupied")
        command = [
            str(self.args.server.resolve()),
            "run",
            "--plan",
            str(self.args.plan.resolve()),
            "--data",
            str(self.data.resolve()),
            "--port",
            str(self.args.port),
            "--steps",
            "1000000",
            "--snapshot-at-end",
            "--report",
            str(self.report.resolve()),
        ]
        (self.directory / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        provenance = {}
        for name, source in (("executable", self.args.server), ("plan", self.args.plan)):
            with source.open("rb") as stream:
                provenance[name] = hashlib.file_digest(stream, "sha256").hexdigest()
        (self.directory / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
        self.logs = [(self.directory / name).open("w") for name in ("stdout.txt", "stderr.txt")]
        self.process = subprocess.Popen(command, stdout=self.logs[0], stderr=self.logs[1])
        until = time.perf_counter() + 60
        while time.perf_counter() < until:
            require(
                self.process.poll() is None, f"{self.label} native startup failed; logs retained"
            )
            if listening(self.args.port):
                return
            await asyncio.sleep(0.05)
        raise AssertionError("separate native restart listener did not become ready")

    async def stop(self):
        if self.process is None:
            return None
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                await asyncio.to_thread(self.process.wait, 30)
            except subprocess.TimeoutExpired:
                self.process.kill()
                await asyncio.to_thread(self.process.wait)
                raise AssertionError(
                    "owned native process failed graceful snapshot shutdown"
                ) from None
        for log in self.logs:
            log.close()
        require(self.process.returncode == 0, f"{self.label} native exit failed; logs retained")
        require(self.report.exists(), f"{self.label} native did not retain its final state report")
        state = json.loads(self.report.read_text())
        require(
            state["script_errors"] == 0,
            f"{self.label} had {state['script_errors']} handler errors/cutoffs; reports retained",
        )
        return state


async def wait_arrival(player):
    until = time.perf_counter() + 15
    while time.perf_counter() < until:
        observed = await player.request("look", limit=1)
        if observed["self"]["intent"] is None:
            return observed
        await asyncio.sleep(0.1)
    raise AssertionError("normal navigation did not reach entrance approach point")


def stable(row):
    return {key: row[key] for key in ("h", "model", "maker", "created_s", "owner", "parent")}


async def run(args, evidence, artifacts, summary):
    source = args.source_data
    snapshots = list((source / "snapshots").glob("snap-*.amb.zst"))
    require(snapshots, "live source world has no completed immutable snapshot")
    snapshot = max(snapshots, key=lambda p: int(p.name.split("-")[1].split(".")[0]))
    data = artifacts / "world"
    (data / "snapshots").mkdir(parents=True)
    shutil.copy2(snapshot, data / "snapshots" / snapshot.name)
    for name in ("scripts", "registry"):
        shutil.copytree(source / name, data / name)
    accepted = json.loads(args.elaboration_report.read_text())["checks"]["accepted"]
    script = accepted["script"]
    summary["source_snapshot"] = str(snapshot)
    summary["immutable_rule"] = script
    url = f"ws://127.0.0.1:{args.port}/ws"
    first = Native(args, data, artifacts, "first")
    second = Native(args, data, artifacts, "resumed")
    try:
        await first.start()
        async with (
            Player("Mira", url, args.mira_key_file, evidence) as mira,
            Player("Sol", url, args.sol_key_file, evidence) as sol,
        ):
            observed = await mira.request("look", limit=1000)
            rows = observed.get("things", []) + observed.get("far", [])
            door = next(
                row["h"] for row in rows if row.get("says", {}).get("name") == "Sliding entrance"
            )
            chairs = [row["h"] for row in rows if row.get("says", {}).get("name") == "Café chair"]
            owned = []
            for handle in chairs:
                inspected = await mira.request("inspect", target=handle)
                if inspected["owner"] == mira.welcome["player"]:
                    owned.append(handle)
            require(len(owned) >= 2, "live observation needs two distinct currently owned items")
            inventory_item, traded_item = owned[:2]
            rule_before = await inspect_rule(mira, script)
            require(
                rule_before["approved"] and rule_before["active"],
                "source snapshot must contain fixture-approved active immutable rule",
            )
            moved = await mira.act("base.move_to", point=[-2, -2.7, 0])
            require(moved.get("effect") == "started", "authored body navigation failed to start")
            await wait_arrival(mira)
            held = await mira.act("base.hold", door)
            require(held.get("effect") == "applied", "actual two-part entrance grasp failed")
            door_before = await mira.request("inspect", target=door)
            parts = door_before["relations"]["base.attached_to"]["in"]
            require(parts, "held object must have actual declared child parts")
            child_before = await mira.request("inspect", target=parts[0])
            require(
                (await mira.act("engine.give", inventory_item, recipient="p:2")).get("ok"),
                "normal self-give failed to collect hidden inventory",
            )
            inventory_before = await mira.request("inspect", target=inventory_item)
            opened = await mira.act("engine.trade_open", other="p:3")
            require(opened.get("ok"), "present players could not open actual trade")
            trade = opened["trade"]
            require(
                (
                    await mira.act("engine.trade_change", trade=trade, money=5, goods=traded_item)
                ).get("ok"),
                "real goods escrow failed",
            )
            require(
                (await sol.act("engine.trade_change", trade=trade, money=3)).get("ok"),
                "second actual trade side failed",
            )
            pending = await mira.act("engine.trade_confirm", trade=trade)
            require(
                pending.get("ok") and not pending.get("completed"),
                "first confirmation must leave actual trade pending",
            )
            escrow_before = await mira.request("inspect", target=traded_item)
            require(escrow_before["flags"] & 128, "pending trade did not mark real goods escrow")
            walking = await mira.act("base.move_to", point=[40, -4, 0])
            require(walking.get("effect") == "started", "ongoing held-load navigation failed")
            before = await mira.request("look", limit=1)
            require(
                before["self"]["intent"] and before["self"]["holding"] == door,
                "snapshot setup must contain active navigation and held compound together",
            )
            summary["before"] = {
                "self": before["self"],
                "door": stable(door_before),
                "part": stable(child_before),
                "inventory": stable(inventory_before),
                "escrow": stable(escrow_before),
                "trade": trade,
                "rule": rule_before,
            }
        state_first = await first.stop()
        previous = hashlib.sha256((data / "events.jsonl.zst").read_bytes()).hexdigest()
        await second.start()
        async with (
            Player("Mira", url, args.mira_key_file, evidence) as mira,
            Player("Sol", url, args.sol_key_file, evidence) as sol,
        ):
            after = await mira.request("look", limit=1)
            require(
                after["self"]["h"] == before["self"]["h"]
                and after["self"]["holding"] == door
                and after["self"]["intent"] == before["self"]["intent"],
                "actual body/held assembly/navigation intent did not survive automatic restore",
            )
            for handle, expected in (
                (door, door_before),
                (parts[0], child_before),
                (inventory_item, inventory_before),
                (traded_item, escrow_before),
            ):
                actual = await mira.request("inspect", target=handle)
                require(
                    stable(actual) == stable(expected),
                    "full object/author/model/custody identity changed during real restart",
                )
            inventory = await mira.act("engine.inventory")
            require(
                inventory_item in inventory["inventory"], "hidden inventory row lost on restart"
            )
            rule_after = await inspect_rule(mira, script)
            require(
                rule_after == rule_before, "approved immutable rule metadata changed on restore"
            )
            floor = json.loads(args.elaboration_report.read_text())["checks"]["target"]
            require(
                (await mira.act("base.switch", floor, on=False)).get("effect") == "applied",
                "restored approved world rule did not execute through actual dispatch",
            )
            settled = await sol.act("engine.trade_confirm", trade=trade)
            require(
                settled.get("ok") and settled.get("completed"),
                "pre-restart trade confirmation/offers were not preserved for real settlement",
            )
            sold = await sol.request("inspect", target=traded_item)
            require(
                sold["owner"] == "p:3"
                and sold["parent"] == sol.welcome["body"]
                and not sold["flags"] & 128,
                "resumed trade lost actual ownership/custody",
            )
            revealed = await mira.act("engine.place_existing", inventory_item, position=[-8, -4, 0])
            require(
                revealed.get("effect") == "applied", "restored hidden geometry failed actual reveal"
            )
            placed = await mira.request("inspect", target=inventory_item)
            require(
                placed["parent"] is None and math.dist(placed["position"], [-8, -4, 0]) < 0.01,
                "inventory reveal did not restore geometry at supplied world pose",
            )
            await asyncio.sleep(0.6)
            continued = await mira.request("look", limit=1)
            require(
                math.dist(after["self"]["at"], continued["self"]["at"]) > 0.001,
                "persisted navigation did not continue through actual post-restart physics",
            )
            summary["after"] = {
                "self": after["self"],
                "continued_self": continued["self"],
                "settled_trade": settled,
                "sold": stable(sold),
                "revealed": stable(placed),
            }
        state_second = await second.stop()
        archive = data / "events.previous-1.jsonl.zst"
        require(
            archive.exists() and hashlib.sha256(archive.read_bytes()).hexdigest() == previous,
            "automatic restart did not preserve exact prior causal records",
        )
        summary.update(
            {
                "passed": True,
                "first_final_step": state_first["step"],
                "resumed_final_step": state_second["step"],
                "record_preserved": True,
                "script_errors": 0,
            }
        )
    finally:
        for native in (first, second):
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
    parser.add_argument(
        "--source-data", type=pathlib.Path, default=pathlib.Path(".cache/world-integrated")
    )
    parser.add_argument(
        "--elaboration-report",
        type=pathlib.Path,
        default=pathlib.Path(".cache/integration/elaboration-report.json"),
    )
    parser.add_argument("--mira-key-file", default=".runtime/keys/mira.key")
    parser.add_argument("--sol-key-file", default=".runtime/keys/sol.key")
    parser.add_argument("--port", type=int, default=18081)
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        default=pathlib.Path(".cache/integration/restart-live-report.json"),
    )
    args = parser.parse_args()
    artifacts = args.output.parent / f"restart-live-artifacts-{time.time_ns()}"
    evidence = Evidence(artifacts)
    summary = {"passed": False, "artifacts": str(artifacts)}
    try:
        asyncio.run(run(args, evidence, artifacts, summary))
    except Exception as problem:
        summary["problem"] = str(problem)
    args.output.write_text(json.dumps(summary, indent=2) + "\n")
    (artifacts / "transcript.json").write_text(json.dumps(evidence.events, indent=2) + "\n")
    print(
        json.dumps({key: value for key, value in summary.items() if key not in {"before", "after"}})
    )
    if not summary["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
