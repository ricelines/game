# Expiring grants and sale offers

Grant and offer checks enforce their recorded expiry step even if cleanup has
not yet run. At each writer boundary, permission maintenance removes due rows
from their lookup/list indexes, clears ordinary sale escrow, and sends normal
player events. Grant issuers receive a reminder before expiring script or
spending authority; the original issuer also receives a delegated grant's
reminder. Both expiry producers write nonchosen S7 system records.

Polling every grant and offer on every step would add a cost proportional to the
world's history. Scheduling custom permission events in the reaction timer wheel
would couple permission administration to script targets and term dispatch.
Instead, each permission owner keeps a derived ordered index keyed by its rows'
expiry steps. Creation inserts a deadline. Cancellation, acceptance, revocation
and retirement make stale entries harmless; due entries are discarded in order.
This keeps an idle maintenance pass independent of the number of stored rows.
Due grant cleanup retains the permission owner's existing bulk row/player-list
removal pass; it is not constant time in the total grant count. The bounded
benchmark measures that actual path, including its scans. Ordinary permission
checks remain allocation-free indexed reads.

The first bulk benchmark exposed a frame violation: expiring 100,000 grants in
one pass took 71–112 ms in three fresh runs. Maintenance therefore uses the
shared `permissions.DEADLINES_PER_STEP` work quota, initially 256 entries per
owner. Due work remains indexed for later steps; it is never refused. Stale
entries consume the same quota. The quota also bounds the number of reminder,
notification and event records produced in one step. The reminder lead is
`permissions.GRANT_REMINDER_SECONDS`, initially 30 seconds.

Automatic grant expiry drains only its due batch. A child cannot outlive its
parent, so all affected authority is already unusable even while some expired
rows await cleanup. Explicit revocation still removes the parent and its
children together. A sale expiry clears only the saved root/part full handles
that are still valid and belong to that quote; it does not move goods, money,
ownership or custody. Deadline work cannot clear a reused generation's flags.

The indexes are rebuilt from authoritative rows on restore and add no snapshot
payload. A before-expiry reminder may be delivered again after recovery into
its reminder window; reminders are notifications, rather than an exactly-once
valuable exchange. Restoration does not extend the authority's recorded expiry.

`perm_test` covers due/future/zero-budget/bounded batches, delegated authority,
revoked entries and restored indexes. `commerce_admin_test` passes the real
deadline producers through `Journal`, checking canonical event IDs/kinds and
grant identifiers. `commerce_test` exercises actual multipart Jolt goods across
offer expiry and re-offering. The repeatable owner-path timing probe is:

```sh
.build/release/grant_deadlines_bench
```

It reports idle and bounded reminder/expiry passes for 1,000, 10,000 and 100,000
real grants. Those timings exclude notification serialization and other world
phases; live authenticated integration verifies those separately.

The [three fresh bounded benchmark samples](../reports/grant-deadlines-bounded-2026-11-15.json)
create actual grants before measuring their permission-owner paths. At 100,000
live grants, idle p50 is 30–31 ns and p95 is 40 ns. All 100,000 reminders and
expirations complete in 391 passes each with a 256-entry quota. Expiry-pass p95
is 1.383–1.410 ms and the observed maximum across samples is 5.509 ms; reminder
p95 is 0.0525–0.0585 ms, maximum 0.0950 ms. Expiry includes the existing full
row and affected-player-list cleanup scans, so this is not an O(batch) claim.
The batch avoids the original 71–112 ms simultaneous-cleanup pause without
adding more reverse indexes. These development-host samples ran alongside a
two-job sanitizer build and exclude notice serialization, Journal I/O, transport
and other world phases; they do not establish full-frame timing.

The separate [fresh-world play-socket gate](../reports/game-deadlines-2026-11-15.json)
checks the integrated output path: a real grant warns before its due step,
authorizes a payment before expiry, denies one afterward, and leaves current
held/given indexes. A real sale releases its original goods at the deadline and
permits an ordinary replacement offer. The S7 notices retain canonical numeric
record IDs/System kind, typed grant/offer references and `agent:false`.
