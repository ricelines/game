# Reaction and scheduler API proposal

Status: reviewed contract implemented in `server/step/step.hpp` and `server/react/react.hpp`; those headers define the concrete API. This follows the revised `architecture.md`, `content-contract.md`, S2–S4 and the root decisions recorded in `architecture-review.md`. Names may be aligned with shared headers before approval; ownership and semantics must not change implicitly.

## Boundaries and shared data

`step` owns scheduling data and links only base data/record types and the standard library. It never calls reaction code or receives world tables. `react` owns registry, values, contributions, subscriptions and immutable Luau code; it can use the scheduler's data-only functions. Thin binding adapters connect individual physical/authority operations. There is no callback capturing the composition root, unified binding context or reaction state aggregate.

Use the existing `amber::record::Origin` verbatim in queued work and timers. Do not create a second cause/root/authors representation. Logging actor identity is `record::Actor`; authority identity additionally distinguishes an installed script's full object handle.

Shared records are ordinary values:

```cpp
struct FactKey { Handle subject; TermId term; }; // full generation in every key
struct ScriptKey { Handle object; ScriptId script; };
struct ScriptHolder { ScriptId script; Handle object; };
using Caller = std::variant<PlayerId, ScriptHolder>;

// S1 Value: tagged double, int64, bool or Handle. Presence is explicit.
struct Answer { bool present; Value value; };
struct RelationAnswer { std::vector<Handle> edges; }; // empty is meaningful

using StateKey = std::variant<std::string, std::int64_t>;
using StateAtom = std::variant<double, bool, std::string, Handle>;
using ScriptState = std::unordered_map<StateKey, StateAtom, StateKeyHash>;
using ScriptStates = std::unordered_map<ScriptKey, ScriptState, ScriptKeyHash>;

struct Process {
  double v0; Step step0; double rate, lo, hi; std::uint64_t revision;
};
struct Contribution { Handle from; double amount; };
struct Shared { double total; std::vector<Contribution> parts; };
struct SubjectTerm { Handle subject; TermId term; };
struct Meter { double amount0; Step step0; double rate; };
```

`Value` is the S1 primitive representation, not arbitrary JSON. Event payloads add a vector and a bounded flat typed record with registered field types; handles, scalars, strings and vectors are values, never native pointers, functions or authority tokens. Action arguments use their term's typed declaration. Validate external payloads once at ingress. Preserve typed handles rather than converting their 64 bits to a Lua number.

Independent tables, all owned by the composition root and passed separately:

| Table | Layout and mutator |
|---|---|
| `Terms`, `Materials`, `RuleVersions` | Separate monotonic-ID definition vectors; registry editing only. Rule history is immutable. |
| `Triggers` | S3.4's material/flag/model/instance maps by trigger; derived from declarations. No engine/content recognizers. |
| `Properties` | `vector<SparseColumn<Value>>`, indexed by term; own-state writer only. |
| `Processes` | `vector<SparseColumn<Process>>`; state/process writer only. Existing process rows become rate-zero when set to a static value, preserving revision monotonicity until object destruction. |
| `SharedColumns`, `GivesTo` | S1.5 subject columns and contributor reverse entries. A contributor is the object, not its owner or a model name. |
| `AuthoredRelations` | Named bidirectional relation columns; authored relation writer. The four physical relations have separate space-owned writers. |
| `Subscriptions` | Primary lookup `(source handle, term)` → consumer/script/declared-watch records, thresholds, binding revision and last-delivered answer. Relation changes remove old-source bindings and add new ones. No world scan. |
| `Allocations`, `Meters` | Subject/paired-term/contributor rows. Meter rates are actual allocations, folded only on allocation changes; finalized removed-contributor meters retain their full handle identity until explicitly retired. |
| `ScriptStates`, `RandomStates` | Flat scalar map per installed script/object; RNG state per object. Both authoritative and snapshotted. |
| `RuntimeMeasurements` | Read-only-to-authors numeric samples supplied by registered derived measurement providers. Distinct from baked asset measurements and state columns; derived caches rebuild on restore. |

Catalog, settings and material/asset metadata are read projections, not reaction-owned writable tables. Neither maker's text nor a ledger reference appears in a reaction binding.

## Scheduler public surface

The following names denote the specified plain records rather than owning facades:

```cpp
struct EventWork {
  Handle receiver, sender;
  TermId event;
  EventPayload payload;
  record::Origin origin;
};
struct WatchWork { FactKey source; record::Origin origin; };
struct ActionWork { ActionRequest request; record::Origin origin; };
using Work = std::variant<EventWork, WatchWork, ActionWork>;

struct ThresholdToken {
  FactKey source;
  Handle consumer;
  ScriptId script;
  std::uint32_t watch_index;
  std::uint64_t process_revision, binding_revision;
};
struct Timer {
  Step due;
  EventWork event;
  std::optional<ThresholdToken> threshold;
};

// WorkQueues owns a causal FIFO, remaining turns and a background FIFO.
// A turn is a queued external input or a due event, all plain data.
void append_inputs(WorkQueues&, std::vector<ExternalInput> arrived);
void promote_due(TimerWheel&, Step now, WorkQueues&);
std::optional<ForegroundItem> pop_foreground(WorkQueues&);
std::optional<BackgroundItem> pop_background(WorkQueues&);
PushResult enqueue_event(WorkQueues&, EventWork, std::size_t event_limit);
PushResult enqueue_action(WorkQueues&, ActionWork, std::size_t event_limit);
void enqueue_watch(WorkQueues&, WatchWork);
void enqueue_background(WorkQueues&, BackgroundItem);
void schedule(TimerWheel&, Timer);

IntentId start_move(Intents&, Handle body, MoveGoal, record::Origin);
std::optional<IntentEnding> cancel_move(Intents&, Handle body);
void finish_move(Intents&, Handle body, IntentEnd);
```

The driver increments the step, retains old foreground work first, appends newly due timers and then newly arrived inputs. `pop_foreground` drains the causal FIFO before taking another turn. Children emitted by an input therefore run before the next external input, subject to budget; work left over keeps that priority next step. Only the composition driver decides when to run space, observe and replicate.

Watches coalesce by full `FactKey`; their queue-membership index is derived from the queue and rebuilt on restore. Coalescing retains the first queued Origin; the handler still reads the current live answer when served. Distinct external actions normally drain separately. Watches are never dropped. Protective overflow counts emitted events/actions, logs a system record and drops only the excess event work. No queued operation is called applied before execution.

The timer wheel is S2's four levels of 256 slots. `after(0)` schedules no earlier than the next step; immediate delivery uses `emit`. Stale full handles or threshold revisions do no work. Clock/budget structures contain time and phase-cost samples only, never table references. Budget checks occur between work items; handler limits still stop an individual runaway. At least one background item runs when any is pending. No workflow planning or action retry is added.

## Pure reaction operations

These signatures expose the precise affected tables. Return values contain data and diagnostics only; no method obtains a world reference through them.

```cpp
Answer read_intrinsic(const Materials&, TermId material, const Term&);
Answer read_baked(const AssetMeasurements&, const Term&);
double read_mass(double baked_volume, double current_density);
Answer read_state(const SparseColumn<Value>&, const SparseColumn<Process>&,
                  Handle, Step now, double step_hz);
Answer read_setting(const InstanceSettings&, const ModelSettings&,
                    const PlotSettings&, const WorldSettings&,
                    Handle, ModelId, PlotId, TermId);
Answer read_shared(const SharedColumns&, const RelationsRead&,
                   Handle, const Term&);
Answer read_runtime_measurement(const RuntimeMeasurements&, FactKey);

bool set_state(Properties&, Processes&, const Term&, Handle self,
               Value, Step now, double step_hz, DeferredEffects&);
bool set_rate(Properties&, Processes&, const Term&, Handle self,
              double per_second, double lo, double hi,
              Step now, double step_hz, DeferredEffects&);
bool set_contribution(SharedColumns&, GivesTo&, const Term&,
                      Handle from, Handle subject, double, DeferredEffects&);
void allocate(const Shared& supply, const Shared& demand, Combine,
              const PriorityValues&, AllocationRows&, MeterRows&,
              Step now, double step_hz, DeferredEffects&);
void set_relation(Relation&, Handle from, Handle to, bool present,
                  DeferredEffects&);

std::vector<HandlerRef> dispatch(const Triggers&, IdentityRead,
                                std::span<const TermId> matching_flags,
                                Handle receiver, TermId trigger);
WatchPlan check_watch(Subscriptions&, FactKey, const CurrentAnswers&);
std::optional<Step> next_crossing(const Process&, const Threshold&,
                                  Step now, double step_hz);
void rebind(Subscriptions&, const ScriptDeclaration&, Handle consumer,
            const RelationChange&, DeferredEffects&);
void retire_object(Properties&, Processes&, ScriptStates&,
                   RandomStates&, Handle);
void withdraw_contributor(SharedColumns&, GivesTo&, Handle, DeferredEffects&);
```

`set_state`/`set_rate` accept only state terms without contribution combiners. They target the invocation's self or declared parts, validated by their binding, and preserve changes on later errors. State reads prefer an existing process, then an explicit property, then absence. Measured and intrinsic properties cannot be written this way. Runtime measured samples use their separate provider-only path. Relation writers receive the selected authored relation, not unrestricted access to physical relations.

The effective read dispatcher uses a term's source/role to call these functions. It does not retain their table references in one broad object. A term with a combiner reads its shared answer, including S1's `along` fallback; it cannot also be state/process storage. Relation answers have their own empty-set representation. Threshold consumers can explicitly follow a declared relation to a process; they need not pretend that the process lives on themselves.

`next_crossing` schedules the first future step where the declared threshold predicate changes, in either direction, respecting process bounds. A rate change, relation rebind, deletion or rule activation invalidates the old token. A threshold timer validates source/consumer generation, process revision and binding revision before reevaluation. A new rule copies the matching column's handles once and catches up through background work. No process is materialized every step.

Allocation meters integrate actual served amounts, not offered supply. Supplier attribution is proportional to supplied shares of the total served amount; consumer meters use their own allocations. Meter reads compute `amount0 + rate * (now-step0)/step_hz`. Fold before changing a rate. Unused supply earns no delivered quantity. These are generic resource measurements, not billing or treasury policy.

Cleanup remains separate: relation owners remove incident edges; subscriptions/trigger owners retire their bindings; grants/offers and space mirrors use their own operations. The lifecycle driver performs all cleanup before making a slot reusable. `retire_object` is not a universal destructor.

## VM invocation and binding surface

`Vm` owns only Luau execution data, immutable compiled modules and VM budgets. It owns no world tables. `Invocation` is transient metadata: self/script, trigger payload/acquired handles, optional direct body endpoints, inherited Origin and deadline. It contains no adapter collection. `DeferredEffects` has an ordered vector of event, watch-dirty, timer, action-request and diagnostic drafts plus a separate ordered structural-command vector; preserving call order avoids reordering `emit`, writes and actions. Structural requests contain values only and are applied after the VM returns, never by an all-table constructor callback.

```cpp
ScriptValidation validate_script(std::string_view source, const Terms&,
                                  const RightsDeclaration&);
CompiledScript compile_script(std::string_view source);
void load_script(Vm&, const CompiledScript&, const ScriptDeclaration&);
void register_binding(Vm&, std::string_view, lua_CFunction, void* binding_local);
RunOutcome run_handler(Vm&, HandlerRef, const Invocation&);
void publish_effects(DeferredEffects&&, const record::Origin& completed_handler,
                     WorkQueues&, TimerWheel&, record::Journal&,
                     std::size_t event_limit);
```

`binding_local` has one concrete binding's exact references, with lifetime owned by the composition root. The runtime stores it opaquely and cannot cast it to a universal host. The same applies to native action callback bindings. Invocation metadata and its output buffer may be obtained through thread-local VM userdata; that userdata contains no subsystem pointers.

| Binding | Exact mutable/read data |
|---|---|
| Intrinsic/measured/setting reads | Their source-specific const projections, identity and acquired handles. No text/authority writes. |
| Own state / rate | Properties, processes, term definitions, self/parts membership and output buffer. |
| Contribution | Shared columns, contributor reverse entries, combine/allocation definitions and appropriate allocation/meter rows. |
| State proxy / random | The selected script state row or object's RNG row only; not every other object's state. |
| Events / timers / `self:act` | Invocation metadata and deferred output buffer; script actions always use `ScriptHolder`. |
| Spatial primitives/query | Its individual space operation, exact identity/transform/geometry views, and bounded invocation endpoints. |
| Financial primitive | Ledger/grants plus supplied current controller descriptors; never initiating-player authority inherited from a body callback. |

The state proxy supports field access, string/integer indexing, immediate scalar assignment and nil deletion. Reject tables, functions, buffers, threads and unknown userdata at assignment. Integer `1` and string `"1"` are different keys. `table.*` helpers are not this proxy's API; ordinary scratch tables retain them. Generic `for key,value in self.state do` can use `__iter`; snapshotting iterated keys avoids invalidating an unordered-map cursor if the script changes keys. No Lua heap copyback is required after error/cutoff, so earlier state writes remain.

Physical bindings accept the invocation's body, selected target and current held source only, with operation-specific endpoint checks. A handle acquired by reading/querying grants no mutation authority. `self:act` queues a normal installed-script action; it cannot borrow the player's direct-operation scope. No actor ID supplied by Lua can mint such a scope.

Conserved transfer is a narrow direct operation over one registered conserved, non-shared state term and two checked endpoints. Settle their processes, preflight scopes, then make equal debit/credit without yielding. Capacity/compatibility/spill remain script decisions. Ordinary own-state `set`/`rate` remain as the original design permits; this is a local transfer guarantee, not a new global conservation/source/sink system.

## Causal records without staged state

1. Run a handler with live tables. All state/physical changes happen immediately. Keep only emitted work/timers/actions and diagnostic drafts in its ordered output buffer; dispatch does not re-enter it.
2. For an event/watch whose parent Origin is known, append its completed handler Event/Credits with `Journal::append`, including error/cutoff and actual changed state. Stamp its buffered outputs with the returned Origin and publish them.
3. Apply a handler's immediate structural primitive outputs after its VM returns, before finalizing its direct `changed` summary. Buffer their completed creation/destruction record drafts until their parent Origin exists. For a chosen action, keep the bounded direct-dispatch handler outcomes until that action's result/changed flag is known. Append the final action first; append each handler with its action Origin, then stamp/publish each handler's outputs in execution order. Do not wait for emitted events or ordinary action descendants. A causal script action uses its inherited Origin rather than becoming a chosen root.
4. Drain the queued consequences before the next external input subject to the normal step budget. Overflow/error diagnostics are children of the completed handler. A later timer retains this same Origin through snapshots; no historical record lookup is needed.

Only direct state changes count for that action/handler's `changed`, excluding the acting body's own movement. Successful application of its structural primitive is a direct effect; an emitted event or `self:act` is later causal work with its own record. Thus successful free construction can count as building without retroactively crediting a parent for a later reaction chain. No world writes are staged, undone or held awaiting logging. A record buffer is not a transaction. Any pending direct-dispatch continuation must remain plain queued data, with no live coroutine or initiating-player authority carried into a later reaction.

## Authored construction law and queued structural changes

Creation is a mandatory authored-law path, not an unconditional native `place` implementation. The geometry-only instantiate primitive is available only to a currently executing active approved world rule. The binding checks that fact using invocation metadata plus the registry entry; Lua cannot supply its script kind, rule identity or approval. An ordinary model/instance script fails this check even if it holds build rights. The world-rule path must also hold the ordinary plot/build authority. This uses the existing elaborator/registry trust boundary, with no new sealed ticket or constructor capability hierarchy.

A concrete starting trace is:

1. An agent calls an authored construction action with model, parameters and destination. The starting construction rule applies to the body's authored fabricator flag. The body's use right is checked normally. Its active approved world rule is the handler; no handler is asked to cancel another, and native code contains no fabricator flag or constructor identity.
2. The rule reads the ready baked model's disjoint material masses, material embodied energy and the builder's available stock. Hand fabrication debits body charge and credits material stock in that installed rule's flat state. Placement consumes the measured mass from that stock. This follows Technicals §18's body/hand construction; a powered constructor can later supply a different authored policy through the same primitives.
3. The rule preflights current geometry, destination build authority and resource availability. A direct request can use only its documented direct build scope; a timer or script-origin request must use the script's ordinary grants. The rule cannot obtain build rights from its fabricator label or from the initiating player's identity alone.
4. If stock is insufficient, the rule performs no creation. Otherwise it queues a typed creation request with the measured binding, destination and an optional `ResourceExchange` describing expected current answers and the rule's chosen replacement values. This primitive returns queued status, not a handle to an object that does not exist. It does not yet debit stock. Hand fabrication's separate `self:exchange` applies its own charge/stock changes immediately.
5. After VM return, the structural operation rechecks the invoking installation, expected resource answers, ordinary Build authority and actual native capacity. It allocates and initializes the object through each owner's narrow functions, then applies the preflighted resource exchange without yielding. A failure before this commit leaves the proposed stock change unapplied. A second queued request based on the same old balance fails its stale-answer check after the first succeeds. The completed creation gets its own causal record/handle, which the direct input response can include. No callback receives all tables, and no external input interleaves the structural application.

Preflight is a live query, not a reservation; there are no retained tickets or rollback semantics. The small exchange belongs to one direct structural operation, rather than staging arbitrary handler writes. Earlier ordinary state writes remain live on error/cutoff. Dismantling checks its resource exchange before lifecycle retirement and applies it while its holder still exists, before cleanup/free; this permits dismantling the holder itself without losing the credit. The script selects all charges and policy. If no construction law is active, the request produces a gap and no object. Stakes-off installs an authored free-construction law; it does not bypass the path through a native switch.

This is an authored game law, not adversarial conservation of body charge. Technicals §13 permits scripts to change their own state, so an owner-installed script can change its body's charge. It still cannot invoke native creation or replacement without the currently active approved world rule and ordinary build authority. Locking charge in an architect-owned institution would impose a different gameplay/trust model and is not the starting implementation. No new term-ownership system, global accounting scan or recipe whitelist follows from the construction law. The request path can create arbitrary approved geometry on any plot for which ordinary build authority is held.

Mass/material-changing edits and model-binding replacement must use the same approved law gate, with the authored rule deciding resource deltas from new baked measurements. Pure transform editing remains a mechanical authorized operation. Asset publication may update cache data, but must not silently commit a resource-changing binding edit outside this path. Ordinary destruction remains a permission-checked valuable operation, including the owner's ability to remove junk; recovery of resources is authored.

Creation/deletion commands are applied through explicit lifecycle operations after VM return, not arbitrary foreign-state setters. Their immutable request records carry the caller, approved rule provenance, ordinary scope descriptors and Origin. They are immediate structural outputs of that input, not a retained initiating-player capability for a later timer. Scripts that need to act on the created object wait for its created/changed event or returned direct-action result. There is no multistep construction workflow planned by the engine.

## Subscribed runtime illuminance

Lux at a live place is a runtime computed measurement, not a baked shape field or authored state. A validated mechanical role binding names its measured term ID and dimensions; scripts cannot set it or fake it through maker's text. Geometry roles still read the asset manifest. Native code uses the role binding, never a content term spelling.

The thin light adapter owns subscriber positions/sites, their cell index, nearby-light candidate lists and runtime measurement samples. Space returns dirty bounds for geometry/transform changes. Emitter intensity/site/radius and zone ambient changes similarly invalidate only affected subscribers; light addition/removal updates the candidate lists. This follows Technicals §7's cell/neighbour invalidation, not a world-wide scan or per-object polling loop.

```cpp
void invalidate_light_bounds(LightSubscriptions&, const Bounds& dirty);
void invalidate_emitter(LightSubscriptions&, Handle emitter, const Bounds& influence);
std::vector<DerivedFactInput> refresh_light_subscribers(
    LightSubscriptions&, const RuntimeMeasurements&, const EmitterRows&,
    const ZoneAmbientRows&, TransformRead, const OcclusionQuery&, Step now);
void apply_derived_fact(RuntimeMeasurements&, const DerivedFactInput&,
                        DeferredEffects&);
```

`OcclusionQuery` is a narrow opaque adapter operation, not access to space's other tables. These functions belong to the light adapter target, which can link space; the reaction target cannot. `DerivedFactInput` contains subject, measured term, value, provider revision and Origin, never arbitrary state changes. Only a registered provider may submit it. Generic reaction processing compares the old/new answer and wakes subscriptions.

Before a dependent watch or a measured-value binding returns, settle the provider for that place, so a handler reading after a light or wall change sees the current world. An explicit measured read computes through the narrow light adapter using current emitter values even when a same-handler state change has not yet published its invalidation notice. It must not return a stale cached sample merely because the normal refresh phase has not run yet. Refresh produces samples; only `apply_derived_fact` updates the shared measurement table and generates its change notice, preserving the prior answer for comparison. Background geometry work publishes its completed dirty bounds as ordinary inputs. Restore rebuilds provider caches and subscriptions, then compares recomputed samples with saved last-delivered watch answers.

## Verified Luau 0.747 hooks

The dependency lock pins `luau-lang/luau` tag `0.747`. The following are primary-source findings, not assumed APIs:

* [Compiler.h](https://github.com/luau-lang/luau/blob/0.747/Compiler/include/Luau/Compiler.h) exposes `compileOrThrow` with a parsed AST/`BytecodeBuilder`, but no opcode-veto callback. [Ast.h](https://github.com/luau-lang/luau/blob/0.747/Ast/include/Luau/Ast.h) exposes `AstExprLocal::upvalue`, assignment/compound-assignment/function-assignment nodes and `AstVisitor`. Reject assignment to an upvalue through all three forms, unwrapping grouped/type-asserted lvalues. This provides source locations and requires no private VM headers.
* [Compiler.cpp](https://github.com/luau-lang/luau/blob/0.747/Compiler/src/Compiler.cpp) emits `SETUPVAL` for an upvalue store. An exact opcode audit is possible through public [BytecodeBuilder](https://github.com/luau-lang/luau/blob/0.747/Bytecode/include/Luau/BytecodeBuilder.h) function data and [BytecodeUtils](https://github.com/luau-lang/luau/blob/0.747/Common/include/Luau/BytecodeUtils.h) instruction lengths, but requires a small pinned serialized-function reader. Its built-in instruction validator is not a public policy hook. Prefer the public AST rule plus runtime freezing unless an opcode audit proves necessary; do not parse textual disassembly.
* Public [lua.h](https://github.com/luau-lang/luau/blob/0.747/VM/include/lua.h) supplies `lua_getupvalue`, table iteration/metatable access, `lua_setreadonly`, userdata tagging and interrupt callbacks. Recursively freeze captured tables, their keys/values/metatables, and reachable Lua closure upvalues with a visited set. Reject retained buffers/threads/class instances and unapproved userdata; immutable handle userdata is allowed. Native binding closures are trusted adapters whose captured data follows the ownership table above.
* Luau's [sandbox guide](https://luau.org/sandbox/) explains readonly libraries and the hazards of environment replacement. Remove `getfenv`, `setfenv`, dynamic loading, suspension, wall clocks and VM-global random access from the world environment. The default per-script writable sandbox environment is not sufficient for this state contract. [The pinned VM's generic-for implementation](https://github.com/luau-lang/luau/blob/0.747/VM/src/lvmexecute.cpp) explicitly honors userdata `__iter`, so the flat state proxy need not pretend to be a Lua table.

Top-level loading instantiates immutable handlers/declarations once, under a load budget and without world mutation/authority. The bake-produced declaration is authoritative; loading must not discover a different action schema or perform gameplay. Persist state, timers and RNG from their actual tables, never VM closure snapshots. Ordinary per-handler scratch tables and functions are transient and need not be serialized. The blanket no-upvalue-write rule also disallows mutating an enclosing local from a nested scratch helper; document that small authoring restriction and use ordinary local loops/state fields instead.

## Required tests before acceptance

Test new-term thresholds in both directions, stale timer revisions after rate/relation changes, disappearance/withdrawal, strict per-input draining, foreground overflow versus nondroppable watches, and background progress. Run the multi-author chain through compiled Luau. Test partial mutations after error/cutoff and flat proxy key/handle fidelity across snapshots. Reject scalar-upvalue mutation, captured mutable tables, retained buffers and actor-authority reuse. Exercise a novel subscribed light term under emitter, door and wall changes without native content recognizers. Benchmark the actual callback/state/threshold paths and their idle cost; no test may replace them with a synthetic simulator.


## Implementation and checks

`amber_step` links `amber_record`; `amber_react` links `amber_step` and the pinned Luau compiler, AST and VM. Add the modules after store/record. Reaction code has no space, permission, networking, text or ledger dependency.

`Subscriptions.rows` and `next_revision` are authoritative. Its consumer/source indexes are derived, and `rebuild_subscription_index` restores them. Every binding receives a monotonically increasing revision, including after removal and recreation. Relation watch answers contain both outgoing and incoming edges; delivered changes include the other endpoint, direction and added/removed bit. `threshold_timers` produces at most one next-boundary timer per binding. The driver validates its token, samples the live source and checks watches before rearming the next boundary.

`AllocationRows.values` and meters persist; subject/term and incident-object indexes rebuild through `rebuild_allocation_index`. Updating one subject touches only that subject's contributors. Withdrawn live contributors stop accumulating but retain their meter identity; destroying either endpoint removes incident rows after any authored settlement. Demand watches save the aggregate and their own allocation separately. The completion adapter consumes contribution-change drafts, reconciles indexed priority dependencies, then publishes ordinary watch work. See [allocation-api.md](allocation-api.md) for the concrete reconciliation and retirement contract.

The runtime's separately registered read/state/relation/contribution/share bindings point at only their declared tables. `bind_events` supplies the pure event/timer/action helpers. State writes additionally enforce the verified script's declared state terms; contributions enforce its declared contribution terms. Runtime construction commands omit identity/ownership fields, and `queue_structural` accepts creation/rebinding only from the active approved world-rule declaration. Composition supplies ownership, attribution and body/motion flags after authority and geometry checks.

Optimized warning-as-error and Address/UndefinedBehavior Sanitizer checks cover scheduler order, timer-wheel cascades, process thresholds in both directions, conserved transfer, source-specific/parametric reads, inverse contribution cleanup, relation watches, rebind revision invalidation, actual-served allocation meters, immutable Luau captures, typed state keys/handles, prior writes after error/cutoff, combined Lua/state memory bounds and script action identity. `react_bench` executes real Luau state-proxy runs as well as sparse state and threshold paths; it reports checksums and nanoseconds per operation.


Structural outputs are held in `DeferredEffects.structural`, separate from `ordered` work. The driver takes/applies them before final direct-change accounting and Journal records. `publish_effects` returns void and rejects undrained structural outputs; it cannot accidentally enqueue descendants first and return geometry afterward. `Vm` requires explicit `VmSettings` from shared configuration for memory/declaration/handler/payload limits. VM call setup and handler execution share one protected Luau call, so a repeated memory-limit failure remains a handler failure rather than an unprotected native allocation error.
