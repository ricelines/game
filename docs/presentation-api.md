# Presentation, replication and runtime measurements

Status: approved and implemented, with focused validation in `tests/presentation_test.cpp` and `tests/measurements_test.cpp`. These adapters are independent functions under `server/bindings/`; they prepare existing observe/net DTOs from read-only catalog/value projections. They do not route actions, invoke Luau, mutate ownership, load geometry files or own a composition root. Runtime aggregate mass is a separate mechanical provider, with its own inputs and output table.

## Object descriptions and metadata

A geometry look already returns a small visible handle set. Resolve effective assets through existing catalog::resolve_asset and construct only those descriptions. The single-object helper receives exactly:

```cpp
observe::ObjectDescription describe_object(
    Handle, IdentityRead, const catalog::Models&, const catalog::ParameterBlocks&,
    const catalog::Assets&, const catalog::ModelTexts&, const catalog::InstanceTexts&,
    const react::ValueRead&, span<const TermId> inspection_terms,
    span<const string> declared_actions, Step, double hz);
```

This is one presentation operation, not a retained host context. It has no authority writer, VM, network socket, physical owner or action resolver. Model reference is `m:<id>`. `measured` contains effective asset hash, canonical resolved parameters, actual size_m/volume_m3/capacity_m3, material ID/name and the explicitly supplied inspected values keyed by registered term name. Registered own mass and assembly mass remain different inspected quantities. Object-handle values serialize as handles; absent answers are omitted. Claims remain only in says. The full instance Text row, when present, replaces model text; an editor wanting a one-field change must merge the current text before writing that row. Empty strings can therefore intentionally clear claims.

Object metadata is a separate helper accepting only IdentityRead, AuthorityRead, ParentRead, text tables and the requested live handles. It produces the existing `{h,model,material,owner,parent,text}` rows. Transform-only motion uses binary replication and does not rebuild this metadata. Agent name/purpose/status rows are supplied independently by their owner and are not part of this context.

Catalog DTO helpers accept explicit model/term/material ID lists and their const catalog/value tables. Material appearance uses explicit role IDs `{red,green,blue,alpha,metallic,roughness}`, bound by MechanicalBindings; no material spelling selects a color. Missing appearance roles use renderer-neutral defaults, not game behavior. The role resolver validates dimensionless numeric quantities and clamps display ranges. IDs/names/kinds/units and actual default asset/material/text references are copied. No default-asset bindings are duplicated into model.bindings: Catalog::Model currently has no separate binding overrides, and copying defaults there would incorrectly override a parameter variant's own asset manifest. The immutable effective asset manifest remains the binding source.

An alternative is a single description callback capturing all region tables. It is concise but conceals access and is rejected by the architecture. Passing complete worlds to a generic JSON serializer is also rejected. The small prepared DTO pipeline is recommended; at most40 normal rows make JSON comparison inexpensive and it observes changing processes correctly.

## Authored physical presentation roles

Parsing is separate from registration. Functions take only `(const catalog::Asset&, const react::Terms&)` and return optional plain descriptors:

* light: existing `{intensity: constant_or_term_name, site:[x,y,z], range:m, color:[r,g,b]}` plus optional positive `source_radius_m`. An omitted source radius uses a named mechanical near-distance clamp from shared config. Return `observe::LightBinding` after the caller supplies the actual source handle. No raw manifest term spelling becomes an engine semantic branch.
* zone: existing `{ambient:constant_or_term_name, priority:int, shape:{kind:'box',size:[x,y,z],offset:[x,y,z]}}`. Return the scalar ambient binding and the closed sensor geometry descriptor. A generic space box-volume constructor makes a real Jolt closed box; it creates no rendered asset or collision-solid object. Additional sensor asset geometry can use an explicitly resolved GeometryId rather than inventing a second manifest.
* observer: `{eye:[x,y,z], forward:[x,y,z], dim_lux:constant_or_term_name}`. Return a local eye/forward and scalar threshold. The caller transforms it with the actual body pose and evaluates the threshold through ValueRead. Local forward has a nonzero XY projection. Bearing uses the transformed XY direction without dividing by its length, so a vertical world direction stays finite. Lux/candela dimensions are catalog-validated. DIM_LUX is authored body data, not a global engine behavior.

Registration/removal calls the existing separate Lights/ZoneAmbients functions; movement updates the light site and actual space volume. Observer and light indexes are derived and rebuilt from live metadata after restore. Changed sources/geometry dirty only indexed subscribed light points and active views. Source processes must join the source's actual derived measurement subscriptions; there is no full-world light scan every step. Static/constant lights cause no recurring work.

## Replication publication

`amber_net` already quantizes cells once and shares encoded bytes across recipients. This adapter owns only derived writer-side indexes used to decide which current values need sampling. No network thread reads world tables.

```cpp
struct ReplicationIndex; // handle->asset/cell registration; cell->handles;
                         // cell viewer counts; dirty handles; active process handles
void register_replication(ReplicationIndex&, Handle, IdentityRead, TransformRead,
    const catalog::Models&, const catalog::ParameterBlocks&, const catalog::Assets&,
    const react::ValueRead&);
void moved(ReplicationIndex&, Handle, TransformRead);
void value_changed(ReplicationIndex&, Handle, TermId, const react::Processes&);
void changed(ReplicationIndex&, Handle); // flags changed; re-register model/parameter changes
void retire_replication(ReplicationIndex&, Handle);
void view_interest(ReplicationIndex&, SessionId, Vec3);
void remove_view_interest(ReplicationIndex&, SessionId);
net::Publication publication(ReplicationIndex&, IdentityRead, TransformRead,
    const react::ValueRead&, Step, double hz);
```

The public header lists the actual plain fields rather than retaining callbacks. Immutable per-asset resolved bindings/replicated term lists are interned by hash; there is no per-instance measurement copy. Parameter identity is a stable uint64 digest of canonical parameter bytes, independently of asset/material/model identity. S6 asset bytes use the actual SHA256, positions are meters, footprint is the actual maximum XY extent, and faces_motion comes only from the manifest. Replication flags are supplied mechanical bit values, never recycled to encode a gameplay category.

Only the selected effective manifest's `replicated` term IDs are read. Missing answers are omitted and mask8 remains full replacement. On registration/value/model changes, relevant process rows are added to a derived sampling subscription; unrelated processes and inert objects never join it. At each replication tick, iterate dirty handles plus currently interested process subscriptions, deduplicate and sample actual current closed-form values. Processes clamped at their terminal bound leave recurring sampling until a rate/value change re-arms them. Shared/contribution/settings changes enter through their existing changed fact notifications. Rendering dependencies not declared replicated are not silently added: authoring must list them.

The writer-side cell index is necessary to subscribe process samples before a newly entering viewer gets its keyframe; it has no local stream IDs or byte baselines. It reuses net::replication_cell and net::interest_cells rather than duplicating their formulas. For a view input the root calls view_interest, queues Server::view, then queues Server::publish with current newly interested rows. Server::view only changes the desired view; the subsequent publication performs replication and builds the keyframe from those current values. On ordinary replication ticks it publishes once for all clients. Network baselines contain the union of current viewer interest; the last viewer leaving a cell removes its published rows. Global rows are interested whenever any viewer exists. Cell membership changes update both process subscriptions and publication dirtiness without scanning all rows. Empty cell/view entries are removed.

The simpler alternative is to sample every registered process whenever any viewer exists. It avoids an interest index but scales with activity elsewhere in the world and sends stale first frames after a dormant period unless explicitly warmed. Use the explicit interested process set; benchmark it at500 viewers and100k inert rows before accepting the added maintenance cost.

## Assembly-mass provider

Root chose a separately registered measured runtime term, preserving disjoint existing Mass for construction accounting. MechanicalBindings maps provider `assembly_mass` to that authored kg term. The provider has no knowledge of body verbs, material names or contents.

```cpp
struct MassRoles { TermId own_mass, assembly_mass; };
struct AssemblyMasses {
    MassRoles roles;
    struct Row { double own_kg, total_kg; uint64_t revision; };
    unordered_map<uint64_t,Row> rows; // derived, keyed by full handle
    unordered_set<uint64_t> dirty;
};
void validate_mass_roles(const react::Terms&, MassRoles);
void invalidate_mass(AssemblyMasses&, IdentityRead, const Relation& declared_parts,
                     span<const Handle> changed);
void refresh_mass(AssemblyMasses&, const react::ValueRead&, const Relation& declared_parts,
                  react::RuntimeMeasurements&, Step, double hz,
                  const record::Origin&, react::DeferredEffects&);
void retire_mass(AssemblyMasses&, react::RuntimeMeasurements&, Handle);
```

Validate own_mass as the existing measured Mass role in kg and assembly_mass as a distinct measured/runtime kg quantity with no combiner. Each total is its own disjoint actual volume*density plus each declared child's total. Cache only derived totals; dirtying a changed object also dirties its explicit ancestor closure. Refresh dirty trees bottom-up and reuse clean child totals, increment revision on an actual answer change and publish DerivedFactInput through react::apply_derived_fact. Material, parameter, birth/removal and part topology mutations call invalidation; no step scans masses. For topology removal, mark old ancestors before cutting edges and new ancestors after adding edges. Full restore rebuilds this cache from the recreated declared graph. It is not snapshot authority.

A root with zero own volume and two10kg children reports assembly mass20kg while ordinary construction sum is0+10+10=20kg. Never sum each row's assembly total for construction, which would count children twice. Material/parameter changes update only the affected node/ancestors, and independent part materials remain distinct. Body content reads the explicitly bound runtime term for carried work; physics receives the same aggregate actual mass for a promoted rigid compound.

## Runtime projection reconciliation

The illuminance adapter samples actual watched points. Dirty geometry first
invalidates points inside the edit, then finds intersecting light spheres
through the light index and dirties each full influence range. An occluder can
shadow receivers many cells away. Supplying ActiveViews to `dirty_geometry`
also invalidates influenced observer caches. LightProjection dirties old/new
binding ranges, including views when its view endpoint is connected.

RunningLights and RunningZones track nonzero intensity/ambient processes.
`register_illumination_processes` indexes explicit initial/born/rebound sources
after their light/zone bindings are installed, so restoring a running process
does not wait for a new state mutation before sampling resumes.
Changes dirty the actual light range or real zone sensor region. A terminal
clamped value is sampled before periodic work stops; changed rates rearm it.
Sensor add/remove/move emits old/new bounds. Incomplete geometry queries retain
the prior answer and schedule retry. Runtime lux uses ordinary fact effects.

MassReconciliation receives mass cache, ValueRead, declared-part edges and
runtime output only. Its callback consumes own Mass/density/measured Volume
FactKeys, invalidates subjects and explicit ancestors, and refreshes dirty
totals into the same deferred effects before descendant dispatch. Normal
publication stamps the completed native/action/handler origin. The assembly
answer creates no feedback work. Lifecycle operations mark old/new topology
ancestors; an empty scalar-change list still refreshes those dirty rows. No
world table is scanned per tick.

## Verification and scope

Add invented term/material/model/observer/zone/emitter fixtures; verify aliases/renames and parameter variants follow data, measured claims stay separate, replication omits undeclared state, process subscriptions activate on interest and stop at bounds, cell migration/stale generations/re-entry produce current keyframes, and no viewer causes no process sampling. Compare500 viewers with100k inert rows to sparse active sampling. Mass tests cover zero-volume root20kg, child material/topology updates, runtime resolver/watch effects and20kg construction accounting. Modules own focused release and sanitizer tests. Root composition remains responsible for feeding lifecycle/value notifications and preserving the stated publication/restore order; a standalone adapter test is not a full-play integration claim.
