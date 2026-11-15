"""Queued service boundary around one warm, isolated CAD worker."""

from __future__ import annotations

import hashlib
import json
import queue
import re
import selectors
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from pathlib import Path

from bake.config import ROOT, load_config
from bake.parameters import asset_hash, resolve
from bake.sandbox import SandboxUnavailable, cgroup_probe


def _readline(process, timeout):
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        if not selector.select(timeout):
            raise SandboxUnavailable("Warm bake worker did not answer before deadline")
    line = process.stdout.readline()
    if not line:
        raise SandboxUnavailable("Warm bake worker exited unexpectedly; see worker stderr log")
    return json.loads(line)


class BakePool:
    """One warm worker and FIFO queue; strict cgroup enforcement is the default.

    require_cgroup=False explicitly selects the documented restricted-host test mode.
    It still enforces namespaces, Landlock, seccomp and address-space/CPU limits.
    """

    def __init__(
        self,
        assets: Path,
        *,
        config: dict | None = None,
        cgroup_root: Path | None = None,
        require_cgroup: bool = True,
    ):
        self.assets = assets.resolve()
        self.assets.mkdir(parents=True, exist_ok=True)
        self.config = config or load_config()
        runtime = json.loads((ROOT / ".cache/bake-runtime.json").read_text())
        self.sandbox = {"cgroup": cgroup_probe(cgroup_root), "restricted_host": not require_cgroup}
        if require_cgroup and not self.sandbox["cgroup"]["available"]:
            raise SandboxUnavailable(self.sandbox["cgroup"]["error"])
        self.jobs_root = ROOT / ".cache/bake-jobs" / uuid.uuid4().hex
        self.jobs_root.mkdir(parents=True)
        self._log = (self.jobs_root / "worker.log").open("w")
        self.native = ROOT / ".build/bake/amb_bake_native"
        native_library = ROOT / ".build/bake/libamb_bake_geometry.so"
        if not native_library.exists():
            raise SandboxUnavailable("Build bake/native first; native geometry library missing")
        command = [
            runtime["bwrap"],
            "--die-with-parent",
            "--new-session",
            "--unshare-all",
            "--clearenv",
        ]
        read_paths = []
        for directory in [
            Path("/nix/store"),
            Path("/lib"),
            Path("/lib64"),
            Path("/usr/lib"),
            Path(sys.base_prefix).resolve(),
            Path(sys.prefix),
        ]:
            if directory.exists() and str(directory) not in read_paths:
                command += ["--ro-bind", str(directory), str(directory)]
                read_paths.append(str(directory))
        command += [
            "--ro-bind",
            str(ROOT / "bake"),
            "/app/bake",
            "--ro-bind",
            str(ROOT / "amb"),
            "/app/amb",
            "--ro-bind",
            str(native_library.parent),
            "/native",
            "--bind",
            str(self.jobs_root),
            "/jobs",
            "--proc",
            "/proc",
            "--dev",
            "/dev",
            "--tmpfs",
            "/tmp",
            "--chdir",
            "/app",
        ]
        read_paths += ["/app", "/native"]
        use_cgroup = bool(self.sandbox["cgroup"]["available"])
        if use_cgroup:
            command += ["--bind", str(cgroup_root), "/cgroup"]
        environment = {
            "PATH": "/nonexistent",
            "HOME": "/tmp",
            "LANG": "C.UTF-8",
            "PYTHONPATH": (
                f"/app:{sys.prefix}/lib/"
                f"python{sys.version_info.major}.{sys.version_info.minor}/site-packages"
            ),
            "LD_LIBRARY_PATH": ":".join(runtime["library_paths"]),
            "OPENBLAS_NUM_THREADS": "1",
            "OMP_NUM_THREADS": "1",
            "PYTHONDONTWRITEBYTECODE": "1",
        }
        for name, value in environment.items():
            command += ["--setenv", name, value]
        command += [runtime["loader"], str(Path(sys.executable).resolve()), "-m", "bake.worker"]
        self._process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self._log,
            text=True,
            env={},
        )
        self._process.stdin.write(
            json.dumps(
                {
                    "config": self.config,
                    "read_paths": read_paths,
                    "library_paths": runtime["library_paths"],
                    "cgroup": use_cgroup,
                }
            )
            + "\n"
        )
        self._process.stdin.flush()
        try:
            self.sandbox.update(_readline(self._process, 45))
        except BaseException:
            self._process.kill()
            self._process.wait()
            self._log.close()
            raise
        self._jobs = {}
        self._cache = {}
        self._queue = queue.Queue()
        self._thread = threading.Thread(target=self._serve, name="bake-supervisor", daemon=True)
        self._thread.start()

    def submit(self, source: str, params: dict | None = None) -> str:
        identity = uuid.uuid4().hex
        self._jobs[identity] = {"job": identity, "status": "queued"}
        self._queue.put({"job": identity, "source": source, "params": params or {}})
        return identity

    def status(self, identity: str) -> dict:
        return json.loads(json.dumps(self._jobs[identity]))

    def _regular(self, path):
        if not path.resolve().is_relative_to(path.parent.resolve()):
            raise ValueError("Asset file escapes its directory")
        info = path.lstat()
        if not stat.S_ISREG(info.st_mode):
            raise ValueError("Asset artifacts must be regular files, not symlinks")
        maximum = min(self.config["BAKE_MB"] * 1024 * 1024, self.config["TRIS_MAX"] * 256)
        if info.st_size > maximum:
            raise ValueError("Asset artifact exceeds configured geometry/memory limit")
        return path

    def _asset_directory(self, directory):
        resolved = directory.resolve()
        bases = (self.jobs_root.resolve(), self.assets.resolve())
        if not any(resolved.is_relative_to(base) for base in bases):
            raise ValueError("Asset directory escapes permitted roots")
        for ancestor in (directory, *directory.parents):
            if ancestor.is_symlink():
                raise ValueError("Asset directories cannot contain symlinks")
            if ancestor in (self.jobs_root, self.assets):
                break

    def _validate(self, directory, expected_hash):
        self._asset_directory(directory)
        manifest = json.loads(self._regular(directory / "manifest.json").read_text())
        if manifest["hash"] != expected_hash:
            raise ValueError("Asset identity mismatch")
        for lod in manifest["lods"]:
            if lod["file"] not in {"lod0.glb", "lod1.glb", "lod2.glb"}:
                raise ValueError("Unexpected LOD path")
            self._regular(directory / lod["file"])
            result = subprocess.run(
                [str(self.native), "--validate-glb", str(directory / lod["file"])],
                check=True,
                capture_output=True,
                text=True,
                timeout=self.config["BAKE_S"],
            )
            actual = json.loads(result.stdout)
            if (
                actual["triangles"] != lod["triangles"]
                or actual["triangles"] > self.config["TRIS_MAX"]
            ):
                raise ValueError("LOD geometry does not match declaration")
        for role in ("static", "dynamic"):
            filename = manifest["collision"][role]
            if filename is not None:
                if filename not in {"collision.bin", "convex.bin"}:
                    raise ValueError("Unexpected collision path")
                self._regular(directory / filename)
                subprocess.run(
                    [str(self.native), "--inspect", str(directory / filename)],
                    check=True,
                    capture_output=True,
                    timeout=self.config["BAKE_S"],
                )
        return manifest

    def _publish(self, request, result):
        directory = self.jobs_root / request["job"]
        self._asset_directory(directory)
        manifest = json.loads(self._regular(directory / "manifest.json").read_text())
        expected = asset_hash(request["source"], manifest["params"], self.config["BAKE_VERSION"])
        self._validate(directory, expected)
        for name, part in manifest["parts"].items():
            if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]*", name):
                raise ValueError("Invalid child path")
            child_hash = hashlib.sha256(f"{expected}\0part\0{name}".encode()).hexdigest()
            if part["asset"] != child_hash:
                raise ValueError("Invalid child identity")
            child_dir = directory / "parts" / name
            self._validate(child_dir, child_hash)
            self._install(child_dir, child_hash, request["source"])
        shutil.rmtree(directory / "parts", ignore_errors=True)
        for filename in ("result.json", "job.log"):
            (directory / filename).unlink(missing_ok=True)
        self._install(directory, expected, request["source"])
        shutil.rmtree(directory)
        return {
            "job": request["job"],
            "status": "done",
            "hash": expected,
            "measured": manifest,
            "timings": result["timings"],
            "resources": result.get("resources", {}),
            "cache_hit": False,
            "sandbox": self.sandbox,
        }

    def _install(self, directory, identity, source):
        target = self.assets / identity
        if target.exists():
            self._validate(target, identity)
            return
        pending = Path(tempfile.mkdtemp(prefix=".pending-", dir=self.assets))
        try:
            for filename in (
                "manifest.json",
                "lod0.glb",
                "lod1.glb",
                "lod2.glb",
                "collision.bin",
                "convex.bin",
                "mesh.bin",
            ):
                path = directory / filename
                if path.exists():
                    self._regular(path)
                    shutil.copyfile(path, pending / filename)
            (pending / "source.py").write_text(source)
            pending.rename(target)
        finally:
            shutil.rmtree(pending, ignore_errors=True)

    def _serve(self):
        while (request := self._queue.get()) is not None:
            identity = request["job"]
            started = time.perf_counter()
            try:
                key = asset_hash(request["source"], request["params"], self.config["BAKE_VERSION"])
                if key not in self._cache:
                    try:
                        resolved, _ = resolve(request["source"], request["params"])
                    except (ValueError, TypeError, SyntaxError):
                        pass  # Computed declarations are evaluated only inside the job.
                    else:
                        candidate = asset_hash(
                            request["source"], resolved, self.config["BAKE_VERSION"]
                        )
                        if (self.assets / candidate).exists():
                            manifest = self._validate(self.assets / candidate, candidate)
                            self._cache[key] = {
                                "status": "done",
                                "hash": candidate,
                                "measured": manifest,
                                "sandbox": self.sandbox,
                            }
                if key in self._cache:
                    cached = self._cache[key]
                    self._jobs[identity] = {
                        **cached,
                        "job": identity,
                        "cache_hit": True,
                        "timings": {"total_s": time.perf_counter() - started},
                    }
                    continue
                self._jobs[identity] = {"job": identity, "status": "baking"}
                self._process.stdin.write(json.dumps(request, allow_nan=False) + "\n")
                self._process.stdin.flush()
                result = _readline(self._process, self.config["BAKE_S"] + 5)
                if result["status"] == "done":
                    result = self._publish(request, result)
                    self._cache[key] = result
                self._jobs[identity] = result
            except Exception as error:
                self._jobs[identity] = {
                    "job": identity,
                    "status": "failed",
                    "errors": [
                        {"stage": "supervisor", "message": f"{type(error).__name__}: {error}"}
                    ],
                }

    def close(self):
        self._queue.put(None)
        self._thread.join(timeout=self.config["BAKE_S"] + 10)
        self._process.stdin.close()
        try:
            self._process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self._process.kill()
            self._process.wait()
        self._log.close()
        shutil.rmtree(self.jobs_root, ignore_errors=True)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
