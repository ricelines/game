#!/usr/bin/env python3
"""Install the pinned native toolchain and library sources inside this checkout."""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import tarfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCAL = ROOT / ".tools"
LOCK = json.loads((ROOT / "tools/dependencies.lock.json").read_text())


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def archive(entry: dict) -> Path:
    destination = LOCAL / "downloads" / entry["archive"]
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() and digest(destination) == entry["sha256"]:
        return destination
    temporary = destination.with_suffix(destination.suffix + ".part")
    request = urllib.request.Request(entry["url"], headers={"User-Agent": "Ambergris-bootstrap"})
    with urllib.request.urlopen(request, timeout=180) as source, temporary.open("wb") as output:
        shutil.copyfileobj(source, output, 1024 * 1024)
    if digest(temporary) != entry["sha256"]:
        temporary.unlink()
        raise RuntimeError(f"Checksum mismatch for {entry['archive']}")
    temporary.replace(destination)
    return destination


def unpack_tar(source: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(source, "r:gz") as bundle:
        for member in bundle.getmembers():
            relative = Path(*Path(member.name).parts[1:])
            if relative == Path("."):
                continue
            member.name = str(relative)
            bundle.extract(member, destination, filter="data")


def source(name: str, entry: dict) -> None:
    destination = ROOT / "third_party" / name
    stamp = destination / ".ambergris-sha256"
    if stamp.exists() and stamp.read_text().strip() == entry["sha256"]:
        print(f"{name}: cached", flush=True)
        return
    if destination.exists():
        shutil.rmtree(destination)
    unpack_tar(archive(entry), destination)
    stamp.write_text(entry["sha256"] + "\n")
    print(f"{name}: {entry['version']}", flush=True)


def cmake() -> None:
    entry = LOCK["tools"]["cmake"]
    executable = LOCAL / "cmake/bin/cmake"
    if not executable.exists():
        unpack_tar(archive(entry), LOCAL / "cmake")
    for name in ("cmake", "ctest", "cpack"):
        link(LOCAL / "bin" / name, executable.parent / name)


def formatter() -> None:
    executable = LOCAL / "bin/clang-format"
    if not executable.exists():
        with zipfile.ZipFile(archive(LOCK["tools"]["clang_format"])) as bundle:
            member = next(
                name for name in bundle.namelist() if name.endswith("/data/bin/clang-format")
            )
            executable.parent.mkdir(parents=True, exist_ok=True)
            executable.write_bytes(bundle.read(member))
        executable.chmod(0o755)


def link(destination: Path, target: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_symlink() and destination.resolve() == target.resolve():
        return
    if destination.exists() or destination.is_symlink():
        destination.unlink()
    destination.symlink_to(target)


def compiler() -> None:
    # The host supplies GCC and its Nix runtime; local links keep all build commands uniform.
    candidates_by_name = {"cc": ("cc", "gcc"), "c++": ("g++", "c++"), "make": ("make",)}
    for name, candidates in candidates_by_name.items():
        selected = next(
            (shutil.which(candidate) for candidate in candidates if shutil.which(candidate)), None
        )
        if not selected:
            raise RuntimeError(f"The host needs {name} before bootstrap can run")
        link(LOCAL / "bin" / name, Path(selected).resolve())
    link(LOCAL / "bin/g++", LOCAL / "bin/c++")
    probe = LOCAL / "check"
    probe.mkdir(parents=True, exist_ok=True)
    (probe / "cpp20.cpp").write_text(
        "#include <span>\n#include <array>\n"
        "int main() { std::array values{1, 2, 3}; return std::span(values).size() != 3; }\n"
    )
    subprocess.run(
        [
            str(LOCAL / "bin/c++"),
            "-std=c++20",
            str(probe / "cpp20.cpp"),
            "-o",
            str(probe / "cpp20"),
        ],
        check=True,
    )
    subprocess.run([str(probe / "cpp20")], check=True)


def godot() -> None:
    entry = LOCK["tools"]["godot"]
    destination = LOCAL / "godot"
    binary = destination / f"Godot_v{entry['version']}_linux.x86_64"
    if not binary.exists():
        destination.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive(entry)) as bundle:
            bundle.extract(binary.name, destination)
        binary.chmod(0o755)
    templates = destination / "templates"
    if not (templates / "web_nothreads_release.zip").exists():
        templates.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive(LOCK["tools"]["godot_web"])) as bundle:
            for member in bundle.namelist():
                if Path(member).name.startswith("web_"):
                    (templates / Path(member).name).write_bytes(bundle.read(member))
    runtime = destination / "lib"
    runtime.mkdir(exist_ok=True)
    if Path("/nix/store").exists():
        fontconfig = next(
            Path("/nix/store").glob("*-fontconfig-*-lib/lib/libfontconfig.so.1"), None
        )
        if fontconfig:
            link(runtime / "libfontconfig.so.1", fontconfig)
    wrapper = LOCAL / "bin/godot"
    if wrapper.is_symlink():
        wrapper.unlink()
    wrapper.write_text(
        "#!/bin/sh\nset -eu\n"
        'tool_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)\n'
        'export XDG_CONFIG_HOME="$tool_root/godot/user/config"\n'
        'export XDG_DATA_HOME="$tool_root/godot/user/data"\n'
        'export XDG_CACHE_HOME="$tool_root/godot/user/cache"\n'
        'export LD_LIBRARY_PATH="$tool_root/godot/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n'
        f'exec "$tool_root/godot/{binary.name}" "$@"\n'
    )
    wrapper.chmod(0o755)
    print(f"Godot: {entry['version']} with web templates", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--godot", action="store_true", help="also install Godot and web export templates"
    )
    parser.add_argument(
        "--build", action="store_true", help="compile reusable native dependency libraries"
    )
    parser.add_argument(
        "--tracy", action="store_true", help="enable Tracy profiling in the dependency build"
    )
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    args = parser.parse_args()
    jobs = [
        lambda name=name, entry=entry: source(name, entry)
        for name, entry in LOCK["sources"].items()
    ]
    jobs.extend((cmake, formatter, compiler))
    if args.godot:
        jobs.append(godot)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        for result in [executor.submit(job) for job in jobs]:
            result.result()
    if args.build:
        build = ROOT / ".build/third-party"
        subprocess.run(
            [
                str(LOCAL / "bin/cmake"),
                "-S",
                str(ROOT / "cmake/third-party"),
                "-B",
                str(build),
                "-DCMAKE_BUILD_TYPE=Release",
                f"-DCMAKE_C_COMPILER={LOCAL / 'bin/cc'}",
                f"-DCMAKE_CXX_COMPILER={LOCAL / 'bin/c++'}",
                f"-DCMAKE_MAKE_PROGRAM={LOCAL / 'bin/make'}",
                f"-DAMBERGRIS_TRACY={'ON' if args.tracy else 'OFF'}",
            ],
            check=True,
        )
        subprocess.run(
            [str(LOCAL / "bin/cmake"), "--build", str(build), "--parallel", str(args.jobs)],
            check=True,
        )
    manifest = {
        name: subprocess.check_output(
            [str(LOCAL / "bin" / name), "--version"], text=True
        ).splitlines()[0]
        for name in ("c++", "cmake", "clang-format")
    }
    (LOCAL / "toolchain.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("Local toolchain ready in .tools/bin", flush=True)


if __name__ == "__main__":
    main()
