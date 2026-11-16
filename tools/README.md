Toolchain bootstrap stays separate from application CMake and subsystem design.

```sh
python3 tools/bootstrap.py --build --godot
```

The host must supply Python 3.12 or newer, Nix, `nice` and `ionice`. Bootstrap installs the pinned
GCC 16.2, Ninja, ccache and mold from Nix's binary cache, and exposes them in `.tools/bin`.
It also installs pinned CMake, clang-format, Godot, and Godot web templates.
Tool archives and build outputs remain in this checkout; Nix supplies compiler
packages from its store. The Nixpkgs revision, source URLs, versions, and archive
SHA256 checksums are recorded in `dependencies.lock.json`. uSockets uses the exact
submodule commit selected by the pinned uWebSockets release.

Native libraries compile once in `.build/third-party`. Jolt builds without its
precompiled header so ccache can reuse its object files after a clean. Its first
build takes longer; ordinary application edits do not rebuild these libraries.
Application CMake can include `cmake/Dependencies.cmake`, then link the individual
imported targets it needs. There is no aggregate runtime target.

The native toolchain requires CMake 3.29 or newer and uses the pinned mold linker.
Application static libraries use thin archives referencing objects in the same
build tree; keep those objects alongside the archives. Imported third-party
libraries retain ordinary self-contained archives.

Both CMake presets use Ninja and ccache. `dev` is for the edit/build/test loop:
Debug code with assertions, `-g1` line information, and no sanitizers. `debug`
keeps full debug information and ASan/UBSan for sanitizer validation and
debugging local variables. Use `debug` for final native validation after
substantial changes. The local Ninja launcher uses Ninja's default parallelism,
CPU nice level 19 and idle I/O priority. An explicit `--parallel N` overrides the
worker default. Bootstrap's `--jobs N` controls tool/source setup concurrency.
Compiler results stay
in `.cache/ccache`; normal incremental builds retain `.build` outputs. Release
application builds are reserved for benchmarks or an explicit runtime need.

During an edit cycle, keep the build directory and queue the server, required
workers and affected tests in one build invocation. Both build presets without
explicit targets build all validation binaries. For example:

```sh
.tools/bin/cmake --preset dev
.tools/bin/cmake --build --preset dev --target amb-server amb_script_validate commerce_test perm_test
.tools/bin/ctest --preset dev -R '^(commerce|perm)$'

.tools/bin/cmake --preset debug
.tools/bin/cmake --build --preset debug
.tools/bin/ctest --preset debug
```

If Ninja repeatedly reports `premature end of file; recovering` and rebuilds
unchanged outputs, compact its dependency log, then build once to restore any
missing dependency records:

```sh
.tools/bin/ninja -C .build/debug -t recompact
.tools/bin/cmake --build --preset debug
```

Ninja records command durations automatically. To separate compiler work,
archiving and executable/shared-library linking:

```sh
python3 tools/build_times.py .build/third-party .build/debug .build/bake
.tools/bin/ccache --dir "$PWD/.cache/ccache" --show-stats
```

The report uses the latest recorded action for each output. Parallel action
durations are work totals, not elapsed build time; record elapsed time separately
when comparing builds.

| Imported target | Public include |
| --- | --- |
| `Ambergris::Jolt` | `Jolt/Jolt.h`, then other Jolt headers |
| `Ambergris::Recast` | `Recast.h` |
| `Ambergris::Detour` | `DetourNavMesh.h`, `DetourNavMeshQuery.h` |
| `Ambergris::DetourCrowd` | `DetourCrowd.h` |
| `Ambergris::Luau.Compiler` | `Luau/Compiler.h` |
| `Ambergris::Luau.VM` | `lua.h`, `lualib.h` |
| `Ambergris::WebSockets` | `App.h` |
| `Ambergris::bitsery` | `bitsery/bitsery.h` |
| `Ambergris::libzstd_static` | `zstd.h` |
| `Ambergris::meshoptimizer` | `meshoptimizer.h` |
| `Ambergris::cgltf` | `cgltf.h` |
| `Ambergris::TracyClient` | `tracy/Tracy.hpp` |
| `Ambergris::nlohmann_json` | `nlohmann/json.hpp` |
| `Ambergris::tomlplusplus` | `toml++/toml.hpp` |
| `Ambergris::picosha2` | `picosha2.h` |

The Luau compiler, VM, and Recast targets carry their transitive includes and
libraries. `cgltf` exports headers; define `CGLTF_IMPLEMENTATION` in one owning
translation unit. uWebSockets uses plain sockets and no WebSocket compression for
local development. A deployment can terminate TLS at its reverse proxy.

Tracy is disabled by default. Add `--tracy` when bootstrapping to enable its
instrumentation consistently for imported consumers.

`.tools/bin/godot` is a wrapper that keeps configuration, cache, and user data in
`.tools/godot/user`. Web export presets can use the custom template
`.tools/godot/templates/web_nothreads_release.zip`. The matching threaded and debug
templates are available in the same directory.
