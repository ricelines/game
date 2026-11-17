"""Restore the actual edited family from its native snapshot in an owned fresh process."""

import argparse
import asyncio
import json
import math
import signal
import socket
import subprocess
import time
from pathlib import Path

from game_config import add_arguments, check_configuration, configuration, launch_arguments
from game_live_authoring import require, script_state

from gateway.credentials import read_key_file
from gateway.session import Sessions


def listening(port):
    with socket.socket() as connection:
        connection.settimeout(0.1)
        return connection.connect_ex(("127.0.0.1", port)) == 0


async def run(options):
    source = json.loads(options.journey.read_text())
    require(source["status"] == "passed", "actual authoring journey must pass before restore")
    expected = source["checks"]
    deadline, snapshot = time.monotonic() + 90, None
    while time.monotonic() < deadline:
        snapshots = list((options.source_data / "snapshots").glob("snap-*.amb.zst"))
        eligible = [
            path
            for path in snapshots
            if int(path.name.split("-")[1].split(".")[0]) >= expected["committed_step"]
        ]
        if eligible:
            snapshot = min(eligible, key=lambda path: int(path.name.split("-")[1].split(".")[0]))
            break
        await asyncio.sleep(0.25)
    require(snapshot is not None, "no completed native snapshot yet contains the actual edit")
    require(not listening(options.port), "independent authoring restore port is occupied")
    options.output.mkdir(parents=True, exist_ok=False)
    state = options.output / "native-state.json"
    command = [
        str(options.server.resolve()),
        "run",
        "--restore",
        str(snapshot.resolve()),
        "--catalog",
        str(options.source_data.resolve()),
        "--data",
        str((options.output / "world").resolve()),
        "--plan",
        str(options.plan.resolve()),
        "--port",
        str(options.port),
        "--steps",
        "1000000",
        "--snapshot-at-end",
        "--report",
        str(state.resolve()),
    ]
    command.extend(launch_arguments(options))
    report = {
        "configuration": configuration(options),
        "kind": "Actual fresh-process restoration of socket-authored model definition/variants",
        "status": "running",
        "snapshot": str(snapshot),
        "command": command,
        "checks": {},
    }
    stdout, stderr = (
        (options.output / "stdout.log").open("w"),
        (options.output / "stderr.log").open("w"),
    )
    native = subprocess.Popen(command, stdout=stdout, stderr=stderr)
    sessions = Sessions(f"ws://127.0.0.1:{options.port}/ws")
    try:
        until = time.monotonic() + 60
        while not listening(options.port):
            require(
                native.poll() is None and time.monotonic() < until,
                "owned restore process did not become ready; logs retained",
            )
            await asyncio.sleep(0.05)
        author = await sessions.get(read_key_file(options.author_key), "authoring-restore-author")
        other = await sessions.get(read_key_file(options.other_key), "authoring-restore-other")
        model = await author.tool("inspect", {"target": expected["model"]})
        require(
            model["source_hash"] == expected["actual_cli_current_definition"],
            "restored model lost committed source binding",
        )
        require(
            json.loads(model["canonical_params"])["width"] == 0.14,
            "restored default0 lost edited source default",
        )
        require(
            any(json.loads(row["canonical_params"])["width"] == 0.2 for row in model["variants"]),
            "restored current definition lost its explicit ready variant",
        )
        restored = []
        for index, prior in enumerate(expected["after"]):
            player = author if index == 0 else other
            current = await player.tool("inspect", {"target": prior["h"]})
            require(
                all(
                    current[key] == prior[key] for key in ("h", "model", "owner", "maker", "parent")
                ),
                "restored root changed full identity, authority or parent",
            )
            require(
                current["measured"]["asset"] == prior["measured"]["asset"]
                and current["measured"]["params"] == prior["measured"]["params"],
                "restored root lost actual geometry/parameter mapping",
            )
            require(
                math.dist(current["position"], prior["position"]) < 0.03,
                "restored root lost settled physical pose",
            )
            actual_state = script_state(current, expected["watcher_script"])
            require(
                actual_state["mass_notices"]
                >= expected["watch_states_after"][index]["mass_notices"],
                "restored actual watcher lost retained flat state",
            )
            require(
                math.isclose(
                    actual_state["observed_mass"], current["values"]["base.mass"], rel_tol=1e-6
                ),
                "restored watcher sees obsolete measured geometry",
            )
            restored.append(current)
        children = []
        for prior in expected["retained_named_parts"]:
            current = await author.tool("inspect", {"target": prior["h"]})
            require(
                current["h"] == prior["h"]
                and current["parent"] == prior["parent"]
                and current["measured"]["asset"] == prior["measured"]["asset"],
                "restored matching child lost its handle/parent/geometry alias",
            )
            children.append(current)
        report["checks"] = {"model": model, "roots": restored, "retained_children": children}
        report["status"] = "passed"
    except BaseException as error:
        report.update({"status": "failed", "problem": str(error)})
        raise
    finally:
        await sessions.close()
        if native.poll() is None:
            native.send_signal(signal.SIGTERM)
            try:
                await asyncio.to_thread(native.wait, 30)
            except subprocess.TimeoutExpired:
                native.kill()
                await asyncio.to_thread(native.wait)
                report.update(
                    {
                        "status": "failed",
                        "problem": "owned restore process did not shut down within30s",
                    }
                )
        stdout.close()
        stderr.close()
        report["native_exit_code"] = native.returncode
        if state.exists():
            report["native_state"] = json.loads(state.read_text())
            try:
                check_configuration(report["native_state"], options)
                require(
                    report["native_state"]["script_errors"] == 0,
                    "restored world recorded handler errors/cutoffs",
                )
            except AssertionError as error:
                report.update(status="failed", problem=str(error))
        if native.returncode != 0 or not state.exists():
            report["status"] = "failed"
        (options.output / "restore-report.json").write_text(json.dumps(report, indent=2))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    add_arguments(parser)
    parser.add_argument(
        "--journey", type=Path, default=Path(".cache/integration/live-authoring-report.json")
    )
    parser.add_argument("--source-data", type=Path, default=Path(".cache/final-world-2026-11-15"))
    parser.add_argument("--server", type=Path, default=Path(".build/release/amb-server"))
    parser.add_argument("--plan", type=Path, default=Path(".cache/content/seeded-plan.json"))
    parser.add_argument("--port", type=int, default=18084)
    parser.add_argument("--author-key", type=Path, default=Path(".runtime/keys/mira.key"))
    parser.add_argument("--other-key", type=Path, default=Path(".runtime/keys/sol.key"))
    parser.add_argument(
        "--output", type=Path, default=Path(".cache/integration/live-authoring-restore")
    )
    options = parser.parse_args()
    result = asyncio.run(run(options))
    print(json.dumps({"status": result["status"], "evidence": str(options.output)}))
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
