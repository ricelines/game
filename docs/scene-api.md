# Object lifecycle and scene integration

`server/bindings/scene.hpp` coordinates the fixed order in which existing owners
maintain a created, rebound, retired, hidden, revealed or moved object. It belongs
to `amber_bindings`; space, reactions, permissions, lifecycle and network remain
separate owners. Engine APIs and Lua handlers never receive this coordinator.

`SceneProjection` privately retains the light, zone, mass and replication
projections, running illumination processes, watched illuminance points, the
readonly value resolver and `MetadataChanges`. That last owner is the single
metadata invalidation/publication backlog shared with authored edits. Moving
objects invalidate observation caches without publishing unchanged metadata on
every pose update.

`ObjectLifecycle` privately retains `SceneProjection`, the object-allocation
owner, reaction installation/retirement and allocation-priority adapters,
commerce retirement, navigation diagnostics, space retirement/intents,
permissions/grants and inventories/limits. Its authority projection is readonly.
Its public operations expose handles, binding changes, visibility and the current
invocation/effects; they do not expose any retained table to callers.

```cpp
void objects_created(ObjectLifecycle&, span<const Handle>, bool in_world,
                     const InvocationInput&, react::DeferredEffects&);
void objects_rebound(ObjectLifecycle&, span<const lifecycle::Rebinding>, bool in_world,
                     const InvocationInput&, react::DeferredEffects&);
void initialize_objects(ObjectLifecycle&, span<const Handle> all,
                        span<const Handle> visible, bool restored,
                        const InvocationInput&, react::DeferredEffects&);
bool can_retire_objects(const ObjectLifecycle&, span<const Handle> exact_closure,
                        string& error);
void retire_objects(ObjectLifecycle&, span<const Handle> exact_closure,
                    const InvocationInput&, react::DeferredEffects&);
void scene_visible(SceneProjection&, span<const Handle>, bool in_world);
void scene_moved(SceneProjection&, span<const Handle>);
void scene_values_changed(SceneProjection&, const space::Space&, observe::ActiveViews&,
                          span<const FactKey>);
void advance_scene(SceneProjection&, IlluminanceSampling&, observe::ActiveViews&,
                   span<const space::Bounds>, Step, double hz, react::DeferredEffects&);
```

The writer calls these directly. There is no observer registration, caller-built
cleanup list, callback ordering parameter or shared pending-effects buffer.
Construction and model-definition replacement retain only `ObjectLifecycle&`
for this responsibility; they continue receiving their existing independent
geometry, resource and authority projections.

## Operation order

Creation follows successful asset/native capacity and resource preflights.
The construction adapter installs actual geometry and declared attachments,
then calls `objects_created` with the returned ordinary handles. The coordinator
queues author navigation diagnostics, installs initial watches, synchronizes
illuminance subscriptions, registers visible light/zone/process bindings,
refreshes disjoint assembly mass, registers replication and invalidates metadata.
Only then does it append the authored start event for each genuinely new object.
All drafts stay in the invoking operation's effects and receive its final
published causal origin.

Rebinding follows the physical assembly rebuild. It queues navigation diagnostics,
reconciles allocation priorities, synchronizes changed script/watch bindings
without resetting saved state, then updates the same derived projections. The
explicit `in_world` value comes from the prepared assembly's actual visibility.
An inventory edit therefore updates mass and watches but creates no light,
zone, running illumination or replication row. A logical root need not have a
collider, so native shape presence cannot stand in for visibility.

Fresh startup and snapshot restore call `initialize_objects` after physical
attachments, compounds and motion are reconstructed. Both initialize measured
answers for all objects and register only the explicitly visible subset.
Restore preserves saved watch answers; fresh startup initializes them quietly.
Neither sends start events: the authored initial plan owns those bootstrap events.

Removal first collects the full declared-part closure through
`lifecycle::Instances::closure`, then calls `can_retire_objects` before resources
or world state change. Commit cancels affected commerce and restores surviving
custody, withdraws reaction contributions/relations and reconciles surviving
consumers, removes state/watches/settings, revokes grants and inventory/limit
rows, cancels movement, and removes real bodies/sensors/attachments. It then
removes illuminance, lights, zones, running processes, replication and derived
mass rows. Only after all owners finish does `Instances::release_rows` remove
declared edges/names and free identities. Surviving ancestor mass is refreshed
against the remaining part graph. External occupants are not declared parts;
space detaches them while retaining their valid ordinary identities.

Commerce and goods custody first perform their existing physical preflight and
commit, then call `scene_visible` on the exact closure. Hiding removes all spatial
projections immediately. Revealing reinstalls them from the current effective
asset. Neither changes script state, authority or measured inventory mass.
`scene_moved` updates replica cells, transformed lights and illuminance points,
then invalidates affected observation caches. `scene_values_changed` projects changed
facts into replication and indexed light/zone processes, synchronizes illuminance
subscriptions and invalidates viewed answers using one deduplicated handle list.
`advance_scene` dirties actual geometry and influenced view/light ranges, advances
running source processes, then samples illuminance into ordinary derived effects.
The runner retains the S2 phase order: movement, changed values, scene sampling,
effects reconciliation, newly derived value changes, then publication. No scene
or lifecycle operation adds an inert-world scan to the simulation step.

## Choice and validation

Keeping independent per-caller callback lists would preserve their flexibility
but require every construction, commerce and startup caller to reproduce the
same order and visibility rules. A generic observer bus would hide that order and
add registration/lifetime machinery. The fixed integration module makes each
supported operation explicit while leaving each existing owner's data and
mechanical implementation separate.

`projection_lifecycle_test` exercises actual Jolt bodies/sensors, indexed lights,
zones, mass, replication, startup/restore visibility, slot generation reuse,
hidden rebinding and reveal. `structural_test` exercises real Lua construction,
retained state, new-part start events, measured and foreign-followed watches,
resource settlement and topology edits. `commerce_test` and `goods_test` exercise
actual visible/hidden custody through the same scene owner. The coherent [post-refactor sanitizer checkpoint](../reports/native-sanitizer-refactor-2026-11-15.json)
builds all 52 test/probe/integration targets and passes all 47 registered tests
plus both actual Bake2 integrations. The native module consumers are instrumented;
third-party archives and the standalone server process are outside that claim.
The separate [500-viewer probe](../reports/net-prototype-refactor-2026-11-15.json)
uses the actual `1900a7c8…` server in a quiet test window.
