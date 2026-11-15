"""Install public runtime dependencies into the local cache on Nix hosts."""

import json
import subprocess
from pathlib import Path

from bake.config import ROOT


def main():
    packages = [
        "libGL",
        "libx11",
        "stdenv.cc.cc.lib",
        "zlib",
        "expat",
        "libseccomp.lib",
        "glibc.out",
    ]
    result = subprocess.run(
        [
            "nix",
            "build",
            *[f"nixpkgs#{name}" for name in packages],
            "--no-link",
            "--print-out-paths",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    libraries = [
        str(Path(line) / "lib")
        for line in result.stdout.splitlines()
        if (Path(line) / "lib").is_dir()
    ]
    cache = ROOT / ".cache"
    cache.mkdir(exist_ok=True)
    subprocess.run(
        ["nix", "build", "nixpkgs#bubblewrap", "--out-link", str(cache / "bubblewrap")], check=True
    )
    data = {"library_paths": libraries, "bwrap": str(cache / "bubblewrap/bin/bwrap")}
    data["loader"] = next(
        str(Path(path) / "ld-linux-x86-64.so.2")
        for path in libraries
        if (Path(path) / "ld-linux-x86-64.so.2").exists()
    )
    (cache / "bake-runtime.json").write_text(json.dumps(data, indent=2) + "\n")
    print("Runtime dependencies installed; use python3 bake/run.py -m pytest tests/test_bake.py")


if __name__ == "__main__":
    main()
