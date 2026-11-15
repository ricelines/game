# Space validation

Release build with host GCC15.2 and pinned Jolt5.6.0/Recast1.6.0, measured in the shared development container. Run `space_test` for focused checks and `space_test --density` for the repeatable density probe. Timings below are microseconds,100 timed samples for step probes,1000 for ray/motion capture. CPU sharing affects tails.

| Probe | p50 | p95 | maximum |
|---|---:|---:|---:|
|10,000 static, no moving bodies|1.23|1.24|203|
|100,000 static, no moving bodies|1.20|1.21|1920|
|10,000 static plus32 moving rigid bodies|41.6|53.2|90.5|
|100,000 static plus32 moving rigid bodies|47.8|62.6|92.7|
|Ray through32-object scene|1.23|1.62|27.4|
|One CharacterVirtual with navigation|14.1|31.7|236|
|Motion capture in32-object scene|0.18|0.23|1.65|

The static-scan baseline at100,000 inert objects cost73.6ms p50/79.6ms p95 per step, beyond the50ms simulation budget. The measured correction uses native active IDs, character IDs and changed rows. Idle cost now stays constant across these densities. The cold idle maximum includes broadphase maintenance after bulk construction; recurring steps remain around1.2us. No claim about100,000 moving bodies follows from this probe.

Focused tests use actual saved Jolt shapes and Recast-built Detour tiles. They cover gravity/support/sleep, removed-floor wake-up, physical vs transparent sight rays, deadlines, volume containment/entry/exit, predicted crossings, grasp/release/impulse, snapshot motion continuation, ordinary fixed/nested parts, geometry-free root movement, custody invisibility/cycle rejection, swept kinematic door obstruction without double displacement, actual CharacterVirtual ascent of ten stair risers, crowd passage through a doorway, forbidden travel volumes, target destruction and asynchronous stale-tile rejection. Idle debug output comes from actual Jolt triangles and installed navigation geometry.

Navigation source copying is filtered through the existing Jolt broadphase. Worker jobs own their geometry/pose copies and stale generations are discarded before installing. A separate navigation installation budget limits writer-thread publication; individual tile installation and copying costs still depend on the geometry inside that tile and should be profiled with authored scenes during integration.
