Toolchain bootstrap stays separate from application CMake and subsystem design.

```sh
python3 tools/bootstrap.py --build --godot --jobs 6
```

The host must supply Python 3.12 or newer, a C++20 compiler, and Make. The bootstrap
checks the compiler and exposes it through `.tools/bin/c++` and `.tools/bin/cc`.
It installs pinned CMake, clang-format, Godot, and Godot web templates in `.tools`.
All downloads and builds remain in this checkout. Source URLs, versions, and
SHA256 checksums are recorded in `dependencies.lock.json`. uSockets uses the exact
submodule commit selected by the pinned uWebSockets release.

Native libraries compile once in `.build/third-party`. Application CMake can
include `cmake/Dependencies.cmake`, then link the individual imported targets it
needs. There is no aggregate runtime target.

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
