# Threshold timer retention review

The old paced `cced856e…` world stopped at step 43,624 with 13,110 pending timers.
An offline decoder read the completed TIMR and PROP sections in all ten retained
snapshots, using the current bitsery field layouts and exact registry IDs. No
world process or game table was changed. The [retained audit](../reports/timer-retention-audit-2026-11-15.json) includes
all ten snapshots; its scratch decoder is under `.cache/integration/`.

At step 33,600 there were 10,098 timers. Each following 1,200-step minute added
exactly 360. At step 43,624, 13,095 tokens had superseded process revisions, six
were current charge thresholds, and nine were ordinary recurring events. Every
threshold belongs to the six authored bodies' `base.charge` watches. The due
steps cluster at 2,854,074–2,857,144, about 39.7 world hours from startup. This is
observed stale-token retention, not duplicated body/school recurring timers.
For this fixed initial charge horizon the backlog eventually expires; describing
it as infinite growth would be inaccurate. Its long linear buildup and large
near-coincident due cohort are nevertheless a real game performance issue.

The actual path is `body_tick` → `self:set(charge,current-work)` → `self:rate` →
WatchWork → `threshold_timers` → `step::schedule`. Even at zero travel work,
`set` correctly stops the existing process and increments its revision; restoring
the rest rate then schedules a fresh crossing. The prior token becomes inert
through revision validation but remains in its wheel bucket. `set_rate` itself
already returns without mutation when rate and bounds are unchanged, so removing
that guard or adding another identical-rate guard would not fix this path.

Three reasonable remedies were considered:

* Skip the authored zero-work `set`. This preserves the rest process for idle
  bodies and removes this workload's needless revisions, but arbitrary authors
  legitimately changing rates could still accumulate long-lived stale tokens.
* Replace pending thresholds by their existing identity: source FactKey,
  consumer full handle, ScriptId and watch index. A derived wheel index points
  to level/bucket/vector offset. Schedule removes the previous row with
  swap/pop, fixes the moved threshold's offset, and inserts the new row. Bucket
  cascade/promotion removes and rebuilds only affected index entries. Expected
  replacement cost is O(1); ordinary authored timers retain their semantics.
* Use list nodes or periodic garbage collection. Stable list nodes simplify
  removal but add allocation/pointer overhead to ordinary timers. Collection
  introduces scan scheduling and can retain a large cohort between passes.

The recommendation is the authored zero-work correction plus keyed threshold
replacement. It uses no quantity names, authority context, persisted workflow or
new serialized fields. Snapshot restore already feeds rows through schedule and
rebuilds the derived index. Old snapshots can contain many versions sorted by
**due time**, which is not revision order: an old later-due token must not replace
a newer earlier-due token during restore. For the same identity, schedule keeps
the greatest binding/process revision pair. Normal revision checks remain in
place for tokens already promoted into the foreground queue and for the last
pending token after a stopped process or removed watch. They do not trigger a
full-world scan.

Equal-due timer insertion order is not part of S2's contract. Swap removal can change surviving bucket order; restoring at another wheel level can therefore choose a different permitted order among ties. Once promoted, ordinary work retains the required queue FIFO. The regression includes a threshold due at 1100 beside ordinary events due at 1110, saved at step 900 and replaced before cascade: both continued worlds must retain both ordinary events, exact deadlines, the newest threshold/cause and post-promotion FIFO. It does not require identical ordering of the two equal-due events. No sequence/sort subsystem or per-node list allocation is added solely for that unrequired guarantee.

Focused tests exercise real threshold scheduling and Luau rate changes,
replacement through multiple wheel levels, swap/pop with other timers, full
handle/script/watch identity separation, old due-sorted snapshot duplicates,
causal Origin preservation and an ordinary recurring timer alongside replacements.
The actual-Luau benchmark repeatedly rearms thresholds beyond an hours-long
horizon and verifies that pending count stays proportional to watched identities.
Its measured handler/watch/Journal path is distinct from full world-frame time.

The correction is implemented in `step/step.hpp` and `step/step.cpp`; no value,
registry, quantity or authority policy enters that owner. `body.luau` now skips
the charge write when travel work is zero, while retaining ordinary rest and
real movement costs. Optimized warning-as-error `step_test` and `turns_test`
pass, including old snapshot duplicates, all wheel levels, neighbor preservation,
full token identity separation, the mixed-due rebucketing case and real own/along
callbacks after replacement.

[Three fresh actual-Luau samples](../reports/threshold-timers-2026-11-15.json)
perform 10,000 alternating rate changes each and retain exactly two pending
thresholds. The next genuine crossing invokes each subscribed handler once with
its actual cause. Across samples, 100-operation batch-mean p50 is 11.18–12.03 µs
and p95 is 13.69–22.63 µs per rearm; the largest batch mean is 29.42 µs. This
includes actual VM invocation, process/watch work, in-memory Journal publication
and index replacement, with regression assertions inside the loop. It excludes
log disk I/O, networking, physics and the full frame. The host was shared with
compilation; these batch-mean percentiles are not individual-operation tails.
Whole-world long-run/recovery checks and the final sanitizer refresh are tracked
separately by their exact executable/plan fingerprints.
