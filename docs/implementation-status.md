# Prototype correction status

The prototype is incomplete and the existing implementation does not satisfy the
original design. The previous completion claim was wrong. Existing passing tests
and benchmark reports exercise that implementation; they do not establish design
compliance or complete acceptance.

The original five design tabs remain the requirements. Read the
[Technicals](02-t.0.md), [Specs](04-t.de137kjm03j.md), and repository
[instructions](../AGENTS.md) before changing a subsystem. Technicals takes
precedence over Specs. Other implementation documents describe prior code and
cannot authorize departures from either source.

Git commit `126e7bf` records the as-found implementation before correction. It is
a baseline for reviewing actual changes, not a reconstructed development history.

## Corrections in progress

- Route foreign script effects through ordinary actions with the script's grants;
  remove privileged mutation calls, inherited player authority and state exchange.
- Reduce integration to cohesive modules with direct calls; remove handwritten
  interface tables, redundant indexes and transaction or workflow machinery.
- Correct slow-viewer buffering, dropped cell removal, protective-limit records,
  plot membership, reach, current allocation reads, foreground starvation and
  sensor-driven zone membership.
- Separate untrusted CAD execution from trusted geometry measurements and make
  useful simplified levels of detail.
- Restore the S5 observation and result contract, gateway-owned compaction,
  optional schema discovery and configured defaults.
- Review public-tool classifications before rerunning the frozen measurement
  formulas. Previous comparisons used an incorrect classification table.

The current architecture correction removes the bindings, drive, command,
installation and lifecycle adapter libraries. There are nine native owners,
including elaboration. Native actions use typed operation families with direct
calls; execution frames contain invocation data, not world references. Family
functions still receive the tables used by their family, rather than promising
per-verb access restriction. The change is a modest source reduction, not a
claim that the former binding code has all disappeared.

The writer no longer implements connection handling, queue admission policy,
suspended script continuation, catalog synchronization, frame illumination or
seed-catalog installation. Those operations live with their existing owners and
take their actual tables. The writer still owns table lifetimes and composes
native completion, retirement and publication in explicit order. That composition
remains large; hiding it behind a universal context or a union of all tables would
recreate the rejected architecture.

Subscriptions and allocation rows retain their primary maps. PriorityLinks,
WatchLinks, their derived maps and the authored-detail secondary index are gone.
Trade cancellation retains the applied escrow detach instead of restoring prior
custody. Material edits update current bindings directly. Spatial queries have
no wall-clock deadlines, and the foreign `read(handle, term)` script host call
is removed. The runtime pin is `ambergris-0.3`: snapshots from the former row
layouts are rejected before decoding, without a migration layer.

Starting Play bodies receive the profile's authored `body_installs` choices during
initial provisioning, through the same slot validation and grant issuance used by
`engine.install`. There is no body-verb permission shortcut. Authentication and
snapshot restoration do not recreate grants, so revocation remains effective.
Grant text uses only `g:N`; the former spelling is rejected. Generated reports and
measurement outputs now live under ignored `.cache` paths.

Tests no longer require rollback of ordinary edits, stale-plan rejection,
foreign watch reads, or a particular freed-slot order. Replacement and nested
script tests check current object identities and applied effects. Retiring a
script invalidates its suspended invocations before another call can recreate
its state; a retired invocation cannot resume through Lua error handling.
Recovery checks compare the restored saved boundary and subsequent play, rather
than requiring identical future physics in independent runs. Explicit
`--steps 0` loads and reports a world without advancing it.

The frozen executable `4170361d…` passed the full native build and 46 C++ tests.
The Python suite passed 134 tests. Current process checks passed fresh-player
pickup and conserved pouring before any reinstall; ordinary revocation, finite
reinstallation and expiry; revocation surviving relogin and restore; nested action
continuation and script retirement; construction retaining already-applied
effects; stairs, power, optics, door collision and actual deadlines. Intentional
denials and handler errors in negative cases remain counted in their reports.
The paired elaboration subprocess checks also passed: a vacuous candidate was
rejected, a working candidate passed its four cases, the rule activated, and the
original action then succeeded. The first harness attempt stopped at a legitimate
reach denial; that evidence is retained separately from the passing rerun, which
used ordinary navigation through the door.

Clean and SIGKILL recovery restored the actual step-1,200 boundary exactly:
361 objects, positions, values, 19 timers and 10 allocation rows. Both worlds then
ran 200 valid steps. A genuine former-runtime snapshot was rejected before owner
decoding. The current spatial run's 1,032 frames measured step-work p50 16.3 ms and
p95 28.8 ms, with five schedule overruns and no script errors. These are functional
and timing checks, not viewer-bandwidth or ordinary-GPU acceptance results.
Evidence is under `.cache/run-refactor/live-final`; the focused benchmark report
is [benchmarks-final.json](../.cache/run-refactor/benchmarks-final.json).

The user resolved rent policy as leases: the landlord retains plot ownership,
tenants receive building and use rights, and unpaid rent ends those rights. This
supersedes the conflicting assumption that a tenant owns a plot while a revocable
script grant can forcibly reclaim it.

## Acceptance still required

The full unaided ownership journey, paid trade, multi-period resident play and
corrected emergence study have not passed. Wikipedia validation produced no
correlation. Browser smoothness on an ordinary GPU, strict baking with a delegated
cgroup, the approximate 100 kbps viewer target and a mixed workload without handler
cutoffs remain unverified or failed. None is converted into a pass by calling a
smaller functional check equivalent.

Historical outputs remain local evidence with their original workload, source and
executable identity. Source changes require new checks; old measurements are not
results for the corrected implementation.
