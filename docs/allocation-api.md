# Shared allocation reconciliation

This implements Technicals §8–9 and Specs S1.5, S3.2, S4.2. Contributions remain authoritative; allocations are generic derived resource rates, and their meters integrate actual served quantity. Native code names no resource or institution.

## Chosen semantics

A term with `share_equal` or `share_priority` is the demand side; its `paired` term supplies the available total. Both are combined quantities with matching dimensions. The demand side identifies the pair; an input index is rebuilt from registry definitions, so supply invalidation does not rely on an optional reverse `paired` field. Higher priority settings are served first, with full-handle order breaking equal priorities. Missing priority is zero.

An explicit demand watch saves `SharedWatchAnswer { aggregate, own_allocation }`, using `binding.consumer.object` as the demander. `self:get`, `self:read`, handler payloads and the aggregate part retain the ordinary shared total. An allocation-only change wakes only explicit consumers whose own allocation changed; no bare quantity handler is invoked. Declared thresholds filter aggregate changes as before; a changed own allocation independently wakes the consumer. A relation rebind compares the old composite answer with the new subject's answer. Other combiners and supply watches retain their existing answer semantics.

Calling `get_share` immediately after contributing during one handler sees the previous completed allocation. Reconciliation happens after that handler returns, before its work is published; the resulting watch executes through the ordinary causal lane. No handler re-enters itself and no mutation is rolled back on error.

Two alternatives were rejected. Waking all shared subscribers after every part edit needs no composite answer but invokes unrelated consumers and violates changed-answer suppression. Adding a second authored share-watch declaration is explicit but introduces an unnecessary scripting concept: the existing declared demand watch already identifies the quantity and consumer.

## Tables and exact APIs

`AllocationRows` retains rates plus derived subject/term and incident-object indexes. `PriorityLinks` is derived: demand terms indexed by either input term, and pair keys indexed by priority setting scope (instance full handle, model ID, plot ID, world). Each active priority demander indexes all four inheritance scopes, so adding/removing an override or editing a default can find affected pairs without scanning world objects. Reconciliation replaces one pair's priority links from its current demand parts. Rebuild these links once on initial load/restore or registry definition changes, without changing saved meters or waking scripts.

```cpp
struct AllocationReconciliation {
  react::AllocationRows& allocations;
  react::MeterRows& meters;
  react::PriorityLinks& priorities;
};
void reconcile_allocations(const react::ValueRead&, AllocationReconciliation&,
                           react::DeferredEffects&, Step, double hz);
void reconcile_priority(const react::ValueRead&, AllocationReconciliation&,
                         react::SettingKey, react::DeferredEffects&, Step, double hz);
void rebuild_priorities(react::PriorityLinks&, const react::ValueRead&);
```

Every actual part edit emits an internal `ContributionChange` draft, including when the aggregate is unchanged. Ordinary aggregate dirty work retains the exact S1.5 total-change rule. The adapter consumes the drafts, coalesces affected pairs, calls the generic allocator once per pair, folds meters before changing rates, and appends a demand dirty fact when allocations changed. `reconcile_effects` is shared by handler and native-action completion and runs before selector synchronization/publication. Native setting edits pass their exact scope to `reconcile_priority`; ordinary dirty instance settings use the same scope index. Binding/model/plot changes call `reconcile_object_priorities`, rebuilding the four scope links from current metadata. Relation edits affect subscriptions through the existing typed relation dependency index; scripts still own withdrawing and contributing on the new subject. Publication rejects unconsumed contribution drafts, so an unwired input path cannot silently retain stale allocations.

Installation and comparison APIs receive `const AllocationRows&` explicitly when they need composite answers. Allocation tables are not added to the general `ValueRead` projection. No adapter receives geometry, accounts, text, networking, or lifecycle allocation authority.

## Retirement and persistence

Withdrawn live demanders keep their accumulated meter at zero rate; rejoining continues that integral. Before a subject disappears, its contributors' reverse `gives_to` references are removed. Contributor withdrawal recomputes surviving subjects; retiring subjects and contributors remove their incident allocation/meter rows and priority links before slot reuse. Financial settlement, when authored, must read meters before destruction cleanup. No dead endpoint accrues future service or leaves an orphan authoritative row.

`retire_reactions` appends cleanup drafts directly to the current operation's `DeferredEffects&`. The fixed lifecycle coordinator receives that operation's `InvocationInput`, runs owner cleanup while identities remain valid, then releases rows. Structural completions and native removals therefore publish these drafts under their actual completed origin without an intermediate shared effects sink. The allocation owner retains no lifecycle callback interface.

SHRD persists allocations and meters, rebuilding their indexes on load. SCST persists the composite saved answers and observation revisions. Priority links and input indexes are rebuilt, not saved. Restore compares nothing and generates no catch-up or duplicate metering.

## Acceptance tests

Use newly invented terms, actual compiled Luau and installed explicit watches. Test equal and priority sharing, surplus redistribution, same-total demand replacement, same-total supplier replacement, changed inherited priority scopes, independent subjects, along-relation rebinds, withdrawal/rejoin integral continuity and retirement/slot reuse. Prove that unchanged consumers and bare quantity handlers stay silent. Run a real contribution handler through the completion adapter and causal watch path, and round-trip saved answers/meters before continuing. Benchmark indexed reconciliation with many unrelated subjects; verify delivered counts and conserved served totals.
