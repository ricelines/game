# Reaction installation and watch delivery

`server/install` connects immutable catalog declarations to reaction dispatch and subscriptions. It depends on store/react/catalog/snap, without physics, permissions, text, finance, networking, VM execution or world composition. Its only authoritative new table is `InstanceScripts`, a sparse column of `vector<ScriptId>`. Model defaults remain in `catalog::Models`; rule versions remain in `react::RuleVersions`; last-delivered answers and binding revisions remain in `react::Subscriptions`. Everything else here is a rebuildable index or a copied output.

Two alternatives were encoding an auxiliary dependency subscription in the high bit of a watch number, or maintaining typed dependency links alongside ordinary subscriptions. Use typed links. The former consumes part of a public index and makes synthetic relation callbacks look like authored handlers. A `WatchKey{ScriptKey consumer,uint32 index}` identifies one declared watch. `WatchLinks` maps it to its resolved value source and a short list of dependency facts, with reverse dependency indexes. It contains no second authoritative last answer.

## Public operations and mutation scope

```cpp
using InstanceScripts = SparseColumn<vector<ScriptId>>;
void set_instances(InstanceScripts&, Handle, span<const ScriptId>, const catalog::Scripts&);
void project_model(react::Triggers&, const catalog::Models&, const catalog::Scripts&,
                   const react::Terms&, ModelId);
void project_instance(react::Triggers&, const InstanceScripts&, const catalog::Scripts&,
                      const react::Terms&, Handle);
RuleChange add_rule(react::RuleVersions&, const catalog::Scripts&, const react::Terms&, ScriptId);
void set_rule_enabled(react::RuleVersions&, RuleKey, bool);
void project_rules(react::Triggers&, const react::RuleVersions&);
WatchChanges sync_watches(react::Subscriptions&, WatchLinks&, const react::ValueRead&,
                          ScriptKey, const react::ScriptDeclaration&, Step, double hz,
                          const record::Origin&, bool preserve_existing);
void remove_watches(react::Subscriptions&, WatchLinks&, ScriptKey);
WatchChanges refresh_dependencies(react::Subscriptions&, WatchLinks&,
                                  const react::ValueRead&, FactKey changed, Step, double hz,
                                  const record::Origin&);
WatchResult check_changed(react::Subscriptions&, const react::ValueRead&, FactKey,
                          Step, double hz, const record::Origin&);
vector<InvocationPlan> plan_deliveries(span<const react::WatchDelivery>,
                                      const react::Triggers&, const catalog::Scripts&,
                                      const react::ValueRead&, Step, double hz);
vector<step::BackgroundItem> catch_up(const react::SharedColumns&, IdentityRead,
                                     const react::RuleVersion&, const record::Origin&);
bool catch_up_valid(const step::BackgroundItem&, const react::RuleVersions&,
                     const react::ValueRead&, Step, double hz);
void write/read_instances(snap::Output/Input&, InstanceScripts&, selected_handles?);
```

The implementation header will supply the concrete plain DTO fields and error types. No function takes every reaction table. `WatchChanges` contains copied dirty work and threshold timers, not queue references. `WatchResult` contains changed deliveries plus replacement timers. `InvocationPlan` supplies the actual installed receiver, sender/source, trigger term, typed payload, acquired handles and copied handler reference; root executes it using its narrow VM bindings. It is never a callback that captures the world.

`set_instances` validates existing ordinary script ids, rejects duplicates/world-rule scripts, and writes only the sparse installation row. Removing that row exposes the unchanged model default. Root checks installation grants separately. It also removes old instance dispatch/watch/state through their individual APIs; this operation does not gain authority over them.

Projection uses the complete sorted `ScriptDeclaration.handlers` derived from actual `on` functions. It does not guess a union of watches/events/actions. A function name is the corresponding registered term name. Model and instance projections replace only their own trigger entries; model defaults and per-instance scripts both apply, in installation order. Disabled entries do not run. Material/flag/model/instance order remains the existing `react::dispatch` order. No installation replays historical event/action callbacks.

`add_rule` groups the new script's actual handlers by selector/trigger, increments the matching version and appends immutable history. Only the latest version per key contributes to dispatch. Switching that version off preserves history and does not revive an older one. The pure install module does not approve source or touch the VM. Its separate runtime binding below performs already-authorized approval/activation and cached metadata updates. `project_rules` removes/replaces only world-rule material/flag entries, preserving model/instance entries. Changes return affected selectors/triggers so root synchronizes only applicable consumers, rather than scanning the world every step.

## Watch binding and relation changes

An ordinary watch starts at its installed object. An explicit `watch.along` follows that object's first outgoing edge. With no such edge it reads absence, rather than the object's own state. The ordinary source resolver then applies the term's source. For a combined term with `Term.along`, an own shared row wins (including an explicit zero row); otherwise its first outgoing related subject supplies the shared value. Resolving a watch records the exact value fact and the relation/own-row-existence dependencies that could change that fact. Explicit `along` and term fallback may both be present; they are resolved in that order, with the same single term-fallback step as `read_shared`.

`sync_watches` runs only when an installation, model binding, rule enable/version or restore changes applicability. New watches record the current answer as their baseline, without an invented callback. Preserving an existing watch retains its last answer and moves its ordinary subscription to the newly resolved value source. A followed relation rebind likewise preserves the old answer; it queues that new source as dirty so the ordinary watch comparison can detect a changed answer. If the answer is unchanged, no handler runs. A missing explicit endpoint uses a null-handle fact solely as an absent-value subscription; it never dereferences object slot zero or arms a timer.

`refresh_dependencies` looks up only consumers indexed under the changed fact, resolves their current target, transfers each binding with its last answer, and returns newly dirty facts/timers. It does not scan installed objects. This handles first-edge changes, detachment, an own shared row appearing/disappearing, and full-generation replacement. Ordinary relation watches remain actual authored subscriptions and receive their endpoint delta records; synthetic dependency links never become author callbacks.

`check_changed` resolves only the changed fact's current answer and uses the existing reaction comparator. Threshold-only consumers run when a declared predicate changes; unthresholded consumers run when the actual typed answer changes, including presence. All affected timers carry current process and binding revisions. Rate/bounds edits, rebind/removal and slot reuse invalidate older timers; the next crossing is rescheduled from the current process. Root checks a promoted timer token before converting it to dirty work. It does not dispatch that timer's placeholder event directly.

`plan_deliveries` keeps exact subscribed consumers and filters them against currently applicable enabled dispatch, in broad-to-narrow order. It does not redispatch the entire handler list once per delivery, which would multiply callbacks. An explicit declared watch is what subscribes a script; extra quantity handlers in `on` remain valid dispatch entries but do not silently create additional dependencies. Event/action dispatch continues through the ordinary complete trigger projection. Along readers execute on the installed object, with the foreign source acquired for reading, rather than executing their code as the source object.

Before destroying a source, composition removes its relation edges and refreshes dependent links while their prior subscription answer is still available, then removes remaining incident subscriptions through the existing lifecycle cleanup. Consumer removal drops its watches and dependency links immediately. Full handles prevent a reused slot from inheriting subscriptions or invocations.

## Late rules and restoration

For each newly active rule quantity handler, `catch_up` copies that shared quantity's existing slots once as full-handle background items. Each item carries the candidate script/version key and Origin. At execution, root skips changed generations, absent shared entries, unmatched selectors and superseded/disabled rules. It then uses the normal authored handler runner. Catch-up is not a per-step scan and never manufactures a chosen agent action.

Snapshots save `InstanceScripts` separately with selected-object filtering and rebuild sparse positions on restore. Existing reaction/catalog codecs save rule history, subscriptions' last answers/revisions, processes and script state. Restore reconstructs trigger projections and typed watch links, preserving authoritative answers. It does not replay events or enqueue fresh catch-up solely because a server restarted. Saved queue/timer entries continue with their validated revisions.

Tests use new arbitrary terms and real Luau handlers to show state-change delivery, threshold timing, explicit and fallback along rebinding, unchanged-answer suppression, removed/disabled/superseded rules, late-rule catch-up and selective snapshot continuation. A missing subject must not read slot zero. Benchmark dependency refresh against unrelated object count and actual watch/threshold delivery; no mock callback can establish the real VM integration gate.

## Explicit subscriptions and catch-up

Ordinary dirty quantity/flag/relation work consumes only an applicable explicit `watches` declaration. A complete `on` handler index supports action/event dispatch and explicit inspection; it does not create subscriptions. `has_applicable_watch(Subscriptions, Triggers, Scripts, ValueRead, FactKey, step, hz)` answers modeled status independently of whether the current value changed or crossed a threshold. No applicable explicit consumer on a shared fact leaves a gap for the S12 adapter.

`catch_up(SharedColumns, IdentityRead, Scripts, Terms, RuleVersion, Origin)` copies full source-column handles only for the activated rule's explicitly watched quantity trigger. A bare quantity handler and watched non-quantity trigger produce no catch-up. The writer synchronizes watch applicability on selector or activation changes, before dirty work runs; it must not lazily baseline a newly applicable watch after its first changed fact has already arrived. Restoring existing installations rebuilds derived links without replay or fresh catch-up.

## Runtime selector changes

`sync_rule_watches(Subscriptions&, WatchLinks&, ValueRead, Scripts, RuleVersions, span<const Effect>, step, hz, Origin)` is a pre-publication adapter over one completed output batch. It finds dirty facts and inspects current registry declarations. A quantity source supplies self and candidates reached through indexed incoming explicit watch relations and one authored term-fallback relation; current first-out and own-row precedence filter these candidates. Relation changes likewise find newly reachable consumers, including ones with no prior watch. Selector flag changes synchronize the affected object and remove consumers that no longer apply. It deduplicates exact object/script consumers and preserves existing last answers. It receives no spatial, network or lifetime service. It does not scan world objects or all subscriptions.

Run it before any dirty work from that handler/native completion is queued, then append its returned dirty facts/timers to the same deferred batch. Normal publication supplies the completed record's final Origin. This matters when a handler writes a quantity before making its selector flag true: waiting for a later flag watch would let the earlier quantity work encounter an uninitialized subscription.

A new binding in `SyncMode::Change` starts from an absent answer (an empty relation answer for relation watches), becomes dirty and arms thresholds. It therefore observes the current fact once when a runtime selector activates. Existing bindings preserve their last answers. `Initial` and `Restore` remain quiet. Registry rule activation copies explicitly watched shared-quantity source handles once to deferred background work. Each background item resolves its actual consumers through the same inverse relation indexes, binds missing watches from absence and compares only that rule script, so unrelated consumers retain their answers. Existing unchanged bindings are quiet; restore produces no new catch-up.


## Approved runtime rule activation

`bindings::update_rule` accepts only Scripts, RuleVersions, Triggers, Subscriptions, WatchLinks, WorkQueues and the writer VM, plus read-only ValueRead/AllocationRows and a narrow registry-publication callback. Validated activation approves only the already immutable, loaded candidate after fixture acceptance. Manual enable/disable requires prior approval. Enabling a historical script appends normal selector/trigger history; disabling never revives older entries. Cached VM approval/activation changes together with catalog metadata. The publication callback receives only the script id/flags, step, Origin and deferred outputs to save the registry pin and append its record.

WatchLinks maintains a derived by-script consumer index. Activation visits only affected subscribed consumers, preserves still-active watches and flat state, invalidates retired bindings/timers by their existing revisions, and cancels obsolete background versions. It never walks object slots. For each newly active declared quantity watch it copies that quantity's sparse shared-column handles once. `DeferredEffects.background` holds these jobs until final logging; `publish_effects` stamps the completed Origin and enqueues them. Events and bare quantity handlers have no catch-up. The alternative of matching all material objects on activation was removed because it violates the source's indexed catch-up rule.
