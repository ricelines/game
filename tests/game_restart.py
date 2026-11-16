"""Real prepared-world recovery gate, retaining every invocation and failure artifact."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
import shutil
import signal
import subprocess
import time

import zstandard
from game_config import add_arguments, check_configuration, configuration, launch_arguments


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def event_records(data):
    source = data / "events.jsonl.zst"
    if not source.exists():
        return []
    with source.open("rb") as raw, zstandard.ZstdDecompressor().stream_reader(raw) as decoded:
        text = decoded.read().decode()
    # The crash observer can read a flushed prefix while the writer is active.
    return [json.loads(line) for line in text.splitlines(keepends=True) if line.endswith("\n")]


def event_errors(data):
    return [row for row in event_records(data) if row.get("status") in {"error", "cutoff"}]


def describe(invocation, command, args):
    (invocation / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    provenance = configuration(args)
    for name, source in (("executable", args.server), ("plan", args.plan)):
        with source.open("rb") as stream:
            provenance[name] = hashlib.file_digest(stream, "sha256").hexdigest()
    (invocation / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")


def crash_after_saved_boundary(args, data, saved_step, artifacts, results):
    invocation = artifacts / "crashed"
    invocation.mkdir()
    command = [
        str(args.server.resolve()),
        "run",
        "--plan",
        str(args.plan.resolve()),
        "--data",
        str(data),
        "--no-network",
        "--steps",
        "1000000",
    ]
    command.extend(launch_arguments(args))
    describe(invocation, command, args)
    outcome = {"artifacts": str(invocation), "data": str(data), "clean_shutdown": False}
    results["crashed"] = outcome
    snapshot = data / "snapshots" / f"snap-{saved_step}.amb.zst"
    saved_hash = hashlib.sha256(snapshot.read_bytes()).hexdigest()
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
                    process.poll() is None, "crash-world process exited before post-snapshot work"
                )
                later = (
                    [row for row in event_records(data) if row.get("step", 0) > saved_step]
                    if (invocation / "stdout.txt").stat().st_size
                    else []
                )
                if later:
                    process.kill()
                    process.wait(timeout=30)
                    require(
                        process.returncode == -signal.SIGKILL,
                        "owned process was not killed by SIGKILL",
                    )
                    outcome.update(
                        {
                            "snapshot": str(snapshot),
                            "snapshot_sha256": saved_hash,
                            "rollback_to_step": saved_step,
                            "observed_record_step": min(row["step"] for row in later),
                            "termination": "SIGKILL",
                            "exit_code": process.returncode,
                            "elapsed_s": round(time.perf_counter() - started, 3),
                            "record_tail": "abandoned branch; not replayed or concatenated",
                        }
                    )
                    require(
                        hashlib.sha256(snapshot.read_bytes()).hexdigest() == saved_hash
                        and list((data / "snapshots").glob("snap-*.amb.zst")) == [snapshot],
                        "crash observer raced into a new saved boundary",
                    )
                    require(not event_errors(data), "crashed branch recorded handler errors")
                    return
                time.sleep(0.002)
            raise AssertionError("no actual post-snapshot journal event before crash-test timeout")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=30)
            outcome["exit_code"] = process.returncode
            outcome["elapsed_s"] = round(time.perf_counter() - started, 3)


def run(args, data, steps, artifacts, label, results):
    invocation = artifacts / label
    invocation.mkdir(parents=True)
    report = invocation / "state.json"
    command = [
        str(args.server.resolve()),
        "run",
        "--plan",
        str(args.plan.resolve()),
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
    command.extend(launch_arguments(args))
    describe(invocation, command, args)
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
    check_configuration(state, args)
    outcome["stats"] = json.loads(stdout.splitlines()[-1])
    outcome["script_errors"] = state["script_errors"]
    if state["script_errors"]:
        errors = event_errors(data)
        (invocation / "handler-errors.json").write_text(json.dumps(errors, indent=2) + "\n")
        raise AssertionError(
            f"{label} had {state['script_errors']} handler error/cutoff(s); "
            "state, stdout, stderr and handler records retained"
        )
    return state, outcome["stats"]


def compare_boundary(restored, saved):
    require(restored["step"] == saved["step"], "restore advanced the saved boundary")
    require(restored["timers"] == saved["timers"], "saved pending timer count changed")
    require(restored["allocations"] == saved["allocations"], "saved allocations changed")
    baseline = {row["handle"]: row for row in saved["objects"]}
    require(
        set(baseline) == {row["handle"] for row in restored["objects"]},
        "saved full object identities changed",
    )
    for row in restored["objects"]:
        expected = baseline[row["handle"]]
        require(row["model"] == expected["model"], "model identity changed after recovery")
        require(row["position"] == expected["position"], f"saved position changed: {row['handle']}")
        require(row["values"] == expected["values"], f"saved properties changed: {row['handle']}")
    return {
        "objects": len(baseline),
        "step": saved["step"],
        "positions_and_values_exact": True,
        "timers": restored["timers"],
        "allocations": restored["allocations"],
    }


def check_advancement(advanced, saved, steps):
    require(advanced["step"] == saved["step"] + steps, "resumed game advanced the wrong step count")
    baseline = {row["handle"]: row for row in saved["objects"]}
    require(
        set(baseline) == {row["handle"] for row in advanced["objects"]},
        "resumed idle world lost live object identities",
    )
    moved = 0
    for row in advanced["objects"]:
        require(row["model"] == baseline[row["handle"]]["model"], "resumed model identity changed")
        require(all(math.isfinite(x) for x in row["position"]), "resumed position is nonfinite")
        require(
            all(math.isfinite(x) for x in row["values"].values()), "resumed quantity is nonfinite"
        )
        moved += row["position"] != baseline[row["handle"]]["position"]
    require(moved > 0, "resumed scripted world did not continue moving")
    require(advanced["timers"] > 0, "resumed world lost all pending timers")
    require(advanced["allocations"] == saved["allocations"], "idle shared allocations were lost")
    return {"step": advanced["step"], "moving_objects": moved, "script_errors": 0}


def preserved_record(data, digest):
    for path in data.glob("events.previous-*.jsonl.zst"):
        if hashlib.sha256(path.read_bytes()).hexdigest() == digest:
            return str(path)
    raise AssertionError("restart did not preserve exact prior compressed causal record")


def reject_incompatible(args, snapshot, artifacts, results):
    invocation = artifacts / "incompatible-version"
    invocation.mkdir()
    report = invocation / "state.json"
    data = invocation / "world"
    original = snapshot.read_bytes()
    command = [
        str(args.server.resolve()),
        "run",
        "--plan",
        str(args.plan.resolve()),
        "--data",
        str(data),
        "--restore",
        str(snapshot),
        "--no-network",
        "--unpaced",
        "--steps",
        "1",
        "--report",
        str(report),
    ]
    command.extend(launch_arguments(args))
    describe(invocation, command, args)
    process = subprocess.run(command, capture_output=True, text=True, timeout=30, check=False)
    (invocation / "stdout.txt").write_text(process.stdout)
    (invocation / "stderr.txt").write_text(process.stderr)
    outcome = {
        "snapshot": str(snapshot),
        "snapshot_sha256": hashlib.sha256(original).hexdigest(),
        "snapshot_bytes": len(original),
        "exit_code": process.returncode,
        "problem": process.stderr.strip(),
        "snapshot_unchanged": snapshot.read_bytes() == original,
    }
    results["incompatible_version"] = outcome
    require(
        process.returncode == 1
        and process.stderr.strip() == "snapshot runtime is not loadable: ambergris-0.2"
        and not report.exists()
        and not (data / "registry").exists()
        and outcome["snapshot_unchanged"],
        "incompatible snapshot was not rejected before registry/row restoration",
    )
    outcome["passed"] = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    add_arguments(parser)
    parser.add_argument(
        "--server", type=pathlib.Path, default=pathlib.Path(".build/release/amb-server")
    )
    parser.add_argument(
        "--plan", type=pathlib.Path, default=pathlib.Path(".cache/content/seeded-plan.json")
    )
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument(
        "--incompatible-snapshot",
        type=pathlib.Path,
        help="A retained ambergris-0.2 snapshot to verify explicit version rejection.",
    )
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    artifacts = args.output.parent / f"{args.output.stem}-artifacts-{time.time_ns()}"
    artifacts.mkdir()
    resumed_data, crash_data = artifacts / "resumed-world", artifacts / "crash-world"
    results = {}
    evidence = {
        "passed": False,
        "steps": [1200, 200],
        "artifacts": str(artifacts),
        "configuration": configuration(args),
        "contract": (
            "Exact saved boundary continuity, then valid advancement; "
            "no deterministic replay assertion"
        ),
        "runs": results,
    }
    try:
        first, _ = run(
            args,
            resumed_data,
            1200,
            artifacts,
            "first",
            results,
        )
        require(first["step"] == 1200, "first invocation ended at wrong simulation step")
        shutil.copytree(resumed_data, crash_data)
        previous_record = hashlib.sha256(
            (resumed_data / "events.jsonl.zst").read_bytes()
        ).hexdigest()
        boundary, _ = run(
            args,
            resumed_data,
            0,
            artifacts,
            "clean-boundary",
            results,
        )
        evidence["clean_boundary"] = compare_boundary(boundary, first)
        evidence["earlier_record_archive"] = preserved_record(resumed_data, previous_record)
        resumed, _ = run(
            args,
            resumed_data,
            200,
            artifacts,
            "resumed",
            results,
        )
        evidence["clean_advancement"] = check_advancement(resumed, first, 200)
        crash_after_saved_boundary(
            args,
            crash_data,
            first["step"],
            artifacts,
            results,
        )
        crash_log = crash_data / "events.jsonl.zst"
        require(
            crash_log.stat().st_size > 0, "crashed native did not retain a compressed record tail"
        )
        abandoned_hash = hashlib.sha256(crash_log.read_bytes()).hexdigest()
        crash_boundary, _ = run(
            args,
            crash_data,
            0,
            artifacts,
            "crash-boundary",
            results,
        )
        evidence["crash_boundary"] = compare_boundary(crash_boundary, first)
        abandoned_archive = preserved_record(crash_data, abandoned_hash)
        crash_resumed, _ = run(
            args,
            crash_data,
            200,
            artifacts,
            "crash-resumed",
            results,
        )
        evidence["crash_recovery"] = {
            "passed": True,
            "signal": "SIGKILL",
            "rollback_to_step": first["step"],
            "automatic_latest_snapshot_recovery": True,
            "abandoned_record_sha256": abandoned_hash,
            "abandoned_record_archive": abandoned_archive,
            "abandoned_tail_preserved_separately": True,
            **check_advancement(crash_resumed, first, 200),
        }
        if args.incompatible_snapshot:
            reject_incompatible(
                args,
                args.incompatible_snapshot.resolve(),
                artifacts,
                results,
            )
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
