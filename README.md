# Ambergris

An authorable simulation: one C++20 server owns world state, ordinary Luau scripts own behavior, build123d sources produce measured geometry, and a Godot web client watches the same world that agents use through the gateway.

The prototype is under correction and does not yet satisfy the design or all acceptance gates. The [design doc](https://docs.google.com/document/d/1zJ2WDWIEMRXXinCnkyQ4KbKp2xhuI_rLnsroY57X0Mg) holds the requirements.

## Build

The supported setup is Linux x86_64 with Python 3.12,
`uv`, and Nix with `nix build`, flakes and the `nixpkgs` registry enabled.
The bake sandbox needs unprivileged user namespaces, Landlock ABI 3 or newer,
and seccomp. `bake.setup` installs public CAD runtime libraries and bubblewrap
through the host's Nix store; it writes their paths under `.cache`. The pinned
toolchain archives, native sources, Python environment, build outputs and game
assets stay inside this checkout. The compiler and runtime libraries use the
same Nixpkgs revision recorded in the checked-in toolchain lock.

```sh
python3 tools/bootstrap.py --build --godot
UV_CACHE_DIR="$PWD/.cache/uv" uv sync --python 3.12
.venv/bin/python -m bake.setup
.tools/bin/cmake -S bake/native -B .build/bake -G Ninja \
  --toolchain "$PWD/cmake/NativeToolchain.cmake" -DCMAKE_BUILD_TYPE=Release
.tools/bin/cmake --build .build/bake
.tools/bin/cmake --preset dev
.tools/bin/cmake --build --preset dev
client/build.sh
```

[Toolchain pins](tools/README.md) and [bake setup](bake/README.md) describe the native dependencies and sandbox. Baking requires a delegated cgroup by default. The commands below explicitly select `--restricted-host-bake` for this workspace host: it retains namespace, filesystem, syscall, CPU, memory and time limits but reports the unavailable cgroup isolation. On a host with a delegated cgroup, replace that option in both preparation and gateway commands with `--cgroup-root /path/to/delegated/cgroup`. Neither mode is selected silently, and an omitted cgroup path fails strict baking.

## Run a world

Prepare the authored world once. Cached geometry is reused; a cache hit still permits a distinct attributed model.

```sh
python3 bake/run.py -m content.prepare --mode seeded --stakes on --restricted-host-bake \
  --output .cache/content/seeded-plan.json
.build/dev/amb-server run --profile development \
  --plan .cache/content/seeded-plan.json --data .runtime
```

Use `--mode forage` for the sparse start and `--stakes off` for the authored alternative.

`--profile development` selects the finite 100 ms handler watchdog in
`config.toml`, for functional development on shared hosts whose unreported VM
pauses can be charged as script CPU time. The default profile is `production`,
which keeps the 5 ms CPU allowance. The build type does not select a runtime
profile. Configuration and profile are inherited by rule fixture subprocesses;
native run reports record the selected profile and allowance. Neither profile
retries interrupted handlers.

The server serves the spectator at `http://127.0.0.1:8080`. Open that page and paste the private value from `.runtime/keys/spectator.key` into the masked spectator-key field, then select Connect. The optional debug view requires a key authorized for debug records. Profiles generate these private files at launch; no credential is embedded in exported web assets, persisted by the page, or needed in a URL. The client has camera movement, object inspection, measured facts and maker claims.

Start the gateway in another terminal:

```sh
python3 bake/run.py -m gateway.service --bake --restricted-host-bake
bin/amb --key-file .runtime/keys/mira.key look
bin/amb --key-file .runtime/keys/mira.key actions
bin/amb --key-file .runtime/keys/mira.key do base.move_to --args '{"point":[0,8,0]}'
```

## Play with an agent

A person or agent connects with a player's key from a start-file profile or `amb join`.
An agent program that runs shell commands, such as Claude Code, Codex or Pi, can play
with `bin/amb --key-file KEY look`, `actions` and `do`. Agents that prefer MCP use
`/mcp` on the gateway with the same key. Models and credentials stay with whoever
runs the agent. Open the browser beside it to watch; the browser only spectates.

Native administrative actions use the same `do` operation. Known actions can be called directly from their listed signatures; `actions --name NAME` optionally explains their exact inputs. These include wallets, inventory, gifts, offers, bilateral trades, grants, groups, land, script installation, model defaults, settings, registry definitions and elaboration. The original technical design defines the authority boundaries.

The authoring service also supports ready parameter variants and live definition edits. `amb variant MODEL --params JSON --wait` bakes a parameter block for an existing model. `amb edit-model MODEL source.py --wait` bakes a proposed definition and returns the arguments for the ordinary `base.revise_model` action. Applying that action updates the model's current instances together, preserving ownership and matching named-part identities.

## Host a world

Install [Caddy](https://caddyserver.com/docs/install) on the Linux host. Keep the
server and gateway on their default loopback addresses. Prepare the playtest
with `--stakes off`, then start the server with `--profile development` and the
gateway as above. In another terminal:

```sh
export AMB_HOST=world.example.com
export AMB_JOIN_HASH="$(caddy hash-password)"
caddy run --config deploy/Caddyfile
```

Open `https://HOST/` to watch. Give players the join password, then they can use:

```sh
AMBERGRIS_JOIN_AUTH=playtest:PASSWORD bin/amb --gateway https://HOST:8443 join NAME --key-file KEY
bin/amb --gateway https://HOST:8443 --key-file KEY look
```

Caddy protects the gateway's join endpoint; the native WebSocket join remains
public as specified by the game protocol. Use the host's operator key to issue
roles or replace a lost player key: `bin/amb operator role p:N architect on` or
`bin/amb operator key p:N`.

## Records, checkpoints and fixtures

S7 records are compressed JSON Lines. Filters preserve the header and stream the records without a network connection or player credential:

```sh
bin/amb log --file .runtime/events.jsonl.zst --since 200 --actor p:2
bin/amb log --file .runtime/events.jsonl.zst --kind handler --thing o:36.1
.venv/bin/python -m measure ours --input .runtime/events.jsonl.zst --output .cache/measure-live
```

Snapshots are written under the selected data directory. Starting again with the same `--data` directory automatically restores its latest completed snapshot and archives the preceding compressed record. A clean stop completes a final snapshot. To restore a particular checkpoint into a separate output directory:

```sh
.build/dev/amb-server run --profile development --restore .runtime/snapshots/snap-1200.amb.zst \
  --catalog .runtime --data .runtime-resumed
```

The current runtime is `ambergris-0.8`. Earlier snapshot layouts are incompatible
and are rejected before decoding. Use a fresh data directory for a new world;
existing saves are not migrated.

Restore rebuilds real VM programs, collision, navigation, attachments, watches and runtime projections. Captures use the same thirteen owner encoders with selected rows. Rule approval launches fresh instances of this server for every candidate/baseline test; a candidate must pass all tests and improve at least one baseline result. `bin/amb fixture CAPTURE --job JOB.json --result RESULT.json --catalog DATA_DIRECTORY` exposes that runner directly. For development fixtures, add `--profile development --server .build/dev/amb-server`; `--config PATH` selects the same configuration used by the world.

For bounded headless runs, use `--no-network --unpaced --steps N --report PATH --snapshot-at-end`. World time still advances by the fixed simulation quantum; the report contains actual resolved values.
Omitting `--steps` runs continuously. Explicit `--steps 0` loads and reports the
world at its saved boundary without advancing it.

## Verify

```sh
.tools/bin/ctest --preset dev
.tools/bin/cmake --preset debug
.tools/bin/cmake --build --preset debug
.tools/bin/ctest --preset debug
python3 bake/run.py -m pytest
.venv/bin/ruff check bake content gateway measure tests
.venv/bin/ruff format --check bake content gateway measure tests
```

Native deadline tests retain their own strict limits in both build presets.
After substantial native changes, run the full Debug suite for ASan/UBSan checks.

Gameplay journeys that launch a server accept `--config PATH` and
`--profile development|production` (default: production). For functional checks,
select the Dev server already built above; omitting `--server` selects Release:

```sh
python3 bake/run.py tests/game_deadlines.py --server .build/dev/amb-server \
  --profile development --output .cache/integration/development-deadlines.json
```

For strict timing acceptance on a host with verified CPU accounting, build the
required optimized Release binaries together and select the production profile:

```sh
.tools/bin/cmake -S . -B .build/release -G Ninja \
  --toolchain "$PWD/cmake/NativeToolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DAMBER_SANITIZE=OFF
.tools/bin/cmake --build .build/release --target check-binaries
python3 bake/run.py tests/game_deadlines.py --profile production \
  --output .cache/integration/production-deadlines.json
```

Development journey reports are labeled `functional`; they do not certify the
5 ms requirement. Run the same journey with `--profile production` on a host
with verified CPU accounting for strict timing validation. A production profile
alone does not verify the host. Shared-host stalls can still exceed the relaxed
watchdog; failures remain failures and must be retained and investigated.

Expiry journeys wait for the grant's advertised simulation step. The body
permission journey uses a 0.5-second grant (`--grant-lifetime-s`); the deadline
journey uses a 0.5-second sale (`--offer-lifetime-s`) and a 1-second Spend grant
(`--grant-lifetime-s`). These test durations are configurable independently of
the production settings. Longer polling timeouts only bound failures.

The content-dependent importer and authoring checks require the prepared plan and actual assets; run them separately from CTest:

```sh
.build/debug/tests/start_test "$PWD/.cache/content/seeded-plan.json" "$PWD/assets" /path/to/current-world/registry/HASH.json
.build/debug/tests/authoring_test "$PWD/.cache/content/seeded-plan.json" "$PWD/assets"
```

The native suites use real Luau, Jolt and Recast/Detour. Performance records distinguish module benchmarks, live server/socket tests, model-driven world events and the browser hardware used. An unavailable dataset or failed hardware gate remains reported as such.

Ordinary JSON timing reports are always available.

To profile the 500-client density load, build the `profile` preset (Release
optimization with line tables and frame pointers) and attach `perf` with
frame-pointer call graphs once the joins finish:

```sh
.tools/bin/cmake --preset profile
.tools/bin/cmake --build --preset profile
uv run python tests/density.py --server .build/profile/amb-server --restricted-host-bake \
  --seconds 60 --no-reconnect
perf record -F 499 --call-graph fp -p "$(pgrep -x amb-server)" -- sleep 30
```
