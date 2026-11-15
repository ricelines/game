# Space API

The implemented public interface is `server/space/space.hpp`. It follows
`architecture.md`, S0/S1/S2/S5/S8/S10, and the immutable formats in `bake-api.md`.
The public dependency is `store.hpp` plus the C++ standard library. The native
library links only `store`, Jolt, Recast, Detour and DetourCrowd. It has no
dependency on react, perm, transport, authored content or the catalog.

Space owns collision mirrors, character controllers, geometry cache entries,
crowd agents, tile jobs and derived paths. Its authoritative writes are object
transforms, parents, masked `SpaceFlags`, and the four physical relations. It
never receives ownership, account, grant, author, text, registry or property
tables. A material's numeric transmission and a model's geometric descriptors
arrive as values. No function looks up an authored term name.

## Coordinate and geometry contract

Public vectors use meters in the server's right-handed Z-up frame. A pose is the
baked model origin: centered in XY, with its base at local Z=0, as S8 specifies.
It is never silently interpreted as a center-of-mass pose. Native Jolt conversion
uses each shape's center-of-mass offset. Character origins are at their feet.

Recast/Detour use Y-up internally. Convert `(x,y,z)` to `(x,z,-y)` on entry and
invert it on exit; this is a proper rotation and preserves triangle winding.
Godot conversion belongs to the replication/client adapter, not this API.

The asset adapter resolves one immutable baked asset and calls `import_geometry`.
It does not lend space a model catalog. `collision.bin` and `convex.bin` are
complete Jolt 5.6.0 `SaveWithChildren` streams; space restores them with
`sRestoreWithChildren`. `mesh.bin` is `AMBMESH1`, little-endian counts and Z-up
vertices/indices, as defined in `bake-api.md`. It supplies the actual LOD1 triangles
used to rasterize navigation. Static bodies use the mesh; simulated and grasped
objects use the baked hull of at most 64 points. Parameter changes import a new
asset and replace the geometry; there is no arbitrary runtime size override.

```cpp
namespace amber::space {

using GeometryId = std::uint32_t; // zero is absent; local to one Space instance

struct Pose { Vec3 position; Quat rotation; };
struct Bounds { Vec3 min; Vec3 max; };
struct GeometrySource {
    std::span<const std::byte> static_jolt;
    std::span<const std::byte> convex_jolt;
    std::span<const std::byte> navigation_mesh;
    Bounds local_bounds;
};
struct ImportResult { GeometryId geometry; std::string error; };

enum class Motion { Static, Dynamic, Kinematic, Character };
struct Surface {
    float friction;
    float restitution;
    float transmission; // 0 opaque, 1 transparent; supplied by material adapter
};
struct CharacterSpec {
    float radius_m, height_m, mass_kg, max_push_force_n;
    float max_speed_m_s, acceleration_m_s2;
    float max_step_m, max_slope_radians, floor_snap_m;
};
struct BodySpec {
    Handle object;
    GeometryId geometry;
    Motion motion;
    Surface surface;
    float mass_kg;
    bool navigation_surface; // geometry participates in tile generation
    bool navigation_obstacle;
    std::optional<CharacterSpec> character;
    bool collision_solid = true;
};
```

`import_geometry` consumes its spans during the call and retains decoded immutable
shapes and triangles, never borrowed asset buffers. `GeometryId` is derived state
and is not persisted. The loader caches it by asset hash outside this library.
Body descriptors contain neither strings nor model/author records. `BodySpec`
chooses motion directly. Body identity controls the native character profile;
permission flags stay with their owning adapter. Geometry never guesses behavior
from a model or material ID.

## State and explicit table access

`motion(space,identity,handle)` returns an optional numeric MotionRow for any
present native mode, including Kinematic. A promoted child's point velocity
comes from its actual aggregate linear/angular motion. Direct scripted pose
teleports can have zero native kinematic velocity; callers may use their
moved-only pose deltas for observed motion. `capture_motion(space,identity,
selected_handles,output)` uses O(selected) hash lookups and emits the same
Dynamic/Character MOVE rows as full capture. Nearby gap capture never needs a
full native-body scan. Full capture remains appropriate for a complete S10
checkpoint.

The public `Space` value contains only an opaque native implementation pointer,
has an out-of-line constructor/destructor, and is noncopyable. It owns no store
references. All operations below are free functions. A long-lived span into
Objects is never retained: object creation may reallocate its vectors.
Every public operation runs on the single writer. Native worker callbacks copy
values into space-owned buffers; background tile builders receive immutable
geometry copies and never call these live-state entry points.

```cpp
struct NavSettings {
    float cell_m, cell_height_m;
    std::uint32_t tile_cells;
    float radius_m, height_m, climb_m, slope_degrees;
    float max_edge_m, contour_error_m, detail_sample_m, detail_error_m;
    std::uint32_t min_region_cells, merge_region_cells;
    std::uint32_t nodes_per_query, polygons_per_path, workers;
    std::uint32_t max_tiles, polygons_per_tile;
};
struct Settings {
    Vec3 gravity_m_s2;
    NavSettings navigation;
    std::uint32_t max_bodies, max_body_pairs, max_contacts;
    std::uint32_t physics_temp_bytes;
    float contact_slop_m, sweep_skin_m, crowd_correction_m;
    float blocked_after_s;
    std::uint32_t tile_install_budget_us;
    std::uint32_t max_characters;
    float crowd_query_radius_multiplier, crowd_optimize_radius_multiplier;
    float crowd_separation_weight;
};
struct Space {
    struct Native;
    std::unique_ptr<Native> native;
    explicit Space(const Settings&);
    ~Space();
    Space(const Space&) = delete;
    Space& operator=(const Space&) = delete;
};
```

The composition root constructs Settings from the single `config.toml`. Body
dimensions and movement characteristics come from its manifest/installed body
configuration; settings are not a second set of hidden humanoid defaults. Only
one navigation size class is needed by this prototype. CharacterSpec must fit
that class; different classes are the explicitly deferred extension in Technicals.

The four physical relation tables are separate from authored relation tables.
The following minimal edge schema is shared with relation readers; moving it to
store as a plain type is reasonable, but passing a registry-wide relation map is
not. Named relation IDs are supplied only by the script-binding adapter.

```cpp
using Edges = amber::Relation;
struct PhysicalRelations {
    Edges contains, supports, attached_to, in_zone;
};
enum class RelationKind { Contains, Supports, AttachedTo, InZone };
struct RelationChange {
    RelationKind kind;
    Handle from, to;
    bool added;
};
```

Directions are container→member, support→supported, subject→parent, and
member→zone respectively. Both indexes change together. `ParentWrite` holds the
immediate physical container/attachment; zone and support edges do not assign a
parent. Nested containers choose the smallest containing sensor volume as the
immediate parent. Explicit attachment wins over geometric containment.

## Object lifecycle and edits

```cpp
enum class Status {
    Ok, StaleHandle, MissingGeometry, MissingBody, UnsupportedMotion,
    Obstructed, AlreadyParented, AttachmentCycle, NotAttached, Capacity
};
struct OperationResult {
    Status status;
    Handle obstacle; // none unless blocked
};

ImportResult import_geometry(Space&, const GeometrySource&);
OperationResult add(Space&, IdentityRead, TransformRead, const BodySpec&);
void remove(Space&, IdentityRead, Handle, ParentWrite, SpaceFlags, PhysicalRelations&,
            std::vector<RelationChange>&);
OperationResult set_surface(Space&, IdentityRead, Handle, Surface);
OperationResult set_pose(Space&, IdentityRead, TransformWrite,
                         Handle, Pose);
```

`add` validates a full handle and inserts the supplied actual baked geometry and
pose without allocating an object row. Viewer loading boxes are display-only.
`remove` runs before object destruction, removes native
mirrors and all physical edges, detaches children in place, and dirties affected
tiles. It does not destroy object rows or alter economic/authoritative state.

`set_pose` is the instantaneous edit/restore primitive, including rotation. It
updates the native mirror and the transform columns in the same call. Collision
checking a proposed placement is a separate query the placement binding uses
before committing. Continuous movement uses the sweep primitive below.
Geometry edits use preflighted `extract_assembly` and `rebuild_assembly` over the explicit complete mechanical closure, preserving retained attachment and velocity truth. Material edits use `set_properties`; optical transmission changes apply immediately. The scene owner supplies explicit body and sensor descriptors; Space does not mutate economic or catalog rows.

Structural mutation preflight uses `capacity`, `can_add` and `can_rebuild_assembly`. Future BodySpec handles may be null for initial allocation checks, so geometry/mode/profile/mass validation and native demand are known before allocating authoritative identities or spending resources. Sensors and CharacterVirtual inner bodies each consume one Jolt body; registered empty logical roots consume none. `can_add` receives an explicit sensor count and unique retiring handles; it credits their actual native bodies, owned sensor bodies and character slots. Assembly rebuild preflight accounts for root query bodies and aggregate promotion, and extraction retires the credited native slots before reconstruction. Failure distinguishes Capacity, UnsupportedMotion and MissingGeometry without mutation. Ordinary heap exhaustion is not a game transaction guarantee.

## Zones and containment

```cpp
enum class VolumeKind { Zone, Container };
struct VolumeSpec {
    Handle owner;
    GeometryId geometry;
    Pose local_pose;
    VolumeKind kind;
};
OperationResult add_volume(Space&, IdentityRead, TransformRead,
                           const VolumeSpec&);
```

An authored zone is an ordinary handle with a supplied closed sensor volume.
Container interiors are supplied by the asset/content adapter as numeric volume
geometry. Neither is inferred from a named material, bounding rectangle or a
capacity value. One sensor of each kind per owner is sufficient for the prototype;
an owner with several disjoint volumes uses a compound shape. Names and ambient
illumination remain properties elsewhere.

Membership means the object's center-of-mass sample point lies in the volume;
the character sample is its capsule center. Sensors provide candidate overlaps;
exact point tests settle boundary membership. Static placement, volume edits and
restore explicitly query overlapping candidates. Stationary objects are not
rescanned every step. For an agent following a known corridor, space predicts
the next segment/volume boundary crossing, then validates membership at the
actual collision-corrected position. A collision or path revision invalidates
that prediction. There is no maintained who-can-see-whom relation.

Jolt's contact-removal callback also fires when a body falls asleep. Therefore it
cannot directly delete containment, zone or support facts: cached state and exact
dirty-object tests determine real exits. This matters for stationary objects in a
zone and for a mug resting inside another object.
Removing or moving supporting geometry wakes affected sleeping rigid bodies and
character controllers, so removing a floor still causes its occupants to fall.

## Queries

```cpp
using Deadline = std::chrono::steady_clock::time_point;
struct QueryFilter {
    Handle ignore_a, ignore_b;
    bool include_sensors;
};
struct Hit {
    Handle object;
    float fraction;
    Vec3 position, normal;
};
struct RayResult { bool complete; std::optional<Hit> hit; };
struct Visibility {
    bool complete, clear;
    float transmission;
    std::vector<Handle> through;
};
struct Nearby { Handle object; float distance_m; Bounds world_bounds; };

RayResult raycast(const Space&, IdentityRead, Vec3 from, Vec3 to,
                  QueryFilter, Deadline);
Visibility visibility(const Space&, IdentityRead, Vec3 from, Vec3 to,
                      QueryFilter, float minimum_transmission, Deadline);
bool near(const Space&, IdentityRead, Vec3 center, float radius_m,
          QueryFilter, Deadline, std::vector<Nearby>& out);
std::optional<Bounds> bounds(const Space&, IdentityRead, Handle);

struct LightSample {
    Handle source;
    Vec3 position;
    double intensity_cd;
    float range_m, source_radius_m;
};
struct Illumination { bool complete; double lux; };
Illumination illuminance(const Space&, IdentityRead, Handle receiver, Vec3 point,
                        double ambient_lux, std::span<const LightSample>, Deadline);
```

`raycast` returns the closest physical surface, including transparent surfaces.
`visibility` collects hits in fraction order, ignores the observer and intended
target supplied in the filter, and multiplies numeric transmission once per
intervening object. It reports the traversed handles so observation code can
describe the material using its own tables. An opaque wall blocks while a
transmitting pane passes. No string such as glass appears in space.

`near` reuses Jolt's broadphase spatial index for candidates and current transformed bounds for
distance, returning handles in distance order. It returns false if the deadline
expires; partial results must be marked incomplete by the caller. All collecting
queries check the supplied handler deadline between candidates/hits. A no-hit
result after timeout is never reported as clear sight. This deadline limits native
query cost and is not a source of world time.

Illuminance is ambient plus `intensity_cd / max(distance², source_radius²)` for
in-range lights with a clear occlusion ray, attenuated by transmission. The
caller supplies current enabled light samples and ambient values. The occlusion
ray ignores both receiver and source, so a point inside the receiver's shape
does not occlude itself. A spatial
light-list cache may retain candidate handles, never cached visibility. Door and
wall changes therefore affect the next query without rebuilding a room graph.

## Mechanical primitives

```cpp
struct Attachment {
    Handle subject, parent;
    Pose local_pose;
    bool grasp;
};
struct Release {
    Handle subject;
    Motion motion;
    Vec3 linear_velocity_m_s, angular_velocity_rad_s;
};
OperationResult attach(Space&, IdentityRead, TransformWrite, ParentWrite,
                        SpaceFlags, Edges& contains, Edges& supports,
                        Edges& attached_to, const Attachment&,
                        std::vector<RelationChange>&);
OperationResult detach(Space&, IdentityRead, TransformWrite, ParentWrite,
                        SpaceFlags, Edges& attached_to, const Release&,
                        std::vector<RelationChange>&);
OperationResult impulse(Space&, IdentityRead, Handle,
                        Vec3 newton_seconds, Vec3 world_point);
RayResult sweep(const Space&, IdentityRead, Handle moving,
                Vec3 translation_m, QueryFilter, Deadline);
struct SweepMove { Status status; Pose actual; std::optional<Hit> contact; };
SweepMove translate(Space&, IdentityRead, TransformWrite, Handle moving,
                    Vec3 translation_m, float seconds, Deadline);
```

These functions implement geometry, not declared body verbs. The body script's
grasp handler selects the target and reach, then calls `attach` with `grasp=true`.
A sitting handler selects an authored site's local pose and calls `attach` with
`grasp=false`. Throwing calls `detach` with a chosen velocity. The same primitives
work for non-humanoid models. Permission and declared-reach validation belong to
the input/host binding before these calls; space neither checks a player's rights
again nor inherits causal authority.

Attachment rejects cycles, suspends independent simulation/navigation, writes
parent and the attachment edge, and sets Held only for a grasp. It removes prior
geometric support/containment edges and follows the parent's transform. Held
objects are query-visible but do not collide with their holder or block its
navigation. Release checks the proposed current pose with the convex shape; an
obstructed release keeps the object attached. A successful release selects the
supplied movable mode and velocities; a body script normally chooses Dynamic so
a released prop falls. Child-relative poses are derived
from saved child/parent transforms on restore; attachment edges and Held identify
the mode, so a hidden attachment snapshot section is unnecessary.

`impulse` requires a dynamic hull and applies Jolt AddImpulse at the supplied
point. It does not compute strike energy, transfer liquid or decide damage.

`sweep` is a translational shape cast at the object's current orientation.
`translate` uses that cast, advances only to the first contact minus the configured
skin, and returns the actual pose/contact. This is the primitive a sliding door
script uses. CastShape does not provide a continuous rotational sweep; the API
does not pretend it does. Rotation is an instantaneous edit in this prototype.

## Intent stepping and navigation jobs

```cpp
struct PointGoal { Vec3 point; float tolerance_m; };
struct ObjectGoal { Handle target; float reach_m; };
using Goal = std::variant<PointGoal, ObjectGoal>;
struct TravelBoundary {
    std::vector<Vec3> polygon; // convex XY footprint, Z ignored
    float min_z, max_z;
};
struct MoveRequest {
    Handle body;
    std::uint64_t intent;
    Goal goal;
    std::span<const TravelBoundary> forbidden;
};
enum class PathStatus { Ready, PendingTiles, NoSurface, Unreachable, AccessDenied };
struct Path {
    PathStatus status;
    std::vector<Vec3> corners;
    float length_m;
    std::uint64_t navigation_revision;
};
Path path(const Space&, IdentityRead, Handle body, const Goal&,
          std::span<const TravelBoundary>, Deadline);

enum class IntentState { Moving, WaitingNavigation, Arrived, Blocked, TargetGone };
struct IntentProgress {
    Handle body;
    std::uint64_t intent;
    IntentState state;
    float remaining_m;
    PathStatus path_status;
};
struct Contact {
    Handle first, second;
    Vec3 point, normal;
    bool started;
};
struct StepResult {
    std::vector<Handle> moved;
    std::vector<Bounds> dirty_bounds;
    std::vector<Contact> contacts;
    std::vector<RelationChange> relations;
    std::vector<IntentProgress> intents;
};
void step(Space&, IdentityRead, TransformWrite, ParentWrite, SpaceFlags,
          PhysicalRelations&, Step now, float seconds,
          std::span<const MoveRequest>, StepResult& out);

struct Tile { std::int32_t x, y; };
struct NavigationInstall {
    std::uint64_t revision;
    std::vector<Tile> installed, pending;
    std::vector<std::string> errors;
};
NavigationInstall install_ready_navigation(Space&);
std::uint64_t navigation_revision(const Space&);
```

The step module owns intent records, their causal origins, cancellation and
replacement. Space receives only active movement requests and returns progress
keyed by the same intent ID. Omitting/replacing a request resets the native target;
it does not manufacture cancellation records. The caller writes those exactly
once. An object goal resolves to a reachable standing point outside its shape
within reach, using the body's current navigable layer; it does not use the target
origin as a floor height. This avoids navigating onto the top of an object just
because its origin is elevated. Partial Detour paths are not successful arrival.
Long routes are split into intermediate crowd targets within the upstream
256-polygon local-planning range; that range is not a maximum walking distance.

Entry permissions are evaluated by the permission binding, which supplies the
currently forbidden numeric volumes. Space rejects a path/step segment entering
one and returns AccessDenied. It has no player or grant access. The initial
prototype reports denial rather than finding an alternate route around a
forbidden plot; an explicit intermediate goal can select another route. Automatic
permission-aware rerouting would require a custom Detour filter and per-agent
policy integration. That is a separate choice, not a silent authorization bypass.
Permission revocation replaces these value descriptors on the next step.

Per physics step: accept completed tile jobs at the boundary; reconcile crowd
proxies from actual character positions; run DetourCrowd steering; apply gravity
to each free CharacterVirtual velocity and call ExtendedUpdate with Z-up stair
and floor-snap vectors; advance rigid bodies once; follow attachments; copy actual
transforms; update dirty physical facts; return events. Dirty bounds include old
and new coverage from edits, additions, removals and motion; observation code uses
them to invalidate subscribed illuminance places. Actual Jolt movement is
authoritative. Crowd predicted positions are never copied to Objects.

DetourCrowd documents that it normally owns positions. Our coupling uses its
public getEditableAgent plus corridor.movePosition to reconcile the steering
proxy to collision-corrected motion, then updates npos and measured velocity.
This is an application integration policy, not an upstream guarantee. An airborne
character leaves the crowd and is reinserted after finding same-layer ground;
it never snaps to an upstairs polygon. The coupling must pass the staircase,
wall, two-agent doorway and moving-obstacle tests before implementation acceptance.

Edits change collision immediately and mark the old and new AABBs, expanded for
agent clearance and tile border, dirty. A worker receives an owning copy of only
the geometry instances intersecting those tiles. It runs the Recast pipeline and
dtCreateNavMeshData off-thread. A completion contains tile bytes, coordinate and
source revision, never pointers to live tables or a live dtNavMesh. The writer
discards stale results, removes/adds only ready tiles within the configured install
budget, and advances the public revision. Neighbor borders are rebuilt together
so floor/stair seams connect. Existing tiles stay usable until replacement; real
collision prevents walking through freshly built obstacles while navigation is
pending. Corridors touching replaced tiles are revalidated/replanned.

A pending build does not terminate an intent as blocked. A completed unreachable
path, or lack of physical progress for the configured interval with no pending
relevant tile, does. Retry/replan is driven by tile revision, moving goal changes
and actual obstruction, not a full-world bake every frame. The worker count and
tile-install budget are performance controls; they do not refuse construction.

## Persistence, diagnostics and validation

```cpp
struct MotionRow {
    Handle object;
    Vec3 linear_velocity_m_s, angular_velocity_rad_s;
    bool asleep;
};
void capture_motion(const Space&, IdentityRead, std::vector<MotionRow>& out);
void restore_motion(Space&, IdentityRead, std::span<const MotionRow>);
void rebuild_attachments(Space&, IdentityRead, TransformRead, ParentRead,
                         const Edges& attached_to);

struct NavigationArea {
    Vec3 point;
    std::uint32_t connected_component;
    bool reachable;
};
struct NavigationReport {
    std::uint64_t revision;
    bool complete, reference_navigable;
    std::vector<NavigationArea> areas;
};
NavigationReport diagnose_navigation(const Space&, std::span<const Bounds> changed_regions,
                                      Vec3 reference, Deadline);
struct DebugLine { Vec3 from, to; std::uint32_t rgba; };
struct DebugSelection { Bounds region; bool collision, navigation, paths, volumes; };
bool debug_lines(const Space&, const DebugSelection&, Deadline,
                 std::vector<DebugLine>& out);
} // namespace amber::space
```

Motion rows are the MOVE snapshot section; no Jolt StateRecorder is used. Restore
creates bodies from object rows and immutable geometry, restores velocities and
sleep, rebuilds attachment-local poses, bakes tiles, then restarts saved intents.
Derived contacts, native IDs and paths are never serialized. Character angular
velocity is zero; its linear velocity is restored explicitly. At-rest characters
may be omitted from active stepping until their controller/support becomes dirty.

Diagnostics label connected components at the installed navigation revision and
report whether authored probe points are navigable. The bake/placement adapter
can compare stair endpoints and author-supplied sites without space knowing their
names. Debug output uses actual collision shapes, sensor volumes, installed tile
edges and active paths. An idle world still shows geometry. Region/deadline bounds
control response cost and an incomplete response is indicated, never silently
treated as the complete scene.

The competing simple designs are to let crowd positions bypass collision, or to
drop crowd and implement steering ourselves. The first fails doors and new walls;
the second duplicates Detour's intended work and violates the chosen library
stack. The proposed explicit steering/controller coupling retains both, with
focused tests at their interface. For navigation, synchronous full-world rebakes
are simpler but violate the step budget; independently revisioned tile jobs are
the smallest design that implements the stated tiled-edit requirement.

Required module tests are: generation reuse; gravity/sleep snapshot continuation;
real baked static mesh and convex restore; stair-only access and tile seams;
glass/wall visibility; translated/renamed zone geometry; stationary containment
across sleep; attachment/grasp/release cycles; a chair blocking a translating
kinematic body; path replacement after a wall edit; pending/stale tile results;
forbidden-region crossing; and debug lines in an idle scene. Benchmarks separately
measure moving-character/crowd cost, visibility candidate/ray cost, dirty-tile worker
time, writer tile-install pause, and copying MOVE rows. Report p50/p95/max and
counts/scene dimensions rather than only an aggregate server frame rate.

## Pinned API evidence

These are the actual local pinned upstream headers used for the proposal:

* `third_party/jolt/Jolt/Physics/Character/CharacterVirtual.h`: settings, optional
  inner rigid body, SetLinearVelocity, ExtendedUpdate, RefreshContacts and
  CharacterVsCharacterCollisionSimple. The inner body is necessary for normal
  raycasts to hit a virtual character. Register every free character in the
  character-vs-character collection; crowd avoidance alone is not collision.
* `third_party/jolt/Jolt/Physics/Character/CharacterBase.h` and
  `Collision/Shape/CapsuleShape.h`: explicit mUp, supporting plane, feet-origin
  shape, and the capsule's native Y axis. Setting mUp alone does not rotate the
  capsule. Construct a Z-aligned, feet-offset shape and set all ExtendedUpdate
  step vectors in Z as well.
* `third_party/jolt/Jolt/Physics/Collision/NarrowPhaseQuery.h`: closest/all-hit
  CastRay, CollidePoint, CollideShape and translational CastShape.
* `third_party/jolt/Jolt/Physics/Body/BodyInterface.h`: SetShape,
  SetPositionAndRotation, MoveKinematic, GetLinearAndAngularVelocity, AddImpulse,
  IsActive, ActivateBody and DeactivateBody.
* `third_party/jolt/Jolt/Physics/Collision/ContactListener.h` and Body.h: callbacks
  may execute on native worker threads with bodies locked; callbacks only copy
  contact values. They never mutate store or call locking body interfaces. Removal
  may mean sleeping, and removed-body callbacks cannot dereference destroyed
  bodies. Handle generation and cached pair identity are checked on the writer.
* `third_party/jolt/Jolt/Physics/Collision/Shape/Shape.h`: SaveWithChildren /
  sRestoreWithChildren and triangle extraction; bare SaveBinaryState alone does
  not preserve external material/subshape references.
* `third_party/recast/DetourCrowd/Include/DetourCrowd.h` and DetourPathCorridor.h:
  addAgent, requestMoveTarget, resetMoveTarget, update, getEditableAgent,
  corridor.movePosition, agent vel/npos and the documented position-ownership
  limitation. The upstream comment's historical 20–30-agent estimate is not a
  prototype cap; benchmark this actual build and defer work only by the real budget.
* `third_party/recast/Detour/Include/DetourNavMeshQuery.h`, DetourNavMesh.h and
  DetourNavMeshBuilder.h: findNearestPoly, findPath, findStraightPath, poly-ref
  validity, removeTile/addTile and dtCreateNavMeshData. A replaced tile invalidates
  old polygon references; storing only world corners does not excuse replanning.

## Implemented density and custody details

Step processing iterates Jolt's active rigid-body IDs, the explicit character set and changed object IDs. It does not read every static body's transform or rewrite its flags. Bodies active before a physics step remain in the changed set even if that step puts them to sleep. Ordinary child objects follow changed parents through reverse attachment edges; moving a geometry-free assembly root also marks its children. Neighbor wake-up and per-tile source collection reuse Jolt's broadphase instead of introducing a duplicate spatial index. Bulk-loading tree optimization occurs at doubling body counts, giving amortized linear rebuilding cost.

Crowd capacity is separately configured as MAX_CHARACTERS, independent of MAX_BODIES. Query radius and path optimization radius are configurable body-radius multipliers; separation weight is a named Detour tuning value. None supplies an authored movement speed or strength.

`OperationResult custody(Space&, IdentityRead, Handle object, Handle custodian, ParentWrite, SpaceFlags, PhysicalRelations&, vector<RelationChange>&, span<const Handle> declared_descendants = {})` removes native geometry and physical relations, then sets the ordinary custodian parent and Held flag. A null custodian represents offline mailbox storage. The item is absent from queries, navigation and scene replication, rather than a query-visible attachment. Parent cycles and stale handles are rejected. The adapter supplies the declared-part descendant closure; all those bodies/volumes leave the scene while ordinary part parents/transforms/internal attachment edges remain. Unrelated attached occupants detach and stay in the scene. Reentering the scene uses the ordinary add/attachment operations with immutable asset geometry resolved by the caller. Permissions, player IDs, mailbox policy and transfer semantics do not enter space.

`register_logical_root(Space&, IdentityRead, Handle)` explicitly records that immutable metadata describes a geometry-free assembly root. It creates no native collider. Returning an assembly to the scene requires its actual body descriptors and logical-root registration through the ordinary installation/rebuild path. Custody preserves this derived registration, destruction removes it, and restore registers empty roots from their immutable asset metadata before play resumes.
