# Bake worker

This module compiles submitted build123d source into real immutable geometry assets. It has no access to the simulation's tables.

On this Nix host, install Python packages and public CAD runtime libraries, then compile the pinned native geometry helper:

```sh
UV_CACHE_DIR="$PWD/.cache/uv" uv sync --python 3.12
.venv/bin/python -m bake.setup
.tools/bin/cmake -S bake/native -B .build/bake -G Ninja --toolchain "$PWD/cmake/NativeToolchain.cmake" -DCMAKE_BUILD_TYPE=Release
.tools/bin/cmake --build .build/bake
python3 bake/run.py -m pytest tests/test_bake.py
```

The native dependencies must first be built using `tools/bootstrap.py --build`. `bake/setup.py` installs public runtime libraries and bubblewrap through Nix and writes their local paths to `.cache/bake-runtime.json`; it does not read environment or credential files. The Python dependency graph is pinned in `uv.lock`.

`BakePool(assets, cgroup_root=...)` requires delegated cgroups by default. Its `submit` and `status` methods provide the queued geometry service boundary. Successful results contain the asset hash, measured manifest and sandbox status. Caller identity and model-registration requests belong to the adapter, so a cache hit can still create a new attributed model.

The author child exports a BREP and JSON declarations into its own directory, then exits. A second child forked from the clean warm parent loads and measures that BREP and writes the final assets into a separate directory. It never executes author source. Both children have the same restrictions and share one job deadline. Standard-shape recipes are untrusted claims: trusted code reconstructs the primitive and compares both solid differences before computing analytic capacity. Custom geometry uses the specified voxel flood. Authored manifests, monkeypatches and numeric capacity tags are never measurement inputs.

LOD generation welds and compacts OCCT's duplicated face positions before meshoptimizer runs; rendering normals are generated separately. LOD1 targets 40% while retaining topology and the configured collision error tolerance. The distant render LOD2 targets 10% without that error ceiling, so thin distant details may disappear. Neither simplification reduces an already-small mesh below twelve triangles. Artifacts use `ambergris-bake-5`, including measured liquid cavity bounds, so earlier immutable caches are not reused.

This host's cgroup mount is read-only and its user systemd bus is unavailable. `require_cgroup=False` is an explicit restricted-host test setting: it retains bubblewrap namespaces, Landlock, seccomp, wall/CPU and address-space limits, and labels every result. It does not satisfy the original delegated-cgroup gate. No unsandboxed fallback is selected automatically.

```sh
python3 bake/run.py -m bake.benchmark --restricted-host
```

That benchmark uses the real configured capacity grid and records coarse end-to-end job durations, LOD counts, bytes and cache timing. Production bakes do not collect per-operation CPU, RSS or phase timings. Its test sources are geometry workloads, never inhabitant policy. Inspect `.cache/bake-benchmark.json` for failures and individual samples.
