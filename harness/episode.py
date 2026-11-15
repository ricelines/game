"""Supervise an actual-model population segment and close its native record cleanly."""

import argparse
import asyncio
import hashlib
import json
import os
import signal
import socket
import subprocess
import time
from pathlib import Path

from aiohttp import web

from bake.pool import BakePool
from gateway.authoring import Authoring
from gateway.credentials import read_key_file
from gateway.http import create_app
from gateway.service import write_resident_report
from gateway.session import Sessions
from harness.resident import Resident, Residents


def digest(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def save(path, value):
    write_resident_report(path, value)


def listening(port):
    with socket.socket() as connection:
        connection.settimeout(0.1)
        return connection.connect_ex(("127.0.0.1", port)) == 0


async def run(options):
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    if listening(options.port) or listening(options.gateway_port):
        raise RuntimeError("Episode's independent native/gateway port is occupied")
    previous = None
    lineage = None
    plan_source = options.plan
    if options.resume_episode:
        prior = options.resume_episode.resolve()
        previous = json.loads((prior / "resident-report.json").read_text())
        closed = json.loads((prior / "episode.json").read_text())
        if closed["status"] != "completed" or closed["native_exit_code"] != 0:
            raise RuntimeError("Continuation requires the prior owned process to close cleanly")
        snapshot = Path(closed["latest_snapshot"])
        plan_source = prior / "start-plan.json"
        lineage = {
            "previous_episode": str(prior),
            "episode_sha256": digest(prior / "episode.json"),
            "resident_report_sha256": digest(prior / "resident-report.json"),
            "original_S7_sha256": digest(prior / "world/events.jsonl.zst"),
            "snapshot": str(snapshot),
            "snapshot_sha256": digest(snapshot),
            "body_script": "Original immutable installed script remains pinned on restore",
        }
    plan = json.loads(plan_source.read_text())
    definitions = json.loads(options.residents.read_text())
    keys = {}
    for profile in plan["profiles"]:
        path = (
            Path(profile["key_file"])
            if previous
            else output / "keys" / Path(profile["key_file"]).name
        )
        profile["key_file"] = str(path)
        keys[profile["id"]] = path
    for row in definitions["agents"]:
        row["key_file"] = str(keys[int(row["id"].split(":")[1])])
    launch_plan, residents_path = output / "start-plan.json", output / "residents.json"
    save(launch_plan, plan)
    save(residents_path, definitions)
    residents_path.chmod(0o600)
    state_path, model_path = output / "native-state.json", output / "resident-report.json"
    command = [
        str(options.server.resolve()),
        "run",
        "--plan",
        str(launch_plan),
        "--data",
        str(output / "world"),
        "--port",
        str(options.port),
        "--steps",
        "1000000",
        "--snapshot-at-end",
        "--report",
        str(state_path),
    ]
    if lineage:
        command += ["--restore", lineage["snapshot"], "--catalog", str(prior / "world")]
    summary = {
        "kind": "Actual Responses API population; original authored personas and purposes",
        "status": "starting",
        "command": command,
        "source": {
            "binary_sha256": digest(options.server),
            "plan_sha256": digest(plan_source),
            "launch_plan_sha256": digest(launch_plan),
            "residents_sha256": digest(options.residents),
            "reference_sha256": digest(options.reference),
        },
        "resident_ids": [row["id"] for row in definitions["agents"]],
        "requested_seconds": options.seconds,
        "max_api_attempts": options.max_responses,
        "continuation_of": lineage,
    }
    save(output / "episode.json", summary)
    logs = [(output / name).open("w") for name in ("native-stdout.log", "native-stderr.log")]
    # The native process has no reason to inherit the model credential.
    environment = {name: value for name, value in os.environ.items() if name != "OPENAI_API_KEY"}
    native = subprocess.Popen(command, stdout=logs[0], stderr=logs[1], env=environment)
    runner, residents, pool = None, None, None
    started = time.monotonic()
    try:
        deadline = time.monotonic() + 90
        while not listening(options.port):
            if native.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("Owned native process did not become ready; logs retained")
            await asyncio.sleep(0.05)
        pool = await asyncio.to_thread(BakePool, options.assets, require_cgroup=False)
        authoring = Authoring(pool)
        sessions = Sessions(f"ws://127.0.0.1:{options.port}/ws")
        rows = [
            Resident(
                identity=row["id"],
                name=row["name"],
                key=read_key_file(row["key_file"]),
                purpose=row["purpose"],
                persona=row.get("persona", ""),
                model=row.get("model", "gpt-5.4-mini"),
                interval_s=float(row.get("interval_s", 8)),
            )
            for row in definitions["agents"]
        ]
        if previous:
            saved = {row["id"]: row for row in previous["agents"]}
            for row in rows:
                original = saved[row.identity]
                if original["purpose"] != row.purpose:
                    raise RuntimeError("Continuation purpose differs from the original definition")
                row.transcript = original["transcript"]
                row.archived_transcripts = original["archived_transcripts"]
                row.context_resets = original["context_resets"]

        def checkpoint(report):
            save(model_path, {**report, "continuation_of": lineage})

        residents = Residents(
            rows,
            sessions,
            authoring,
            reference=options.reference.read_text(),
            report_sink=checkpoint,
            max_responses=options.max_responses,
        )
        app = create_app(
            sessions,
            authoring,
            residents,
            allowed_origins=[f"http://127.0.0.1:{options.port}"],
            purpose_token=read_key_file(keys[8]),
            resident_report=checkpoint,
        )
        runner = web.AppRunner(app, access_log=None)
        await runner.setup()
        await web.TCPSite(runner, "127.0.0.1", options.gateway_port).start()
        residents.start()
        summary.update({"status": "running", "sandbox": pool.sandbox})
        save(output / "episode.json", summary)
        print(
            json.dumps(
                {
                    "status": "running",
                    "evidence": str(output),
                    "native_port": options.port,
                    "gateway_port": options.gateway_port,
                }
            ),
            flush=True,
        )
        until = time.monotonic() + options.seconds
        while (
            time.monotonic() < until
            and not (output / "STOP").exists()
            and not residents.budget_reached
        ):
            if native.poll() is not None:
                raise RuntimeError("Owned native process ended during model turns; logs retained")
            await asyncio.sleep(0.5)
        if residents.budget_reached:
            summary["stop_reason"] = "API attempt budget reached"
        elif (output / "STOP").exists():
            summary["stop_reason"] = "STOP marker"
        else:
            summary["stop_reason"] = "duration elapsed"
        # Keep the native world alive while complete model/tool turns finish.
        residents.stopping = True
        for row in residents.rows.values():
            row.wake.set()
        try:
            await asyncio.wait_for(asyncio.gather(*residents.tasks), 300)
            summary["resident_stop"] = "completed active turns"
        except TimeoutError:
            summary["resident_stop"] = (
                "300s grace elapsed; unfinished turns cancelled; preceding checkpoints retained"
            )
        residents.checkpoint()
        summary["status"] = "completed"
    except BaseException as error:
        summary.update({"status": "failed", "problem": type(error).__name__ + ": " + str(error)})
        raise
    finally:
        if runner:
            await runner.cleanup()
        elif pool:
            await asyncio.to_thread(pool.close)
        if native.poll() is None:
            native.send_signal(signal.SIGTERM)
            try:
                await asyncio.to_thread(native.wait, 30)
            except subprocess.TimeoutExpired:
                native.kill()
                await asyncio.to_thread(native.wait)
                summary["shutdown"] = "owned native required SIGKILL after30s; not a clean shutdown"
        for log in logs:
            log.close()
        summary.update(
            {"native_exit_code": native.returncode, "elapsed_seconds": time.monotonic() - started}
        )
        if state_path.exists():
            summary["native_state"] = json.loads(state_path.read_text())
        snapshots = sorted(
            (output / "world/snapshots").glob("snap-*.amb.zst"),
            key=lambda path: int(path.name.split("-")[1].split(".")[0]),
        )
        summary["latest_snapshot"] = str(snapshots[-1]) if snapshots else None
        if native.returncode == 0 and state_path.exists() and snapshots:
            summary["shutdown"] = (
                "owned native SIGTERM waited successfully; final state and snapshot retained"
            )
        else:
            summary["status"] = "failed"
        save(output / "episode.json", summary)
        print(
            json.dumps(
                {
                    "status": summary["status"],
                    "native_exit_code": native.returncode,
                    "evidence": str(output),
                }
            ),
            flush=True,
        )
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, default=Path(".build/release/amb-server"))
    parser.add_argument("--plan", type=Path, default=Path(".cache/content/seeded-plan.json"))
    parser.add_argument("--residents", type=Path, default=Path("content/residents.json"))
    parser.add_argument("--reference", type=Path, default=Path("docs/agent-authoring.md"))
    parser.add_argument("--assets", type=Path, default=Path("assets"))
    parser.add_argument("--port", type=int, default=18082)
    parser.add_argument("--gateway-port", type=int, default=18882)
    parser.add_argument("--seconds", type=float, default=1200)
    parser.add_argument(
        "--max-responses", type=int, default=0, help="API attempt cap;0 is uncapped"
    )
    parser.add_argument(
        "--resume-episode", type=Path, help="Prior cleanly closed episode directory"
    )
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    if options.max_responses < 0:
        parser.error("--max-responses must be nonnegative")
    result = asyncio.run(run(options))
    return 0 if result["status"] == "completed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
