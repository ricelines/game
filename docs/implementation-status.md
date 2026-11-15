# Implementation and acceptance evidence

Ambergris runs as one native simulation with authored behavior, actual CAD,
physics and navigation, a model-agent gateway, a browser spectator, permissions
and commerce, fixture-tested elaboration, and snapshot recovery. The rejected
implementation was removed. This page distinguishes working game paths, measured
results and acceptance limits; each retained report identifies its own source,
plan and executable rather than borrowing a later build's identity.

## Architectural boundaries

The original five source tabs are retained beside
[`design-source.json`](design-source.json). Technicals takes precedence where
those tabs disagree. [`architecture.md`](architecture.md) describes the compiled
boundaries and [`architecture-review.md`](architecture-review.md) records their
independent review.

The executable allocates independent tables and sequences their owners. Cohesive
engine owners retain separate libraries; related integration code shares
`amber_bindings`. Four editing capabilities register the twelve editing actions.
Fixed lifecycle, scene, authoring-completion, publication and checkpoint operations
own their internal ordering instead of callers assembling overlapping callback
lists. Modules receive the columns and operations they use, through read
projections and masked writers. No engine or script receives those integration
coordinators as a server/world aggregate. Lifecycle creation and owner serialization
are the explicit complete object-table operations. The dependency check supplements
review of interfaces and control flow.

Luau and registry content own body verbs, conserved pouring, material responses,
power, oxygen, zones, lighting, construction, rent and treasury policy. Native
reaction code schedules declared thresholds; replication reads each object's
effective asset declarations. Neither algorithm recognizes a café quantity or body
verb. Authored zones supply names and illumination. The production resident
harness calls an actual model and supplies no purpose-keyword controller or
prewritten CAD policy.

The long-run audit found superseded threshold timers accumulating until their
distant due dates. The correction replaces pending thresholds by their existing
watch identity in the generic timer owner and removes the authored body's
unnecessary zero-work write. It adds no quantity name or persisted service.
[`timer-retention.md`](timer-retention.md) records the original failure, the
alternatives, revision ordering on restore and the measured replacement path.

## Playable paths

| Capability | Actual implementation and evidence |
| --- | --- |
| Invent and build | build123d/OCCT produces measured manifests, collision and GLBs. Authored approved laws consume stock and invoke structural operations. Actual residents submitted new CAD and placed its results. See [resident validation](resident-validation.md), [construction](construction-api.md) and [baking](bake-report.md). |
| Combine independently authored behavior | Typed values, closed-form processes, indexed own/along watches, thresholds and allocation execute actual Luau. The live power/oxygen chain combines independently authored producers and consumers. See [spatial validation](live-spatial-validation.md) and [allocation](allocation-validation.md). |
| Act physically | Jolt bodies and CharacterVirtual, Recast/Detour, attachments, compound parts, swept motion, custody and native collision queries serve ordinary actions. Body handlers select general mechanical primitives; pouring transfers conserved volume. |
| Observe and watch | Geometry supplies sight, glass/opaque occlusion and authored zone membership; sampled illumination follows authored sources. The Godot spectator imports runtime GLBs and authoritative declared state. Browser controls include following, inspection, free camera, debugging and resident purposes. |
| Trade and delegate | Player wallets, bounded delegated spending/issuance, groups, expiring grants, next-owner ceilings, mailboxes, offers and bilateral escrow swaps use authenticated actions. A live restart preserves and completes a pending trade. See [commerce validation](commerce-validation.md). |
| Edit during play | Instance/model/parameter/material changes, script installation, defaults, registry/rule edits, text, grants, groups and land use operation-specific contexts. Real two-owner live model revision preserves matching named parts and notifies measured-value watches. See [editing](editing-api.md) and [live authoring](live-authoring-report.md). |
| Fill an unmodeled gap | A chosen unhandled action creates a selected capture. Fresh native candidate/baseline fixture processes reject a vacuous rule and accept an improving immutable rule; retry then changes the action. Other sessions remain responsive. See [live elaboration](live-elaboration-validation.md). |
| Recover | Thirteen owner codecs preserve identity, catalog, VM state/RNG, queues, timers, processes, watches, allocations, permissions, custody and pending work. Restore rebuilds actual geometry/navigation. Full snapshots and selected captures use those same codecs. See [restart validation](live-restart-validation.md). |
| Attribute and measure | S7 records preserve chosen actions, derived handlers, roots and cross-author effects. Accepted malformed actions and failed CAD submissions produce actual denied records. Frozen S11 adapters process those records and real public datasets; undefined measures and failed expectations remain explicit. See [measurement](measure-report.md). |

The authored start package uses the same registry, script, asset and installation
paths as later authoring. Seeded/forage and stakes on/off are four selections from
one package. The seeded start installs 361 objects, including 300 replicated fish,
from 36 model entries and 31 immutable geometry assets. Bootstrap publications
are explicitly non-agent activity.

## Current build checks

The current coherent Release executable is SHA-256
`1900a7c8fcd3183aeee9a83a208b88fe5a6cc7d34c467cc5de3f1a2024943406`.
The authored seeded plan is
`1eaa07b36531a3ecfacc13034fecf7e1be36286e5126759a1b531f4be38bf189`.
All four current plans use `ambergris-bake-2`; their hashes are in
[`authored-plans-final-2026-11-15.json`](../reports/authored-plans-final-2026-11-15.json).
The bake version changed after an actual closed-solid capacity classification
failure was fixed; runtime floating-point geometry was not replaced by guessed
capacity values.

* Release: **47/47 registered CTest checks**, plus `start_test` and `authoring_test` using
  the actual prepared plan and baked assets. The latter are explicit integration
  invocations because they require those external build artifacts.
* Python: **117/117 tests** through the configured bake runtime. Ruff check and
  formatting pass for 88 files; native source formatting passes. See
  [`native-refactor-suite-2026-11-15.json`](../reports/native-refactor-suite-2026-11-15.json).
  The retained source manifest is unchanged across the coherent checks.
* ASan/UBSan: the current coherent rebuild covers all 52
  test/probe/validation/integration targets and passes **47/47 serial checks** in
  25.458s plus both actual Bake2 integrations in 1.918s/2.031s, with strict
  leak/error handling. Its final source manifest stayed unchanged during
  validation. Native module callers are instrumented; separately built
  third-party archives and the standalone main server are outside that gate.
  See [`native-sanitizer-refactor-2026-11-15.json`](../reports/native-sanitizer-refactor-2026-11-15.json).
  The [pre-consolidation checkpoint](../reports/native-sanitizer-timers-2026-11-15.json)
  retains its original source and scope.
* Actual clean and SIGKILL recovery both match the uninterrupted 1,400-step
  simulation: 361 full handles, property presence, values and positions agree
  exactly; both retain 20 timers and 10 allocations with no handler errors or
  overruns. The exact preceding compressed record is archived; the crash tail
  remains its separate abandoned branch. See
  [`game-recovery-refactor-2026-11-15.json`](../reports/game-recovery-refactor-2026-11-15.json).

The [current live authoring and fresh-process restore journeys](../reports/live-authoring-refactor-2026-11-15.json) pass, including
actual restricted failed CAD, accepted malformed actions, a staged family with
all parameter variants, two owners, mass-dependent watches, structural/resource
checks and connected S6 updates. Automatic variants are installed in one model
registration and create one chosen event. Separate explicit variant requests
remain separate choices. [All four current modes](../reports/native-refactor-modes-recovery-2026-11-15.json)
also match a 200-step checkpoint plus 50-step recovery with uninterrupted 250-step
worlds. [The current native/gateway/browser check](../reports/prototype-functional-refactor-2026-11-15-manifest.json)
passes bare-origin credential entry, actual assets and spectator controls, purpose
submission, persisted checkpoint/reload and causal-link clicking with zero browser
errors and no model calls. [The authenticated deadline/navigation gate](../reports/game-deadlines-refactor-2026-11-15.json)
passes ordinary grant/offer expiry and genuine CAD construction with an actual
Detour author warning. These reports use the exact current executable and plan
hashes. They are functional checks under sanitizer-compilation concurrency, not
quiet-host performance measurements.

## Performance evidence and its scope

The following current measurements use `1900a7c8…`. Quiet runs occurred after
compilers, other native worlds, CAD, browsers and model calls stopped; only serial
public-data HTTP acquisition continued. Passing a module benchmark does not
establish a full-game or browser gate. Earlier reports retain their original
executable fingerprints.

* The fresh seeded/stakes world completed **60,000 actual steps** (3,000 world
  seconds) in 70.343s, with zero handler errors or overruns. Step p95 was 2.435ms
  over the final 4,096 sampled steps; maximum snapshot pause was 1.185ms. This
  unpaced process had no network or residents. See
  [`native-long-refactor-2026-11-15.json`](../reports/native-long-refactor-2026-11-15.json).
  The [independent audit](../reports/native-long-refactor-audit-2026-11-15.json)
  decodes all ten late snapshots from steps 49,200–60,000: each has nine ordinary
  timers and six unique thresholds with current process and binding revisions.
  The old `f74178af…` and current public states are byte-identical, covering 361
  objects and 19,081 named values; their complete 60,000-step compressed snapshots
  and all thirteen saved sections are also byte-identical.
* The 500-socket probe received actual world deltas and every concurrent
  event-query reply, independently decoded replication baselines, and checked
  reconnect keyframes. It joined in 3.789s; query RTT p95 was 925.708ms, with at
  least 160 deltas per viewer. The measured median viewer duration was 8.087s,
  including initial keyframes, yielding median binary bandwidth 22,568 bytes/s.
  JSON, HTTP assets and transport overhead are excluded. See
  [`net-prototype-refactor-2026-11-15.json`](../reports/net-prototype-refactor-2026-11-15.json)
  and [transport validation](net-validation.md) for scope and earlier failures.
* The separate mixed live world completed 30,287 steps over 1,514.357s with zero
  overruns, but retains **12 `aq.swim` handler deadline cutoffs** and **two rejected
  model revisions**. It included actual authoring/revision/browser/500-viewer
  checks and sanitizer compilation. Its final sampled step p95 was 6.948ms and
  maximum snapshot pause 2.349ms. The [report](../reports/native-live-prototype-refactor-2026-11-15.json)
  retains exact S7 IDs and source hash; records alone do not establish CPU cause.
  The quiet run does not erase these mixed-workload failures. The [earlier mixed
  world](../reports/native-live-prototype-f74178af-2026-11-15.json) remains retained.
* Actual model-definition revision across 100 live instances, two owners and two
  parameter sets measured p50 6.417ms, p95 13.931ms and maximum 17.242ms over 100
  samples. This includes Lua law, Jolt reconstruction, retained parts/state,
  resource debit and catalog/trigger projection. Fixture setup and transport
  flush are excluded. See
  [`model-revision-refactor-benchmark-2026-11-15.json`](../reports/model-revision-refactor-benchmark-2026-11-15.json).
* Navigation tests use actual stair geometry. The authenticated edited-geometry
  and deadline journey checks disconnected-surface diagnostics, exact offer/grant
  due steps, ordinary before/after rights and notifications. Bounded permission
  and offer maintenance benchmarks retain
  their initial unbounded failure and corrected results. See
  [permission deadlines](permission-deadlines.md) and
  [navigation diagnostics](navigation-diagnostics-api.md).
* Three actual-Luau threshold-rearm benchmark runs retain two pending thresholds
  after 10,000 changes each, then invoke each real crossing callback once.
  Batch-mean p95 was 13.69–22.63µs per rearm. This includes VM/watch/Journal work,
  but excludes physics, networking and disk I/O. See
  [`threshold-timers-2026-11-15.json`](../reports/threshold-timers-2026-11-15.json).
* An actual on-demand Tracy recording contains 205 frames and 1,188 native
  zones over 10.09s. Its 300-step paced run had no handler errors or overruns,
  step p95 6.200ms and maximum snapshot pause 1.997ms under temporary 2s snapshots.
  [Profiling instructions](profiling.md) identify the trace and the host's
  supported monotonic timer fallback.

## Environment limits and external study

The retained earlier actual-world browser test measured SwiftShader: 9 FPS and
frame p95 133.4ms, with 371 streamed objects and 35 assets. It proves rendering,
connectivity and
controls; it does **not** pass the ordinary-hardware smooth-frame-rate gate.
Profiles identify software GPU presentation as the dominant cost. This host
cannot establish the ordinary laptop GPU result.

The host exposes no delegated writable cgroup. Strict baking therefore refuses
to run here. The explicit restricted-host mode retains namespaces, Landlock,
seccomp and CPU/memory/time limits and reports the missing cgroup isolation.
Actual geometry and worker-termination checks use that declared mode.

Actual model play has produced independently authored CAD, placement and adoption
of another resident's model. The unchanged six-person episode retains all model
contexts, submitted sources and actual S7 attribution. It has no completed paid
trade; opening a negotiation is not reported as a transaction. Historical runs
and the bounded continuation are distinguished in [resident validation](resident-validation.md).

The complete pinned Emergence, 1F3D9 and MoltNet public exports have been measured.
All three preregistered Emergence expectations failed and remain failed. The
Wikipedia draw contains 40 uniformly sampled eligible trees, 48,562 distinct
articles and 7,107 independently replayed response proofs. Final validation
processed all twelve official 2024 histories: 53 periods per world and 384,325
attributable events. All twelve correlations are unavailable with zero paired
changes. Each of the 2,120 weekly totals has a verified positive title count
without sufficient historical identity evidence; 92 actual page/response pairs
supply these obstructions. More title counts alone cannot repair those sums.
Readership collection stopped at 18,540 articles, leaving 30,022 uncollected;
complete annual acquisition is explicitly not claimed. The configured 1,000
bootstrap resamples of worlds were not attempted because the point estimates are
undefined. See the
[final source and validation report](../reports/wikipedia-validation-final-2026-11-15.json),
[measurement results](measure-report.md) and [acquisition details](wikipedia-acquisition.md).

The [README](../README.md) contains preparation, server, gateway and verification
commands. Resident play is an explicit gateway option; the server itself starts
no model service.
