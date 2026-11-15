# Observation and light API proposal

Status: implemented; focused geometry/value/response checks accompany this contract. This completes Technicals7 and S5.3 through generic geometry, registered values and authored presentation metadata. It adds no body verbs, named zones, material spelling tests or gameplay state.

## Ownership and dependencies

`amber_observe` owns per-session response caches, distance-based hearing queues, a derived light-candidate index and dirty subscribed illumination points. It links store, space and react; catalog presentation data arrives as per-object immutable DTOs. The separate observation adapter resolves catalog records and action declarations with const references. No function receives Objects, authority writers, wallets, VM ownership or a composition root.

A look runs three stages: geometry selects visible handles; the adapter resolves only those handles' descriptions/effective values; formatting compares those prepared rows to the session's previous rows. This DTO approach is preferred to one function with every table reference. A callback capturing a universal context is rejected. Full JSON reconstruction for at most40 nearby rows is acceptable for this prototype and correctly notices a continuously evaluated process changing between looks; an object-revision-only cache would miss that change.

## Authored inputs and tuning

```cpp
struct Observer {
    Handle body;
    Vec3 eye;                // world meters, resolved from authored body site
    Vec3 forward;            // unit XY projection, canonical body +Y
    double dim_lux;          // effective authored observer threshold
};
struct Settings {
    float sight_m, reach_m, far_m;
    float far_angular_degrees, moving_m_s;
    double far_source_lux;
    size_t things_max, far_max, events_per_response;
};
struct LightBinding {
    Handle source;
    Vec3 local_site;
    variant<double,TermRef> intensity; // constant or validated candela quantity
    float radius_m, source_radius_m;
};
struct ZoneAmbient {
    Handle zone;
    variant<double,TermRef> ambient;   // constant or validated lux quantity
    int priority;
};
```

The root reads mechanical settings from shared config.toml; the body site and dim threshold come from content. Proposed salience tuning fills an unspecified S5 choice: far search256m, apparent size at least1degree, speed at least0.1m/s or emitter contribution at the eye at least0.1lux. The default near radius/reach/counts remain S5's30m/1.5m/40/8. These values are tuning, not realism claims or hidden authoring behavior. A finite spatial search avoids scanning the entire region for each observer. Another viable choice is every region-wide large object's visibility plus a shorter moving/light radius, but that needs a second candidate policy with little prototype benefit; use the one bounded query first and measure it.

Size salience is `2*atan(max_dimension/(2*distance))`; motion uses actual supplied velocity, not a model category; brightness uses the registered emitter value with ordinary occlusion. If several reasons apply use large, then moving, then bright, while ordering far results by distance and full handle for ties. Near objects are nearest first. No illumination cutoff removes a geometrically visible object: dim things remain present with lit:false, matching S5.

## Lights, zones and live measurements

```cpp
struct Lights;               // derived handles/bindings and cell -> candidate handles
struct ZoneAmbients;        // object-keyed binding rows
void set_light(Lights&, const LightBinding&, TransformRead);
void move_light(Lights&, Handle, TransformRead);
void remove_light(Lights&, Handle);
void set_zone(ZoneAmbients&, ZoneAmbient);
void remove_zone(ZoneAmbients&, Handle);

void light_samples(const Lights&, IdentityRead,
                   const react::ValueRead&, Step, double hz,
                   Vec3 point, vector<space::LightSample>&);
double ambient_at(const ZoneAmbients&, const react::ValueRead&,
                  span<const Handle> containing_zones,
                  Step, double hz, double authored_world_lux);
space::Illumination illuminate(const space::Space&, IdentityRead,
                              Handle receiver, Vec3 point,
                              double ambient_lux,
                              span<const space::LightSample>, Deadline);
```

The cell index caches source candidates only. A light is added/moved/removed through these operations after binding/transform changes; intensity is read from current effective values at query time, including closed-form processes. Source intensity changes therefore require no geometry-index rebuild. Ambient selects highest priority containing zone, then lowest full handle for stable ties; the fallback is an authored world value. Invalid binding types/units fail catalog validation. An absent scalar quantity contributes zero; an explicit constant zero switches a source off. This is the scalar absence rule, independent of any authored property name. A material's numeric transmission is already supplied to space. Illumination is ambient plus unoccluded intensity/distance²; source_radius_m supplies the finite near-distance denominator already used by space.

For points with a physical object, in_zone supplies containing handles. For a free subscribed point, space needs the narrow additional query:

```cpp
bool volumes_at(const space::Space&, IdentityRead, Vec3,
                space::VolumeKind, Deadline, vector<Handle>&);
```

It performs a Jolt broadphase point query followed by the actual closed mesh containment test. It returns volume handles, never authored names or ambient state. It also permits a geometry-free zone owner. The adapter resolves zone names from maker text separately.

Only subscribed places keep authoritative runtime illuminance facts:

```cpp
struct LightPoint { Handle subject; Vec3 position; TermId measured_term; };
struct LightSubscriptions;
void subscribe(LightSubscriptions&, LightPoint);
void unsubscribe(LightSubscriptions&, Handle, TermId);
void dirty_geometry(LightSubscriptions&, span<const space::Bounds>);
void dirty_light(LightSubscriptions&, Vec3, float radius_m);
vector<LightPoint> take_dirty(LightSubscriptions&);
```

The table indexes subscribed points by cell. Geometry edits dirty the touched cells and neighboring cells, as Technicals7 specifies; a changed light dirties points in its influence radius. The adapter recomputes only these points through the same light query, compares the value and calls react::apply_derived_fact with the mechanical provider's term ID and causal origin. It never names a particular light quantity. Unsubscribed places are measured afresh only when perceived. Source/zone renaming changes descriptive output but never geometry or lux.

## Visibility and prepared observation rows

```cpp
struct MotionSample { Handle object; Vec3 velocity; };
struct Seen {
    Handle object;
    float distance_m, bearing_degrees;
    double lux;
    bool held, lit, reachable;
    vector<Handle> through;
    enum class Salience { Near, Large, Moving, Bright } salience;
};
struct Scan { bool complete; vector<Seen> things, far; vector<Handle> zones; };
Scan scan(const space::Space&, IdentityRead, TransformRead,
          const Relation& declared_parts, const Observer&, Handle held,
          const Settings&, const Lights&, const ZoneAmbients&,
          const react::ValueRead&, span<const MotionSample>,
          Step, double hz, double world_ambient_lux,
          size_t requested_limit, Deadline);
```

Candidates come from Jolt's existing broadphase, not a full object scan or a separately maintained visibility graph. Use bounds distance for proximity/reach and the candidate's center for bearing. Cast to center plus four side samples at quarter and three-quarter height for large/partially occluded geometry; one clear path suffices. Exclude observer and target's own geometry from blocking their visibility path. Intervening glass is reported by handle and attenuates transmission while remaining collision-solid. Physical rays and sight rays stay separate. Sample limits and deadlines prevent a single look monopolizing the writer. `complete:false` is explicit; callers must not cache a partial result as a complete observation.

The explicit declared_parts relation maps child hits to the enclosing assembly root when that root has no geometry, and lets visibility ignore that assembly's own shapes. It does not collapse seated bodies, held items or arbitrary ParentRead relationships. Ordinary part handles remain available to detailed inspection and scripts. Held objects are included with held:true/dist_m:0 independently of a sight ray; custody inventory is not made scene-visible.

```cpp
struct ObjectDescription {
    Handle object;
    string model;
    nlohmann::json measured;  // truth: asset/material/effective registered values
    catalog::Text says;      // model claims with instance text override
    vector<string> actions; // declarations, no permission filtering
};
struct PreparedThing { Seen seen; ObjectDescription description; };
struct SelfDescription {
    Handle body, holding;
    Vec3 position;
    float facing_degrees;
    nlohmann::json intent;
    vector<pair<Handle,string>> zones;
};
struct SessionCache;
nlohmann::json observation(SessionCache&, Step, double hz,
                           const SelfDescription&, span<const PreparedThing> things,
                           span<const PreparedThing> far,
                           span<const Heard>, span<const string> body_actions,
                           span<const string> player_actions);
void forget(SessionCache&);
```

The adapter's one-object description helper receives only IdentityRead, const model/parameter/asset/text metadata, const effective value read projection and declaration summaries. Material/size/mass/capacity are measured truth; material names resolve registered IDs. It evaluates the explicitly supplied inspection-term list, rather than iterating the whole registry for every thing. Maker name/description/writing stay under says and never alter measured data. Actions are model/body declarations plus registered administrative discovery supplied by the dispatcher, shown within reach regardless of current rights. Detailed inspect uses the same description helpers with extra identity/rights DTOs prepared by the permission adapter; observe performs no permission checks.

Cache comparison excludes distance/bearing, which always update. An unchanged descriptive/measured row becomes `{h,dist_m,bearing}`. A change in live state, material, maker text, lighting, held status or declaration list sends the full row. Body/player action lists appear on the first response and on subsequent changes. Reconnect calls forget. The cache is per session, never shared between distinct viewers/agents.

## Hearing and invalidation

```cpp
struct Sound { Handle source; Vec3 at; float radius_m; EventPayload payload;
               PlayerId speaker; record::Origin origin; };
struct Listener { uint64_t session; Handle body; Vec3 ear; };
struct Heard { Sound sound; float distance_m; };
struct Hearing;
void emit_sound(Hearing&, const Sound&, span<const Listener>);
vector<Heard> take_heard(Hearing&, uint64_t session, size_t limit, size_t& omitted);
void remove_listener(Hearing&, uint64_t session);
```

Sound uses authored radius and distance only; wall acoustics remain deferred. It carries typed content rather than converting every sound into speech or recognizing a verb. Queues preserve arrival order. Responses use the newest EVENTS_PER_RESPONSE entries by default and report older omitted count; an explicit larger request can retrieve more. The adapter also attaches its ordinary event queue to each response as S5 requires.

ActiveViews stores only active session eye/radius DTOs in a cell index. track_view/remove_view update its lifetime; dirty_views marks only sessions whose radius intersects changed bounds and take_dirty_views drains those IDs. World changes mark affected active observers dirty using spatial bounds and their sight range; an explicit look always scans current geometry and values. No inert object schedules observation work. Renamed or moved zones, changed intensities and new occluders invalidate only relevant observers/subscribed points.

## Verification

Use real baked Jolt shapes: opaque wall vs transmissive pane, partial occlusion, nearest/reach ordering, a distant large shell, moving/bright salience and ignored out-of-range objects. Build a new named zone/emitter away from the seed, translate/rename/delete it and verify membership/name/lux follows the data. Compare light-off and occluder-close on both an explicit look and a subscribed runtime fact. Test geometry-free root/ordinary part visibility, held item inclusion, separate measured/says data, full-to-short cache transitions, process values changing without writes, action-list refresh and distance hearing queues. Benchmark look and subscribed-light invalidation against inert-heavy scenes; the candidate scan and source index must scale with local density/actual changes.
