# Ambergris

An authorable simulation: one C++20 server owns world state, ordinary Luau scripts own behavior, build123d sources produce measured geometry, and a Godot web client watches the same world that agents use through the gateway.

The original design is preserved in [docs](docs/design-source.json). Start with the [architecture](docs/architecture.md), [authoring reference](docs/agent-authoring.md), and [implementation evidence](docs/implementation-status.md).

## Build

The supported setup is Linux x86_64 with Python 3.12, a C++20 compiler, Make,
`uv`, and Nix with `nix build`, flakes and the `nixpkgs` registry enabled.
The bake sandbox needs unprivileged user namespaces, Landlock ABI 3 or newer,
and seccomp. `bake.setup` installs public CAD runtime libraries and bubblewrap
through the host's Nix store; it writes their paths under `.cache`. The pinned
toolchain archives, native sources, Python environment, build outputs and game
assets stay inside this checkout. The host's `nixpkgs` registry selects those
runtime libraries; it is separate from the checked-in toolchain and Python locks.

```sh
python3 tools/bootstrap.py --build --godot --jobs 4
UV_CACHE_DIR="$PWD/.cache/uv" uv sync --python 3.12
.venv/bin/python -m bake.setup
.tools/bin/cmake -S bake/native -B .build/bake -DCMAKE_BUILD_TYPE=Release
.tools/bin/cmake --build .build/bake -j 4
.tools/bin/cmake -S . -B .build/release -DCMAKE_BUILD_TYPE=Release
.tools/bin/cmake --build .build/release -j 4
client/build.sh
```

[Toolchain pins](tools/README.md) and [bake setup](bake/README.md) describe the native dependencies and sandbox. Baking requires a delegated cgroup by default. The commands below explicitly select `--restricted-host-bake` for this workspace host: it retains namespace, filesystem, syscall, CPU, memory and time limits but reports the unavailable cgroup isolation. On a host with a delegated cgroup, replace that option in both preparation and gateway commands with `--cgroup-root /path/to/delegated/cgroup`. Neither mode is selected silently, and an omitted cgroup path fails strict baking.

## Run a world

Prepare the authored world once. Cached geometry is reused; a cache hit still permits a distinct attributed model.

```sh
python3 bake/run.py -m content.prepare --mode seeded --stakes on --restricted-host-bake \
  --output .cache/content/seeded-plan.json
.build/release/amb-server run --plan .cache/content/seeded-plan.json --data .runtime
```

Use `--mode forage` for the sparse start and `--stakes off` for the authored alternative.

The server serves the spectator at `http://127.0.0.1:8080`. Open that page and paste the private value from `.runtime/keys/spectator.key` into the masked spectator-key field, then select Connect. The optional debug view requires a key authorized for debug records. Profiles generate these private files at launch; no credential is embedded in exported web assets, persisted by the page, or needed in a URL. The client has camera movement, object inspection, measured facts, maker claims and resident purpose controls. Residents require the gateway below.

Start the gateway in another terminal:

```sh
python3 bake/run.py -m gateway.service --bake --restricted-host-bake \
  --purpose-token-file .runtime/keys/spectator.key
bin/amb --key-file .runtime/keys/mira.key look
bin/amb --key-file .runtime/keys/mira.key actions
bin/amb --key-file .runtime/keys/mira.key do base.move_to --args '{"point":[0,8,0]}'
```

For an actual model-driven population, add `--residents content/residents.json` and `--resident-report .runtime/resident-report.json`. Supply the model credential through the existing credential wrapper or your process supervisor. The harness accepts purposes and personas; it has no hidden CAD designs or purpose-keyword policy. [Gateway documentation](docs/gateway-api.md) covers CLI, HTTP, MCP, authoring jobs and model evidence.

Once the gateway is running, open `http://127.0.0.1:8080/?gateway=http://127.0.0.1:8081` and enter the spectator key to see residents and give them purposes. The URL supplies the separate agent service's address; it contains no credential. The bare `http://127.0.0.1:8080` page remains a spectator view without an agent-service connection.

Native administrative actions use the same `do` operation and advertise their exact inputs through `actions --name NAME`. These include wallets, inventory, gifts, offers, bilateral trades, grants, groups, land, script installation, model defaults, settings, registry definitions and elaboration. [Commerce](docs/commerce-api.md) and [editing](docs/editing-api.md) document authority and mutation boundaries.

The authoring service also supports ready parameter variants and live definition edits. `amb variant MODEL --params JSON --wait` bakes a parameter block for an existing model. `amb edit-model MODEL source.py --wait` bakes a proposed definition and returns the arguments for the ordinary `base.revise_model` action. Applying that action updates the model's current instances together, preserving ownership and matching named-part identities. The authored construction law accounts for the material change. See [live authoring](docs/live-authoring-api.md) for defaults, variants and failure behavior.

## Records, checkpoints and fixtures

S7 records are compressed JSON Lines. Filters preserve the header and stream the records without a network connection or player credential:

```sh
bin/amb log --file .runtime/events.jsonl.zst --since 200 --actor p:2
bin/amb log --file .runtime/events.jsonl.zst --kind handler --thing o:36.1
.venv/bin/python -m measure ours --input .runtime/events.jsonl.zst --output .cache/measure-live
```

Snapshots are written under the selected data directory. Starting again with the same `--data` directory automatically restores its latest completed snapshot and archives the preceding compressed record. A clean stop completes a final snapshot. To restore a particular checkpoint into a separate output directory:

```sh
.build/release/amb-server run --restore .runtime/snapshots/snap-1200.amb.zst \
  --catalog .runtime --data .runtime-resumed
```

Restore rebuilds real VM programs, collision, navigation, attachments, watches and runtime projections. Captures use the same thirteen owner encoders with selected rows. Rule approval launches fresh instances of this server for every candidate/baseline test; a candidate must pass all tests and improve at least one baseline result. `bin/amb fixture CAPTURE --job JOB.json --result RESULT.json --catalog DATA_DIRECTORY` exposes that runner directly.

For bounded headless runs, use `--no-network --unpaced --steps N --report PATH --snapshot-at-end`. World time still advances by the fixed simulation quantum; the report contains actual resolved values.

## Verify

```sh
.tools/bin/ctest --test-dir .build/release --output-on-failure
python3 bake/run.py -m pytest
.venv/bin/ruff check bake content gateway harness measure tests
.venv/bin/ruff format --check bake content gateway harness measure tests
```

The content-dependent importer and authoring checks require the prepared plan and actual assets; run them separately from CTest:

```sh
.build/release/server/bindings/start/start_test "$PWD/.cache/content/seeded-plan.json" "$PWD/assets"
.build/release/server/bindings/authoring/authoring_test "$PWD/.cache/content/seeded-plan.json" "$PWD/assets"
```

The native suites use real Luau, Jolt and Recast/Detour. Performance records distinguish module benchmarks, live server/socket tests, model-driven world events and the browser hardware used. An unavailable dataset or failed hardware gate remains reported as such.

[Native profiling](docs/profiling.md) documents the optional Tracy timeline build
and headless capture. Ordinary JSON timing reports are always available.
