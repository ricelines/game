"""Measure 500 text clients on a fresh paced Release world, without a gateway or LLM.

Run with bake/run.py. Preparation finishes before measurement starts. Defaults run
five minutes after the last staggered join, reconnect every client together, then
run until five minutes after the disconnect. Smaller arguments are smoke checks,
not acceptance evidence. All artifacts are retained beside the JSON report.
"""

from __future__ import annotations

import argparse
import asyncio
import io
import json
import os
import random
import signal
import socket
import subprocess
import sys
import time
from collections import Counter
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from types import SimpleNamespace

import zstandard

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from game_restart_live import Native  # noqa: E402

from gateway.session import Session  # noqa: E402


def save(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def private_key(path, key):
    with open(path, "x", opener=lambda name, flags: os.open(name, flags, 0o600)) as output:
        output.write(key + "\n")


@dataclass
class Client:
    number: int
    origin: float
    session: Session | None = None
    worker: asyncio.Task | None = None
    messages: int = 0
    received_bytes: int = 0
    connected_seconds: float = 0
    joined_at: float = 0
    bucket: int = -1
    bucket_bytes: int = 0
    max_bytes_per_second: int = 0
    requests: int = 0
    replies: int = 0
    cancelled: int = 0
    outcomes: Counter = field(default_factory=Counter)
    refusals: Counter = field(default_factory=Counter)
    reply_errors: Counter = field(default_factory=Counter)
    script_errors: Counter = field(default_factory=Counter)
    failures: list = field(default_factory=list)
    server_closed: list = field(default_factory=list)

    def received(self, raw):
        size = len(raw.encode("utf-8")) if isinstance(raw, str) else len(raw)
        bucket = int(time.monotonic() - self.origin)
        self.bucket_bytes = self.bucket_bytes + size if bucket == self.bucket else size
        self.bucket = bucket
        self.max_bytes_per_second = max(self.max_bytes_per_second, self.bucket_bytes)
        self.received_bytes += size
        self.messages += 1

    def report(self):
        return {
            "client": self.number,
            "joined_s": self.joined_at - self.origin if self.joined_at else None,
            "messages_received": self.messages,
            "bytes_received": self.received_bytes,
            "connected_seconds": self.connected_seconds,
            "mean_bytes_per_second": self.received_bytes / self.connected_seconds
            if self.connected_seconds
            else 0,
            "max_bytes_per_second": self.max_bytes_per_second,
            "requests": self.requests,
            "replies": self.replies,
            "cancelled_for_disconnect": self.cancelled,
            "reply_outcomes": dict(self.outcomes),
            "received_refusals": dict(self.refusals),
            "reply_error_messages": dict(self.reply_errors),
            "script_error_messages": dict(self.script_errors),
            "failures": self.failures,
            "server_closed_sockets": self.server_closed,
        }


class CountedSession(Session):
    def __init__(self, url, key, client):
        super().__init__(url, key, "density")
        self.client = client
        self.client_closing = False

    async def _read(self):
        started = time.monotonic()
        receive = self.socket.recv

        async def counted_receive(*args, **kwargs):
            raw = await receive(*args, **kwargs)
            self.client.received(raw)
            return raw

        self.socket.recv = counted_receive
        try:
            await super()._read()
        finally:
            self.client.connected_seconds += time.monotonic() - started
            if not self.client_closing:
                self.client.server_closed.append(
                    {
                        "at_s": time.monotonic() - self.client.origin,
                        "reason": self.closed_reason,
                        "close_code": self.socket.close_code,
                        "close_reason": self.socket.close_reason,
                    }
                )

    async def close(self):
        self.client_closing = True
        await super().close()


async def request(client, kind, arguments):
    client.requests += 1
    try:
        reply = await client.session.tool(kind, arguments)
    except asyncio.CancelledError:
        client.cancelled += 1
        raise
    except Exception as error:
        client.failures.append({"kind": kind, "error": f"{type(error).__name__}: {error}"})
        raise
    client.replies += 1
    client.reply_errors.update(reply.get("errors", []))
    if reply.get("error"):
        client.reply_errors[str(reply["error"])] += 1
    client.script_errors.update(reply.get("script_errors", []))
    if reply.get("ok") is False:
        reason = reply.get("denied", {}).get(
            "why", reply.get("reason", reply.get("error", "unspecified"))
        )
        client.refusals[str(reason)] += 1
        outcome = "refused"
    elif reply.get("error") or reply.get("errors"):
        outcome = "error"
    else:
        outcome = reply.get("reason", reply.get("effect", "ok"))
    client.outcomes[str(outcome)] += 1
    return reply


def move():
    return {
        "action": "base.move_to",
        "args": {
            "point": [random.uniform(-100, 100), random.uniform(-45, 45), 0],
        },
    }


async def act(client):
    try:
        while True:
            await asyncio.sleep(random.uniform(1, 10))
            choice = random.random()
            if choice < 0.6:
                await request(client, "do", move())
            elif choice < 0.8:
                await request(client, "look", {})
            else:
                await request(client, "inspect", {"target": "land"})
    except Exception:
        # request retains the failure; the other clients keep generating load.
        return


async def close_clients(clients):
    workers = [client.worker for client in clients if client.worker]
    for worker in workers:
        worker.cancel()
    await asyncio.gather(*workers, return_exceptions=True)
    closed = await asyncio.gather(
        *(client.session.close() for client in clients if client.session), return_exceptions=True
    )
    return [
        f"socket cleanup: {type(error).__name__}: {error}"
        for error in closed
        if isinstance(error, BaseException)
    ]


async def prepare(args, directory):
    plan = directory / "plan.json"
    command = [
        sys.executable,
        str(ROOT / "bake/run.py"),
        "-m",
        "content.prepare",
        "--mode",
        "seeded",
        "--stakes",
        "off",
        "--output",
        str(plan),
        "--assets",
        str(ROOT / "assets"),
        "--config",
        str(args.config.resolve()),
    ]
    if args.restricted_host_bake:
        command.append("--restricted-host-bake")
    with (directory / "prepare.log").open("w") as log:
        process = await asyncio.create_subprocess_exec(
            *command, cwd=ROOT, stdout=log, stderr=asyncio.subprocess.STDOUT, start_new_session=True
        )
        try:
            if await process.wait():
                raise RuntimeError("fresh plan preparation failed; prepare.log retained")
        finally:
            if process.returncode is None:
                os.killpg(process.pid, signal.SIGTERM)
                await process.wait()
    document = json.loads(plan.read_text())
    for profile in document["profiles"]:
        profile["key_file"] = str(directory / "keys" / f"seed-{profile['id']}.key")
    save(plan, document)
    return plan


async def memory(native, report):
    status = Path(f"/proc/{native.process.pid}/status")
    while native.process.poll() is None:
        try:
            for line in status.read_text().splitlines():
                if line.startswith("VmHWM:"):
                    report["server_vm_hwm_kib"] = max(
                        report["server_vm_hwm_kib"], int(line.split()[1])
                    )
        except FileNotFoundError:
            return
        await asyncio.sleep(0.5)


async def stop(native, report):
    if native.process is None:
        return
    if native.process.poll() is None:
        native.process.send_signal(signal.SIGINT)
        try:
            await asyncio.to_thread(native.process.wait, 30)
        except subprocess.TimeoutExpired:
            native.process.kill()
            await asyncio.to_thread(native.process.wait)
            report["errors"].append("server exceeded the 30-second SIGINT shutdown allowance")
    for log in native.logs:
        log.close()
    report["server_exit_code"] = native.process.returncode
    for line in (native.directory / "stdout.txt").read_text().splitlines():
        if line.startswith("{"):
            row = json.loads(line)
            if row.get("status") == "stopped":
                report["stopped"] = row
    if native.process.returncode or "stopped" not in report:
        report["errors"].append("server did not stop successfully with its final metrics")


def dropped_records(path):
    names, reasons = Counter(), Counter()
    with path.open("rb") as raw:
        with io.TextIOWrapper(zstandard.ZstdDecompressor().stream_reader(raw)) as rows:
            for line in rows:
                row = json.loads(line)
                reason = str(row.get("reason", ""))
                if (
                    row.get("name") == "overflow"
                    or "drop" in reason
                    or any(key.startswith("dropped") for key in row)
                ):
                    names[row["name"]] += 1
                    reasons[reason or row.get("queue", "unspecified")] += 1
    return {"by_name": dict(names), "by_reason_or_queue": dict(reasons)}


def assessments(report):
    stopped = report.get("stopped", {})
    p95 = stopped.get("phase_p95_ms")
    overruns = stopped.get("overruns")
    rows = report["clients"]
    failures = sum(len(row["failures"]) for row in rows)
    closed = sum(len(row["server_closed_sockets"]) for row in rows)
    mean_per_client = sum(row["mean_bytes_per_second"] for row in rows) / len(rows) if rows else 0
    highest_mean = max((row["mean_bytes_per_second"] for row in rows), default=0)
    complete = report["completed"] and not report["errors"] and not failures and not closed
    report["workload_completed"] = complete
    report["acceptance_evidence"] = complete and report["workload"]["full_workload"]
    report["brief_checks"] = {
        "phase_p95_under_25_ms": {
            "status": "pass" if complete and p95 is not None and p95 < 25 else "fail",
            "value_ms": p95,
        },
        "no_overruns_after_last_join": {
            "status": "pass" if complete and overruns == 0 else "unmeasured",
            "whole_run_overruns": overruns,
            "scope": "Only the whole-run counter is available. Zero proves zero after joining; "
            "a nonzero count cannot locate overruns relative to the last join.",
        },
        "no_server_closed_sockets": {
            "status": "pass" if complete and closed == 0 else "fail",
            "count": closed,
        },
        "no_missing_requested_replies": {
            "status": "pass" if complete and failures == 0 else "fail",
            "failed_requests": failures,
        },
        "mean_per_client_under_12500_bytes_per_second": {
            "status": "pass" if complete and mean_per_client < 12500 else "fail",
            "mean_per_client": mean_per_client,
            "highest_client_mean": highest_mean,
        },
    }
    report["authoritative_requirements"] = {
        "density": {
            "requirement": "well under 50 ms per step",
            "numeric_cutoff_defined": False,
            "whole_run_max_ms": stopped.get("phase_max_ms"),
            "recent_p95_ms": p95,
        },
        "load": {
            "requirement": "500 clients, no silent drops, reconnect storms recover quickly",
            "request_failures": failures,
            "server_closed_sockets": closed,
            "reconnect_seconds": report.get("reconnect_seconds"),
            "scope": "Requested replies and recorded overflow/drop events are observed; "
            "unsolicited messages have no independent sequence accounting.",
        },
        "aquarium": {
            "status": "not_measured",
            "reason": "Text clients do not measure the "
            "hundreds-of-fish viewer replication workload.",
        },
    }
    report["cost_investigation_needed"] = any(
        row["status"] != "pass" for row in report["brief_checks"].values()
    )
    if report["cost_investigation_needed"]:
        report["cost_investigation"] = (
            "Retain this run and collect the five largest phase costs "
            "with Tracy or temporary phase timers in a separate profiling run; no fixes here."
        )


async def run(args):
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    directory = output.with_suffix("")
    directory.mkdir(mode=0o700)
    (directory / "keys").mkdir(mode=0o700)
    report = {
        "workload": {
            "clients": args.clients,
            "join_stagger_s": args.join_stagger,
            "before_reconnect_s": args.seconds,
            "after_disconnect_s": args.seconds,
            "actions": "60% move_to Street, 20% look, 20% inspect land after random 1–10 s",
            "recovery_probe": "one base.move_to reply per reconnected client",
            "full_workload": args.clients == 500
            and args.join_stagger == 0.1
            and args.seconds == 300,
        },
        "exclusions": [
            "no gateway or LLM",
            "clients and server share this host",
            "preparation finishes before measurement; no bake work or authoring",
            "text sessions, not binary aquarium viewers",
        ],
        "measurement": {
            "bandwidth": "raw WebSocket application payload bytes, including hello/join; "
            "mean per connected second; max fixed one-second bucket",
            "phase_percentiles": "most recent up to 4096 server steps",
            "phase_max_ms": "whole server run, including join ramp and reconnect",
            "memory": "kernel VmHWM sampled every 0.5 seconds until server exit",
        },
        "errors": [],
        "completed": False,
        "server_vm_hwm_kib": 0,
    }
    clients, joining = [], []
    native = monitor = None
    try:
        plan = await prepare(args, directory)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        options = SimpleNamespace(
            server=ROOT / ".build/release/amb-server",
            plan=plan,
            config=args.config,
            profile="production",
            port=port,
        )
        native = Native(options, directory / "world", directory, "native")
        await native.start()
        monitor = asyncio.create_task(memory(native, report))
        origin = time.monotonic()
        url = f"ws://127.0.0.1:{port}/ws"

        async def join(client):
            client.session = CountedSession(url, "", client)
            await client.session.join(f"Density {client.number}")
            client.joined_at = time.monotonic()
            private_key(directory / "keys" / f"client-{client.number}.key", client.session.key)
            client.worker = asyncio.create_task(act(client))

        for number in range(args.clients):
            client = Client(number, origin)
            clients.append(client)
            joining.append(asyncio.create_task(join(client)))
            await asyncio.sleep(args.join_stagger)
        joined = await asyncio.gather(*joining, return_exceptions=True)
        for number, result in enumerate(joined):
            if isinstance(result, BaseException):
                report["errors"].append(f"join {number}: {type(result).__name__}: {result}")
        if report["errors"]:
            raise RuntimeError("not all clients joined; partial evidence retained")
        last_join = max(client.joined_at for client in clients)
        report["last_join_s"] = last_join - origin
        await asyncio.sleep(max(0, last_join + args.seconds - time.monotonic()))
        disconnected = time.monotonic()
        report["mass_disconnect_s"] = disconnected - origin
        report["errors"].extend(await close_clients(clients))

        async def reconnect(client):
            client.session = CountedSession(url, client.session.key, client)
            await client.session.open()
            await request(client, "do", move())
            client.worker = asyncio.create_task(act(client))

        recovered = await asyncio.gather(
            *(reconnect(client) for client in clients), return_exceptions=True
        )
        for number, result in enumerate(recovered):
            if isinstance(result, BaseException):
                report["errors"].append(f"reconnect {number}: {type(result).__name__}: {result}")
        if not report["errors"]:
            report["reconnect_seconds"] = time.monotonic() - disconnected
        await asyncio.sleep(max(0, disconnected + args.seconds - time.monotonic()))
        report["completed"] = True
    except BaseException as error:
        report["errors"].append(f"{type(error).__name__}: {error}")
    finally:
        for task in joining:
            if not task.done():
                task.cancel()
        await asyncio.gather(*joining, return_exceptions=True)
        report["errors"].extend(await close_clients(clients))
        if native:
            try:
                await stop(native, report)
            except Exception as error:
                report["errors"].append(f"shutdown: {type(error).__name__}: {error}")
        if monitor:
            monitor.cancel()
            await asyncio.gather(monitor, return_exceptions=True)
        report["clients"] = [client.report() for client in clients]
        journal = directory / "world/events.jsonl.zst"
        if journal.exists():
            try:
                report["overflow_or_drop_records"] = dropped_records(journal)
            except Exception as error:
                report["errors"].append(f"journal read: {type(error).__name__}: {error}")
        assessments(report)
        save(output, report)
    return 0 if report["workload_completed"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clients", type=int, default=500)
    parser.add_argument("--join-stagger", type=float, default=0.1)
    parser.add_argument("--seconds", type=float, default=300)
    parser.add_argument("--config", type=Path, default=ROOT / "config.toml")
    parser.add_argument("--restricted-host-bake", action="store_true")
    stamp = datetime.now(UTC).strftime("%Y%m%d-%H%M%S")
    parser.add_argument("--output", type=Path, default=ROOT / f".cache/bench/density-{stamp}.json")
    args = parser.parse_args()
    if args.clients < 1 or args.join_stagger < 0 or args.seconds <= 0:
        parser.error("clients and seconds must be positive; join-stagger must be nonnegative")
    raise SystemExit(asyncio.run(run(args)))


if __name__ == "__main__":
    main()
