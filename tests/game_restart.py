"""Real prepared-world recovery gate, retaining every invocation and failure artifact."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
import signal
import subprocess
import time

import zstandard


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def event_errors(data):
    source = data / "events.jsonl.zst"
    if not source.exists():
        return []
    with source.open("rb") as raw, zstandard.ZstdDecompressor().stream_reader(raw) as decoded:
        rows = decoded.read().decode().splitlines()
    return [row for line in rows if (row := json.loads(line)).get("status") in {"error", "cutoff"}]


def describe(invocation, command, server, plan):
    (invocation / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    provenance = {}
    for name, source in (("executable", server), ("plan", plan)):
        with source.open("rb") as stream:
            provenance[name] = hashlib.file_digest(stream, "sha256").hexdigest()
    (invocation / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")


def crash_after_snapshot(server, plan, data, artifacts, results):
    invocation = artifacts / "crashed"
    invocation.mkdir()
    command = [
        str(server),
        "run",
        "--plan",
        str(plan),
        "--data",
        str(data),
        "--no-network",
        "--unpaced",
        "--steps",
        "1000000",
    ]
    describe(invocation, command, server, plan)
    outcome = {"artifacts": str(invocation), "data": str(data), "clean_shutdown": False}
    results["crashed"] = outcome
    started = time.perf_counter()
    with (
        (invocation / "stdout.txt").open("w") as stdout,
        (invocation / "stderr.txt").open("w") as stderr,
    ):
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr)
        try:
            until = started + 120
            while time.perf_counter() < until:
                require(
                    process.poll() is None, "crash-world process exited before a periodic snapshot"
                )
                completed = list((data / "snapshots").glob("snap-*.amb.zst"))
                if completed:
                    snapshot = max(completed, key=lambda p: int(p.name.split("-")[1].split(".")[0]))
                    step = int(snapshot.name.split("-")[1].split(".")[0])
                    require(step < 1400, "crash observer missed the first periodic snapshot")
                    process.kill()
                    process.wait(timeout=30)
                    require(
                        process.returncode == -signal.SIGKILL,
                        "owned process was not killed by SIGKILL",
                    )
                    outcome.update(
                        {
                            "snapshot": str(snapshot),
                            "rollback_to_step": step,
                            "termination": "SIGKILL",
                            "exit_code": process.returncode,
                            "elapsed_s": round(time.perf_counter() - started, 3),
                            "record_tail": "abandoned branch; not replayed or concatenated",
                        }
                    )
                    return step
                time.sleep(0.002)
            raise AssertionError("no completed periodic snapshot before crash-test timeout")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=30)
            outcome["exit_code"] = process.returncode
            outcome["elapsed_s"] = round(time.perf_counter() - started, 3)


def run(server, plan, data, steps, artifacts, label, results):
    invocation = artifacts / label
    invocation.mkdir(parents=True)
    report = invocation / "state.json"
    command = [
        str(server),
        "run",
        "--plan",
        str(plan),
        "--data",
        str(data),
        "--no-network",
        "--unpaced",
        "--steps",
        str(steps),
        "--snapshot-at-end",
        "--report",
        str(report),
    ]
    describe(invocation, command, server, plan)
    started = time.perf_counter()
    try:
        process = subprocess.run(command, text=True, capture_output=True, timeout=120, check=False)
        stdout, stderr, exit_code = process.stdout, process.stderr, process.returncode
    except subprocess.TimeoutExpired as problem:
        stdout = problem.stdout or b""
        stderr = problem.stderr or b""
        stdout = stdout.decode() if isinstance(stdout, bytes) else stdout
        stderr = stderr.decode() if isinstance(stderr, bytes) else stderr
        exit_code = "timeout"
    (invocation / "stdout.txt").write_text(stdout)
    (invocation / "stderr.txt").write_text(stderr)
    outcome = {
        "exit_code": exit_code,
        "elapsed_s": round(time.perf_counter() - started, 3),
        "artifacts": str(invocation),
        "data": str(data),
    }
    results[label] = outcome
    require(exit_code == 0, f"{label} native process failed ({exit_code}); artifacts retained")
    require(report.exists(), f"{label} did not produce a state report; logs retained")
    state = json.loads(report.read_text())
    outcome["stats"] = json.loads(stdout.splitlines()[-1])
    outcome["script_errors"] = state["script_errors"]
    if state["script_errors"]:
        errors = event_errors(data)
        (invocation / "handler-errors.json").write_text(json.dumps(errors, indent=2) + "\n")
        raise AssertionError(
            f"{label} had {state['script_errors']} handler error/cutoff(s); "
            "production limits unchanged; "
            "state, stdout, stderr and handler records retained"
        )
    return state, outcome["stats"]


def compare(resumed, uninterrupted):
    require(resumed["step"] == uninterrupted["step"] == 1400, "automatic recovery lost step count")
    require(resumed["timers"] == uninterrupted["timers"], "pending timer counts diverged")
    require(resumed["allocations"] == uninterrupted["allocations"], "shared allocations diverged")
    baseline = {row["handle"]: row for row in uninterrupted["objects"]}
    require(
        set(baseline) == {row["handle"] for row in resumed["objects"]},
        "full live object identities diverged",
    )
    distance_max, value_error_max = 0.0, 0.0
    for row in resumed["objects"]:
        expected = baseline[row["handle"]]
        require(row["model"] == expected["model"], "model identity changed after recovery")
        distance_max = max(distance_max, math.dist(row["position"], expected["position"]))
        require(row["values"].keys() == expected["values"].keys(), "property presence diverged")
        for term, actual in row["values"].items():
            wanted = expected["values"][term]
            value_error_max = max(value_error_max, abs(actual - wanted))
            require(
                math.isclose(actual, wanted, rel_tol=1e-6, abs_tol=1e-6),
                f"property continuity failed: {row['handle']} {term}: {actual} != {wanted}",
            )
    require(distance_max < 0.02, f"physical positions diverged by {distance_max} m")
    return {
        "objects": len(baseline),
        "position_error_max_m": distance_max,
        "value_error_max": value_error_max,
        "timers": resumed["timers"],
        "allocations": resumed["allocations"],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--server", type=pathlib.Path, default=pathlib.Path(".build/release/amb-server")
    )
    parser.add_argument(
        "--plan", type=pathlib.Path, default=pathlib.Path(".cache/content/seeded-plan.json")
    )
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    artifacts = args.output.parent / f"{args.output.stem}-artifacts-{time.time_ns()}"
    artifacts.mkdir()
    resumed_data, full_data = artifacts / "resumed-world", artifacts / "full-world"
    results = {}
    evidence = {
        "passed": False,
        "steps": [1200, 200],
        "artifacts": str(artifacts),
        "production_handler_limits_unchanged": True,
        "runs": results,
    }
    try:
        first, _ = run(
            args.server.resolve(),
            args.plan.resolve(),
            resumed_data,
            1200,
            artifacts,
            "first",
            results,
        )
        require(first["step"] == 1200, "first invocation ended at wrong simulation step")
        previous_record = hashlib.sha256(
            (resumed_data / "events.jsonl.zst").read_bytes()
        ).hexdigest()
        resumed, _ = run(
            args.server.resolve(),
            args.plan.resolve(),
            resumed_data,
            200,
            artifacts,
            "resumed",
            results,
        )
        archive = resumed_data / "events.previous-1.jsonl.zst"
        require(
            archive.exists()
            and archive.stat().st_size > 0
            and hashlib.sha256(archive.read_bytes()).hexdigest() == previous_record,
            "restart did not preserve exact prior compressed causal record",
        )
        uninterrupted, _ = run(
            args.server.resolve(),
            args.plan.resolve(),
            full_data,
            1400,
            artifacts,
            "uninterrupted",
            results,
        )
        evidence.update(compare(resumed, uninterrupted))
        crash_data = artifacts / "crash-world"
        snapshot_step = crash_after_snapshot(
            args.server.resolve(), args.plan.resolve(), crash_data, artifacts, results
        )
        crash_log = crash_data / "events.jsonl.zst"
        require(
            crash_log.stat().st_size > 0, "crashed native did not retain a compressed record tail"
        )
        abandoned_hash = hashlib.sha256(crash_log.read_bytes()).hexdigest()
        crash_resumed, _ = run(
            args.server.resolve(),
            args.plan.resolve(),
            crash_data,
            1400 - snapshot_step,
            artifacts,
            "crash-resumed",
            results,
        )
        crash_archive = crash_data / "events.previous-1.jsonl.zst"
        require(
            crash_archive.exists()
            and hashlib.sha256(crash_archive.read_bytes()).hexdigest() == abandoned_hash,
            "crash rollback did not preserve its exact abandoned compressed record tail",
        )
        evidence["crash_recovery"] = {
            "passed": True,
            "signal": "SIGKILL",
            "rollback_to_step": snapshot_step,
            "automatic_latest_snapshot_recovery": True,
            "abandoned_record_sha256": abandoned_hash,
            "abandoned_tail_preserved_separately": True,
            **compare(crash_resumed, uninterrupted),
        }
        evidence.update(
            {
                "passed": True,
                "final_step": resumed["step"],
                "earlier_record_preserved": True,
                "automatic_latest_snapshot_recovery": True,
            }
        )
    except Exception as problem:
        evidence["problem"] = str(problem)
        args.output.write_text(json.dumps(evidence, indent=2) + "\n")
        raise SystemExit(f"Restart gate failed: {problem}. Evidence: {args.output}") from None
    args.output.write_text(json.dumps(evidence, indent=2) + "\n")
    print(json.dumps(evidence))


if __name__ == "__main__":
    main()
