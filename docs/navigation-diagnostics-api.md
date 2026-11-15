# Navigation completion reports

Technicals §6 requires a connectivity check after construction and an author-facing report of unreachable areas. The check uses the actual installed Detour polygons. It does not recognize stair models or infer walkability from an object's top or name. A small object without a walkable polygon is not an unreachable region.

Two approaches were considered. Probing guessed corners or upper bounds is cheap, but can misclassify cups, sloped surfaces and rotated assemblies. Checking actual polygons overlapping the changed physical bounds reports the navigation mesh that bodies really use, at the cost of a derived component traversal. The latter is implemented. It detects disconnected walkable regions; it does not infer an author's intended destination beyond a staircase ending in empty space.

The native API is:

```cpp
struct NavigationArea {
  Vec3 point;
  std::uint32_t connected_component;
  bool reachable;
};
struct NavigationReport {
  std::uint64_t revision;
  bool complete, reference_navigable;
  std::vector<NavigationArea> areas;
};
NavigationReport diagnose_navigation(const Space&, std::span<const Bounds> changed_regions,
                                      Vec3 reference, Deadline);
```

The reference is the builder's actual body position. Detour projects it using the same clearance class as navigation. Each returned area represents one actual component intersecting the changed bounds, padded by the configured body radius and raster tolerance. Its point lies on a real polygon. A missing reference surface is reported separately; it is not proof of unreachability. Component numbers are meaningful only for that navigation revision.

Connectivity labels and the traversal cursor are derived Space state, retained only for the current tile revision and excluded from snapshots. The first query builds them incrementally within its deadline. Later queries reuse them. A tile change invalidates them. Dirty tiles or an expired deadline return `complete=false`; they never produce a successful partial classification.

`bindings::NavigationDiagnostics` retains only const Space, IdentityRead and TransformRead, plus a transient map of pending requests. Fixed birth and rebind operations coalesce changed full handles and physical bounds for the actual immediate Player caller and action body. Before completion, only those pending handles refresh their current bounds, so another actor's intervening move cannot leave the check at an old position. An autonomous ScriptHolder is not treated as a player. Retired generations and missing scene geometry are removed. Initial reconstruction does not invent a new authoring action or replay old notifications.

After every tile-install pass, main calls `publish_navigation(context, install, deadline)`. Pending tiles and incomplete diagnostics retain the request for a later step. Completed `NavigationNotice {author,event}` DTOs go through `Sessions::event_for_player`, the existing play-session notification channel. No build waits for the report, and the diagnostic writes no game facts or chosen-action records.

The event contains `kind:"navigation"`, full `objects`, actual `revision`, `complete:true`, `reference_navigable`, `areas:[{point,component,reachable}]`, errors and readable text. Status is `connected`, `unreachable`, `no_walkable_area`, `no_reference_surface` or `rebuild_failed`. Native tile failures remain explicit. The shared `navigation.DIAGNOSTIC_BUDGET_US` gives reporting 1ms per step; it yields instead of extending an incomplete check indefinitely inside one frame.

The native regression builds a real staircase and landing, waits for asynchronous tiles, verifies a connected report for its builder, then adds a real wall and verifies disconnected polygon points. Removing the wall and using the normal rebind hook proves cache invalidation. It also covers dirty-tile and deadline retries, tiny nonwalkable geometry, missing reference surfaces, stale generations and autonomous caller identity. `navigation_diagnostics_test --bench` measures a first component build and subsequent queries over an 80×80m floor with 32 distinct elevated islands; physics/nav build time is outside that focused measurement.

The focused Release space and diagnostic tests passed in 0.34s. With Tracy disabled, the first component build and query took 390.472µs; 200 cached queries had median 68.078µs, p95 90.960µs and maximum 105.398µs. These are development-host library timings, not full frame measurements. Exact binary/source hashes and scope are retained in `reports/navigation-diagnostics-2026-11-15.json`.

The final portable, on-demand Tracy build passed all five focused space, physical, causality, illumination and navigation tests in 1.19s, including an intervening platform move while its diagnostic waited for tiles. Its first component query took 468.496µs; 200 cached queries had median 55.525µs, p95 102.741µs and maximum 123.572µs. The collector was detached and other workspace compilation could remain, so these separate runs are not an isolated estimate of profiling overhead. Evidence: `reports/navigation-diagnostics-portable-tracy-2026-11-15.json`.

The complete portable-profiling/native-cleanup sanitizer checkpoint rebuilt 50 native test/probe targets and passed all 47 registered tests in 23.075s with strict AddressSanitizer and UndefinedBehaviorSanitizer settings. It includes the live-bounds retry and current assembly/retirement paths. A retained source/config/dependency-export manifest confirms that no tested source changed during this checkpoint. Evidence: `reports/native-sanitizer-prototype-2026-11-15.json` and its log. This checkpoint precedes the separately investigated threshold-timer coalescing fix.

The final frozen-source checkpoint after timer coalescing and authoring/control publication fixes built 52 native test/probe/integration targets and passed the same 47 registered tests in 24.022s, plus both actual Bake2 importer/authoring integration executables. Its manifest remained unchanged during the final build and tests. Evidence: `reports/native-sanitizer-timers-2026-11-15.json` and retained logs. The native callers are instrumented; separately built third-party archives and the main server process are outside this sanitizer claim.


The later [actual native authoring journey](../reports/game-deadlines-2026-11-15.json) verifies production notification wiring on SHA `933642e7…` with the corrected Bake2 start plan. Mira submits real restricted-worker CAD, registers the model and places a 2×2m platform at world height 3m through the ordinary approved stock law. The completed author notification reports its real Detour polygon at `[-4,-7,3.06]`, disconnected component 5, `reference_navigable:true`, `status:"unreachable"` and no tile errors. The owned world stops cleanly with zero script errors. This is a functional integration result under concurrent compilation, separate from the library timings above and from strict cgroup/GPU acceptance gates.

The [final repeated live gate](../reports/game-deadlines-final-2026-11-15.json) passes on `f74178af…` and final seeded plan `1eaa07b3…`. Its new actual restricted CAD submission and ordinary placement again yield the real unreachable component 5 notice, alongside exact offer/grant deadline checks. The owned server stops cleanly at step173 with zero script errors. Independent resident activity may share the host; this run claims functional publication and real geometry, not isolated timing or strict cgroup enforcement.
