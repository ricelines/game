# Bake implementation evidence

Measured 2026-11-14 on this development host with the pinned build123d 0.12.0, Jolt 5.6.0 and meshoptimizer 1.3. The worker uses the real CAD kernel, meshes and binary collision artifacts. Commands and setup are in [bake/README.md](../bake/README.md); formats are in [bake-api.md](bake-api.md).

`python3 bake/run.py -m pytest tests/test_bake.py -q` passed **28 tests in 10.86 seconds**. Coverage includes actual standard/arbitrary solid measurement, thin-wall capacity, glTF coordinate conversion and parsing, Jolt restore and bounded convex hulls, moving-part mass/render separation, an empty assembly root, geometric validation, parameter identity, cache behavior, network/process/foreign-file denials, memory limits, timeout recovery and supervisor symlink rejection. Ruff and clang-format checks passed; native CMake targets built cleanly.

`python3 bake/run.py -m bake.benchmark --restricted-host --repeats 5` produced `.cache/bake-benchmark.json`. Each case used five fresh geometry jobs, not cache hits. Capacity used the configured longest-axis grid of 256.

| Case | p50 | p95 / p99 |
| --- | --- | --- |
| Standard solid box | 109 ms | 124 ms |
| Standard hollow cylinder | 187 ms | 219 ms |
| Arbitrary thin-wall cavity with voxel capacity | 2.349 s | 2.461 s |

With only five samples, p95/p99 are the sample maximum, not a strong tail estimate. Worker startup was 2.725 seconds, including 2.649 seconds importing the warm CAD runtime. An observed hot cache response took 5.4 ms including the benchmark's polling interval. The curved hollow's LODs contained 1004, 400 and 102 triangles; its complete asset was about 97 KB. The simple box kept its 12 triangles at all levels because topology/border constraints prevented further valid reduction.

Sandbox results are deliberately separate from geometry performance. Bubblewrap user/mount/PID/network namespaces, Landlock ABI 7, seccomp and CPU/address-space limits ran on the actual jobs. A direct attempt to create `/sys/fs/cgroup/ambergris-bake-probe` failed with `EROFS` (errno 30); the mount is read-only. `systemd-run --user --scope -p Delegate=yes true` failed because the user bus was unavailable. No delegated cgroup was obtained. Strict `BakePool` mode therefore refuses this host. The benchmark explicitly selected `require_cgroup=False`; every result marks `restricted_host: true`, and it does **not** pass the original per-job cgroup gate. It never used an unsandboxed execution fallback.

These measurements are the isolated bake checkpoint. World authentication, model-row registration, native Luau verification and the HTTP authoring adapter are now integrated through their owning modules. [The actual resident run](resident-validation.md) submitted genuine CAD artifacts through that path; [live variant/definition editing](live-authoring-api.md) adds current-source parameter blocks and approved-law family replacement. Their outcomes and final native refresh remain separate from the standalone bake timings above.

## Production-grid capacity correction

The final live authoring journey exposed a real measurement defect in an arbitrary solid `Box(.14,.12,.16, align=(CENTER,CENTER,MIN))`. The actual restricted warm worker returned capacity 0 at grid 64 but 0.0026377806 m³ at the production grid 256, despite the shape being solid (volume 0.002688 m³). The grid 64 unit-cube regression had hidden the defect. The retained reproduction is `.cache/integration/capacity-repro-1794696901822255102/`.

The top triangle plane and rounded float voxel center were 0.00031250715 m apart, exceeding the inflated half-cell 0.00031250625 m. The triangle-box test therefore rejected the closed top face, and the exterior flood entered the solid as if it were an open basin. Voxel centers, triangle-box projections and flood levels now use double precision; the stored/rendered/Jolt mesh remains float. Surface coverage and the specified priority-flood rule are unchanged.

`BAKE_VERSION` is now `ambergris-bake-2`, so corrected source/parameter submissions have new immutable hashes and cannot hit the old measurements. Existing immutable assets are retained as historical outputs. Long-running gateway/warm-worker processes must restart to load the new native library and version before new artifacts or start plans are prepared.

The real CAD bake suite passes 40/40 tests, including production 256 and 64 grids, small/millimeter/large solids translated above and below the origin, and translated open basins that retain the expected liquid volume. Three uncached restricted-worker production-grid samples per case give p50 0.251 s for a correctly closed small solid and 1.494 s for a translated open basin. The exact previously failing solid now has capacity 0; the open basin retains 0.0024355 m³. [The benchmark report](../reports/capacity-fixed-2026-11-15.json) records the native library hash, version, samples, resources, restrictions and retained artifacts. Timings are shared-host measurements, not a claim of strict cgroup availability.
