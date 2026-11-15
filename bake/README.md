# Bake worker

This module compiles submitted build123d source into real immutable geometry assets. It has no access to the simulation's tables. Formats are specified in [bake-api.md](../docs/bake-api.md); the running HTTP registration adapter is documented in [gateway-api.md](../docs/gateway-api.md).

On this Nix host, install Python packages and public CAD runtime libraries, then compile the pinned native geometry helper:

```sh
UV_CACHE_DIR="$PWD/.cache/uv" uv sync --python 3.12
.venv/bin/python -m bake.setup
.tools/bin/cmake -S bake/native -B .build/bake -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="$PWD/.tools/bin/c++"
.tools/bin/cmake --build .build/bake -j 4
python3 bake/run.py -m pytest tests/test_bake.py
```

The native dependencies must first be built using `tools/bootstrap.py --build`. `bake/setup.py` installs public runtime libraries and bubblewrap through Nix and writes their local paths to `.cache/bake-runtime.json`; it does not read environment or credential files. The Python dependency graph is pinned in `uv.lock`.

`BakePool(assets, cgroup_root=...)` requires delegated cgroups by default. Its `submit` and `status` methods provide the queued geometry service boundary. Successful results contain the asset hash, measured manifest, timings and sandbox status. Caller identity and model-registration requests belong to the adapter, so a cache hit can still create a new attributed model.

This host's cgroup mount is read-only and its user systemd bus is unavailable. `require_cgroup=False` is an explicit restricted-host test setting: it retains bubblewrap namespaces, Landlock, seccomp, wall/CPU and address-space limits, and labels every result. It does not satisfy the original delegated-cgroup gate. No unsandboxed fallback is selected automatically.

```sh
python3 bake/run.py -m bake.benchmark --restricted-host
```

That benchmark uses the real configured capacity grid and records warm import, real jobs, collision/LOD/capacity work, bytes and cache timing. Its test sources are geometry workloads, never inhabitant policy. Inspect `.cache/bake-benchmark.json` for failures and individual samples.
