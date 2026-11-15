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
