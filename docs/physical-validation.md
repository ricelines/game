# Observation, physical bindings and presentation validation

Focused modules use real pinned Jolt5.6.0, Detour1.6.0 and Luau0.747. Their fixtures invent term/model names and invoke actual Luau handlers. No geometry or script mock substitutes for the physical operations.

Release and ASan/UBSan checks cover observation, physical bindings, custody, native capacity and physical assembly editing. Current final suite counts belong to the CTest output because other owners add independent modules during composition. Jolt's pinned prebuilt library has RTTI disabled, so the imported target disables only UBSan's vptr check for consumers; address instrumentation and other undefined-behavior checks remain enabled in native modules. The release library itself is uninstrumented.

Observed timings below are microseconds on the shared development container, not hardware renderer measurements. Run `physical_test --bench`, `presentation_test --bench` and `observe_test --bench` for repeatable probes.

| Workload | p50 | p95 | maximum |
|---|---:|---:|---:|
|One Luau batch moving300 ordinary non-solid Kinematic bodies|403|634|930|
|The same batch plus a real Jolt step|836|1305|1634|
|Native preflight for17 physical assembly handles|2.29|3.42|5.96|
|Actual Jolt extraction/rebuild for17 physical assembly handles|73.3|121.8|221.9|
|Replication publication:100k offscreen running processes,32 visible processes,500 viewers|26.4|38.9|67.2|
|Observation:12 local objects,0 distant inert bodies|10.3|16.4|—|
|Observation:12 local objects,10k distant inert bodies|11.1|17.0|—|
|Observation:12 local objects,100k distant inert bodies|17.6|29.8|—|
|Selected32 MOVE capture with100k inert and32 dynamic bodies|3.53|4.19|24.5|
|Readonly32 motion queries with100k inert and32 dynamic bodies|3.71|4.59|23.9|
|Physics step:100k inert bodies,0 moving|1.21|1.22|1961|
|Physics step:100k inert bodies,32 moving|49.8|89.8|130|

The selected-motion probe runs `space_test --density`, querying32 actual handles
for1000 samples. Its10000-inert comparison measured selected capture2.79/3.21µs
and motion reads2.45/3.25µs (p50/p95). Idle and selected work does not grow in
proportion to inert population. The large isolated idle maximum occurred on the
shared container during parallel compilation; it is included rather than
discarded. Full checkpoint capture still intentionally visits all native rows.

The batch benchmark runs200 frames and includes permission preflight for every ordinary endpoint. A separate preflight failure regression confirms that a denied/invalid later row does not move earlier rows; timed invocations never borrow a player's direct-body authority. All poses update actual query-visible Jolt bodies. Non-solid updates avoid waking adjacent collision-solid objects. No native rule defines school paths or water bounds.

The replication benchmark registers100k actual closed-form processes outside viewer interest and32 inside it, then attaches500 viewers and performs1000 publications. Exactly32,000 entity rows are sampled, demonstrating that offscreen processes contribute no recurring value reads. The adapter shares interest helpers with net, samples each interested process at most once per step, drops terminal clamps and re-arms on changed rates. Binary serialization and socket delivery are measured separately in net-validation.md.

Observation timings are steady-state after the first real physics step. A query immediately after bulk insertion and before Jolt's initial broadphase maintenance measured around8ms at100k rows; this startup cost is not hidden in the steady-state values. Real transparent/opaque/partial occluders, live process-backed lights, authored closed sensor zones, heard typed payloads and changed-description caching have dedicated correctness tests.

The empty-root assembly-mass provider publishes a runtime answer through the ordinary source resolver/watch effects. Two10kg disjoint children give20kg assembly mass, while construction still sums0+10+10=20kg. Material and topology mutations invalidate only the changed closure and ancestors. Inert cached mass trees do no recurring work. Runtime rigid compounds keep root/child handles and original material transmission separate, preserve non-solid child collision behavior and sum all disjoint own masses once. A real10kg root plus two10kg children has30kg physical mass. Tests replace held geometry while retaining grasp/seat occupants, retire a removed seat without losing its external occupant, preserve flight across physical↔geometry-free root changes, keep inventory edits invisible and follow nested logical parts. The replacement benchmark performs100 actual extractions/native rebuilds with immutable imported geometry reused.

These remain module-level timings. The composition now feeds lifecycle/value notifications, installs the narrow bindings and loads authored metadata. Actual authored stairs, glass/opaque sight, multi-author power and door obstruction are covered by [the retained spatial journey](live-spatial-validation.md); [resident/browser evidence](resident-validation.md) reports real GLB play separately. Final causal/revision binary reruns and hardware GPU performance are tracked explicitly, rather than inferred from these probes.
