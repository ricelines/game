"""Warm worker: an author fork exports BREP; a clean fork measures it."""

import json
import os
import signal
import sys
import time
from pathlib import Path

from bake.sandbox import (
    landlock_version,
    load_seccomp,
    restrict_filesystem,
    restrict_resources,
    restrict_syscalls,
)


def main():
    configuration = json.loads(sys.stdin.readline())
    started = time.perf_counter()
    from bake.pipeline import author, load_native, measure

    native = load_native(Path("/native/libamb_bake_geometry.so"))
    seccomp = load_seccomp(configuration["library_paths"])
    abi = landlock_version()
    print(
        json.dumps({"ready": True, "import_s": time.perf_counter() - started, "landlock_abi": abi}),
        flush=True,
    )
    for line in sys.stdin:
        request = json.loads(line)
        job = Path("/jobs") / request["job"]
        job.mkdir()
        authored, asset = job / "author", job / "asset"
        cgroup = None
        if configuration["cgroup"]:
            cgroup = Path("/cgroup") / request["job"]
            cgroup.mkdir()
            (cgroup / "memory.max").write_text(
                str(configuration["config"]["BAKE_MB"] * 1024 * 1024)
            )
            (cgroup / "memory.swap.max").write_text("0")
        config = configuration["config"]
        deadline = time.monotonic() + config["BAKE_S"]
        for operation, args, output in (
            (author, (request["source"], request["params"], authored, config), authored),
            (
                measure,
                (request["source"], request["params"], authored, asset, config, native),
                asset,
            ),
        ):
            output.mkdir()
            result_file = output / "result.json"
            ready_read, ready_write = os.pipe()
            pid = os.fork()
            if pid == 0:
                os.close(ready_write)
                try:
                    os.read(ready_read, 1)
                    os.close(ready_read)
                    log = os.open(output / "job.log", os.O_WRONLY | os.O_CREAT, 0o600)
                    for descriptor in (0, 1, 2):
                        os.dup2(log, descriptor)
                    os.close(log)
                    restrict_resources(config)
                    reads = [Path(path) for path in configuration["read_paths"]] + [authored]
                    restrict_filesystem(reads, output)
                    restrict_syscalls(seccomp)
                    result = operation(*args)
                except BaseException as error:
                    result = {
                        "status": "failed",
                        "errors": [
                            {
                                "stage": getattr(error, "stage", "job"),
                                "message": f"{type(error).__name__}: {error}",
                            }
                        ],
                    }
                try:
                    result_file.write_text(json.dumps(result, allow_nan=False))
                finally:
                    os._exit(0)
            os.close(ready_read)
            if cgroup:
                (cgroup / "cgroup.procs").write_text(str(pid))
            os.write(ready_write, b"1")
            os.close(ready_write)
            timed_out = False
            while True:
                finished, status = os.waitpid(pid, os.WNOHANG)
                if finished:
                    break
                if time.monotonic() >= deadline:
                    timed_out = True
                    if cgroup and (cgroup / "cgroup.kill").exists():
                        (cgroup / "cgroup.kill").write_text("1")
                    else:
                        os.kill(pid, signal.SIGKILL)
                    _, status = os.waitpid(pid, 0)
                    break
                time.sleep(0.005)
            if timed_out:
                result = {
                    "status": "failed",
                    "errors": [
                        {
                            "stage": "timeout",
                            "message": "Bake wall-clock limit exceeded",
                        }
                    ],
                }
            elif result_file.exists():
                result = json.loads(result_file.read_text())
            else:
                result = {
                    "status": "failed",
                    "errors": [
                        {
                            "stage": "job",
                            "message": f"Job terminated without a result (wait status {status})",
                        }
                    ],
                }
            if result["status"] != "done":
                break
        result["job"] = request["job"]
        if cgroup:
            cgroup.rmdir()
        print(json.dumps(result, allow_nan=False), flush=True)


if __name__ == "__main__":
    main()
