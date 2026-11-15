# Native timeline profiling

Tracy instruments the same native executable used for play. The profile contains
step boundaries, work duration and queue counts; foreground/background drains;
individual Luau invocations tagged with their script ID; physics/navigation;
replication encoding/grouping; and snapshot capture, serialization and worker
compression/file publication. The step-work interval ends before pacing sleep,
while ordinary frame marks include that sleep.

The existing bootstrap switch enables instrumentation consistently in the pinned
Tracy library and its consumers:

```sh
python3 tools/bootstrap.py --build --tracy --jobs 4
.tools/bin/cmake -S . -B .build/release -DCMAKE_BUILD_TYPE=Release
.tools/bin/cmake --build .build/release -j 4
```

Collection is on demand. Start the world normally and connect the matching Tracy
0.15.0 profiler to its process on port 8086. Without an attached profiler, the
instrumentation checks whether collection is active and records no timeline.
Bootstrap without `--tracy`, followed by a consumer rebuild, compiles the markers
out entirely. The JSON timing reports remain available in either build.

This development host does not expose an invariant CPU timestamp counter. Use
Tracy's supported monotonic-clock fallback here, then rebuild the consumers so
the library and instrumentation use the same clock:

```sh
.tools/bin/cmake -S cmake/third-party -B .build/third-party \
  -DTRACY_TIMER_FALLBACK=ON -DTRACY_DISALLOW_HW_TIMER=ON
.tools/bin/cmake --build .build/third-party --target TracyClient -j 2
.tools/bin/cmake -S . -B .build/release -DCMAKE_BUILD_TYPE=Release
.tools/bin/cmake --build .build/release -j 4
```

The trace then uses the portable monotonic clock.

For a headless capture, build the pinned command-line recorder inside this
checkout. Its own dependencies are fetched into the local cache; existing pinned
zstd and JSON sources are reused. Disabling recorder LTO avoids requiring a
compiler-specific archive wrapper in the development environment.

```sh
.tools/bin/cmake -S third_party/tracy/capture -B .build/tracy-capture \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DNO_LTO=ON \
  -DCPM_zstd_SOURCE="$PWD/third_party/zstd" \
  -DCPM_json_SOURCE="$PWD/third_party/json" \
  -DCPM_SOURCE_CACHE="$PWD/.cache/tracy-dependencies"
.tools/bin/cmake --build .build/tracy-capture --target tracy-capture -j 2
.build/tracy-capture/tracy-capture -a 127.0.0.1 -o .cache/world.tracy -s 10
```

The recorder prints actual frame/zone counts and saves a trace that the matching
profiler can open. Its sampling work has a cost, so attached-capture measurements
and ordinary disconnected timing runs must be reported separately. No gameplay
deadline changes when profiling is enabled.

The current corrected executable `4170361d…` completed the live spatial checks
(stairs, power, optics and door collision) with 1,032 sampled steps: step-work
p50 16.3 ms, p95 28.8 ms, five schedule overruns and zero script errors. Maximum
snapshot pause was 1.77 ms. This is a paced 20 Hz mixed world with a 300-part school;
it is not a bandwidth or ordinary-GPU result. Evidence is in
[spatial.json](../.cache/run-refactor/live-final/spatial.json).

Separate serial microbenchmarks on the same source build measured p95 32.6 us
for one actual mutation/watch comparison/Luau delivery among 1,000 watches,
244 us for one controller writing environmental rates to 300 parts, and 25.4 us
for dirty allocation reconciliation among 1,000 subjects with actual allocation
and meter history. A rare inherited-priority change scanned that same populated
history in p95 30.4 us. These kernels exclude physics, transport, snapshots and
frame pacing; the part-rate kernel does not measure swimming geometry. The
[benchmark report](../.cache/run-refactor/benchmarks-final.json) pins executable,
source and plan hashes, sample counts, hardware and exact scope. These are single
runs on a shared host; scheduling noise is included.

The [historical refactored capture](../.cache/reports/tracy-capture-refactor-2026-11-15.json)
uses executable `1900a7c8…` and contains 205 frames and 1,188 zones over 10.09
seconds, with a measured timer resolution of 40 ns. The seeded world completed
300 paced steps with zero overruns or script errors; step work p95 was 6.200 ms.
Snapshot frequency was temporarily two seconds in that run's separate config to
exercise serialization and compression; the largest writer pause was 1.997 ms.
Only serial public-data HTTP acquisition ran alongside the native process and
recorder. The report retains commands, executable/asset/config hashes, recorder
output and the trace fingerprint. The 13.91 MB trace remains under
`.cache/integration/profile-refactor-2026-11-15/world.tracy`.

The same executable's separate [quiet 60,000-step run](../.cache/reports/native-long-refactor-2026-11-15.json)
had step work p95 2.435 ms over its final 4,096 samples, maximum snapshot pause
1.185 ms and no errors or overruns. That run had no network, residents or attached
recorder. The [mixed live workload](../.cache/reports/native-live-prototype-refactor-2026-11-15.json)
retains twelve swim-handler deadline cutoffs and two rejected model revisions;
its records do not establish their CPU cause. These distinct scopes must not be
combined into a zero-error claim for live play. The [earlier capture](../.cache/reports/tracy-capture-2026-11-15.json)
remains evidence for its original executable.
