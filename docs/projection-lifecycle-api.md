# Lifecycle projection adapters

These adapters maintain derived spatial, lighting, mass and replication indexes for spans of already allocated ordinary handles. They contain no action routing, permission decisions, catalog mutation or reaction installation. Physics creation stays in the existing geometry adapter; projection birth never adds a body or sensor a second time.

The fixed scene integration operations call these owner-specific functions after identity/catalog/part changes are committed. Light and zone descriptors resolve the object's actual parameter asset, using the same manifest parsers as presentation. The physics adapter has already installed any real zone/container sensors. The zone projection only installs its authored ambient/priority row. A model or parameter rebind can call birth again to replace or remove its old descriptor.

Each operation has a separate narrow context:

```cpp
struct SpatialRetirement {
    space::Space& space;
    IdentityRead identity;
    ParentWrite parents;
    SpaceFlags flags;
    space::PhysicalRelations& relations;
    vector<space::RelationChange>& changes;
    step::Intents& intents;
    vector<step::IntentEnding>& endings;
};
struct LightProjection {
    observe::Lights& lights;
    observe::LightSubscriptions& subscriptions;
    IdentityRead identity;
    TransformRead transforms;
    const vector<uint32_t>& params;
    const catalog::Models& models;
    const catalog::ParameterBlocks& parameters;
    const catalog::Assets& assets;
    const react::Terms& terms;
    float default_source_radius_m;
    observe::ActiveViews* views = nullptr;
};
struct ZoneProjection {
    observe::ZoneAmbients& zones;
    IdentityRead identity;
    const vector<uint32_t>& params;
    const catalog::Models& models;
    const catalog::ParameterBlocks& parameters;
    const catalog::Assets& assets;
    const react::Terms& terms;
};
struct MassProjection {
    AssemblyMasses& masses;
    react::RuntimeMeasurements& measurements;
    IdentityRead identity;
    const Relation& declared_parts;
};
struct ReplicationProjection {
    ReplicationIndex& index;
    TransformRead transforms;
    const catalog::Models& models;
    const catalog::ParameterBlocks& parameters;
    const catalog::Assets& assets;
    const react::ValueRead& values;
};
void register_light_projection(LightProjection&, span<const Handle>);
void move_light_projection(LightProjection&, span<const Handle>);
void retire_light_projection(LightProjection&, span<const Handle>);
void register_zone_projection(ZoneProjection&, span<const Handle>);
void retire_zone_projection(ZoneProjection&, span<const Handle>);
void invalidate_mass_projection(MassProjection&, span<const Handle>);
void retire_mass_projection(MassProjection&, span<const Handle>);
void register_replication_projection(ReplicationProjection&, span<const Handle>);
void move_replication_projection(ReplicationProjection&, span<const Handle>);
void retire_replication_projection(ReplicationProjection&, span<const Handle>);
```

The fixed [scene integration operations](scene-api.md) compose these direct calls with reaction, custody and permission cleanup. They retain the narrow owner contexts privately; callers supply no registration or cleanup lists. Spatial retirement cancels the current intent and calls actual Space remove while identities and declared edges are valid. It follows the root-first supplied closure, so retiring an entire compound does not rebuild it once per removed child. Removing only a child still uses Space's ordinary remaining-compound rebuild.

Light movement/rebind/removal dirties only subscribed sample points in the old/new actual light ranges. Mass invalidation follows declared ancestors before retirement deletes their edges; retiring rows and runtime answers are erased, while surviving ancestors remain dirty. Composition refreshes mass after the lifecycle operation completes, using the existing refresh_mass function and its actual Origin. No observer, object or process scan runs every step.

Replication registration, movement and removal reuse the existing shared cell/interest implementation. The resulting publication carries ordinary additions/removals for actual interested viewers. Values/fact changes remain the existing term-specific value_changed notification; this adapter adds no second sampling schedule.

Visibility and lifetime are distinct. For custody, composition removes light/zone/replication scene projections for the explicit collected closure after actual Space custody. It retains mass/runtime measurements because stored goods still have physical mass. Inventory replacement invalidates mass but does not register visible projections. Projection birth receives only the handles actually present in the scene.

The projection primitives stay independent so geometry, observation and replication keep their own mechanical implementations. Creation, rebinding, retirement, visibility and startup ordering are owned once by the integration module; none of these indexes adds an inert-world step scan.

Validation uses actual lifecycle allocation/release, Jolt geometry, authored variant light/zone descriptors, a real mass resolver and a subscribed replication stream. Tests cover birth, movement, rebind without double body/sensor allocation, entire assembly removal, surviving ancestor mass, hidden rebind/restore/reveal and slot generation reuse. The coordinator also runs through actual Lua structural and commerce integration tests.
