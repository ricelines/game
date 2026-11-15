# Physical Luau bindings API proposal

Status: implemented and tested with the actual Luau VM and Jolt geometry. These bindings connect authored handlers to existing mechanical owners. They do not implement action routing or recognize body verb spellings. Removing a body's declared handler removes its behavior. The dispatcher and administrative action handlers remain separate root work.

## Invocation and authority contract

Each binding obtains the transient react::Invocation already owned by the VM. Its binding-local data contains only that operation's named tables. There is no PhysicsHost holding every adapter, captured composition lambda or retained authority ticket.

Append these declaration fields through the react/catalog owner, who also updates source extraction and immutable declaration JSON:

```cpp
struct ActionDeclaration {
    TermId action;
    string right, function;
    string right_on = "selected";   // validated "self" or "selected"
    bool reach = false;
    vector<string> mechanics;       // validated host primitive names below
};
struct BodyEndpoints {
    Handle body, selected, held;
    PlayerId actor = 0;             // authenticated immediate input only
    vector<string> mechanics;       // copied from that validated action declaration
};
```

`mechanics` accepts navigation, attachment, grasp, release, impulse, sweep, sound, transfer and avatar_control. It selects no gameplay implementation; a new authored action may compose these primitives. The dispatcher selects the body declaration first, keeps Invocation.self as its actual installed receiver, checks its declared right/reach on right_on's self/selected endpoint and constructs BodyEndpoints only for that direct body handler. It verifies self is the authenticated body. Event/timer/watch work carries Origin and acquired handles, never BodyEndpoints. `self:act` still queues the installed script holder, irrespective of this envelope.

A primitive first uses normal installed-script authority: implicit move/edit on self and declared parts, ordinary grants/presets for foreign objects, and avatar animate grants for foreign avatars. If it needs the immediate player, its primitive must be listed in mechanics, and every endpoint must be body, selected target, current held source or their explicit declared-part descendants. A relation, nearby query or arbitrary handle can acquire read access but cannot enlarge this player's mutation scope. The selected/held handles are checked live; held must still be grasped by this body. The direct envelope expires when the handler returns. Reusing the same checked selected right need not redo the action check; a second endpoint's right is checked before mutation.

The independent `declared_parts` relation is the only implicit own-part authority source. ParentRead includes seats, custody and grasps and therefore grants no own-part authority. Placement/lifecycle maintains the relation and supplies actual child handles, including descendants. Ownership transfers affect all those ordinary goods through perm's assembly operations.

## Primitive rights and effects

| Primitive | Endpoints and required authority |
|---|---|
|navigation|Move on invoking body (implicit own-script movement), or animate on foreign avatar. Validate forbidden land entry; approaching an object does not itself need Use on that object.|
|attachment|Move/animate on subject, Use on parent, within declared physical reach for direct requests. Own self/part motion remains implicit.|
|grasp|Take on selected item and control of invoking body/site. Take authorizes this custody/physical attachment; do not add an unrelated extra Move requirement.|
|release|Control of invoking body plus current grasp relation. Relinquishing its held item remains possible after Take is revoked, so revocation cannot strand a grasped item.|
|impulse|Move on the physical subject, or own self/part motion. Applying an impulse never infers damage or emits a fabricated impact.|
|sweep|Move on self or an explicitly declared part; foreign parts require their ordinary Move authority. Use actual collision geometry.|
|sound|Emit from invoking self or declared emission site, with authored radius/content. No remote arbitrary sound emitter using a direct player's authority.|
|transfer|Take/debit authority on source and Use/receive authority on destination. Own self/declared-part state writes remain implicit. Both foreign endpoint checks finish before either value changes.|
|avatar_control|Only current direct body actor can grant its own Avatar scope to an installed script on selected target; normal grants/expiry/revocation afterward. No automatic grant on attachment.|

Physical reach uses the authored body/site and measured target bounds with S5's configured reach default. Bindings do not depend on target names or maker claims. Read-only position/near/ray/light operations require acquired handles/query radius, not mutation rights. The caller is told denied need/on/reason when an endpoint fails. Ordinary reaction partial effects remain after later errors; a single conserved transfer is one inseparable debit/credit.

## Exact adapter contexts and functions

These public types live in `server/bindings/physical.hpp` (one module library can contain several small context types). Names below show all access.

```cpp
struct NavigationBinding {
    step::Intents& intents;
    vector<step::IntentEnding>& endings; // caller publishes replaced/cancelled intents
    IdentityRead identity;
    AuthorityRead authority;
    const perm::OwnerLimits& limits;
    const perm::Players& players;
    const perm::Grants& grants;
    const perm::Plots& plots;
    const Relation& declared_parts;
};
struct AttachmentBinding {
    space::Space& space;
    IdentityRead identity;
    TransformWrite transforms;
    ParentWrite parents;
    SpaceFlags flags;
    space::PhysicalRelations& relations;
    vector<space::RelationChange>& changes; // caller publishes relationship drafts
    const Relation& declared_parts;
    AuthorityRead authority;
    const perm::OwnerLimits& limits;
    const perm::Players& players;
    const perm::Grants& grants;
};
struct ImpulseBinding {
    space::Space& space;
    IdentityRead identity;
    AuthorityRead authority;
    const perm::OwnerLimits& limits;
    const perm::Players& players;
    const perm::Grants& grants;
    const Relation& declared_parts;
};
struct SweepBinding {
    space::Space& space;
    IdentityRead identity;
    TransformWrite transforms;
    AuthorityRead authority;
    const perm::OwnerLimits& limits;
    const perm::Players& players;
    const perm::Grants& grants;
    const Relation& declared_parts;
};
struct PartPoseBinding {
    space::Space& space;
    IdentityRead identity;
    TransformWrite transforms;
    const Relation& declared_parts;
};
struct QuantityBinding {
    react::Properties& properties;
    react::Processes& processes;
    const react::Terms& terms;
    IdentityRead identity;
    AuthorityRead authority;
    ParentRead parents;
    const Relation& declared_parts;
    const Relation& grasped; // physical attached_to, interpreted with Held flags
    const perm::OwnerLimits& limits;
    const perm::Players& players;
    const perm::Grants& grants;
};
struct PositionBinding { IdentityRead identity; TransformRead transforms; };
struct QueryBinding { const space::Space& space; IdentityRead identity; float max_radius_m; };
struct SoundBinding {
    observe::Hearing& hearing;
    IdentityRead identity;
    TransformRead transforms;
    const vector<observe::Listener>& listeners; // active listener DTOs only
};
struct AvatarControlBinding {
    perm::Grants& grants;
    perm::Players& players;
    IdentityRead identity;
    AuthorityRead authority;
    const SparseColumn<vector<ScriptId>>& installed_scripts;
    const catalog::Models& models; // current default script remains installed
};
```

No context owns the VM or another context. Registration calls react::register_binding separately for each C function and its concrete context. References are to stable table/vector objects, never borrowed spans retained across object allocation. Transform/grasp functions return physical relation/contact drafts; the caller publishes them through the existing single-writer phase and preserves the handler's Origin. None owns Journal/WorkQueues just to obtain a universal path to other state.

```cpp
int navigate_binding(lua_State*);   // self:navigate(point_or_handle, options?)
int cancel_motion_binding(lua_State*);
int attach_binding(lua_State*);     // self:attach(subject,parent,local_pose)
int grasp_binding(lua_State*);      // self:grasp(selected,local_pose)
int release_binding(lua_State*);    // self:release(held_or_own_attached_body_part,linear?,angular?)
int impulse_binding(lua_State*);    // self:impulse(subject,vector,point)
int sweep_binding(lua_State*);      // self:sweep(part,translation)
int transfer_binding(lua_State*);   // self:transfer(source,destination,term,amount)
int position_binding(lua_State*);   // self:position(acquired_handle)
int transform_binding(lua_State*);  // self:transform(acquired_handle)
int pose_batch_binding(lua_State*); // self:pose_batch({{object,position,rotation?},...})
int move_part_binding(lua_State*); // self:move_part(part,world_position,unit_rotation)
int near_binding(lua_State*);       // self:near(point,radius)
int ray_binding(lua_State*);        // self:ray(from,to)
int sound_binding(lua_State*);      // self:sound(payload,radius,local_site?,{kind='sound'|'speech'}?)
int allow_avatar_binding(lua_State*);
int revoke_avatar_binding(lua_State*);
```

Handles remain typed userdata, not Lua floating-point IDs. Vectors/poses are finite typed arrays/records validated at the boundary. Queries add returned live handles to Invocation.acquired. No handle stored in self.state becomes mutation authority. Deadlines use the current invocation cutoff; incomplete queries report incomplete rather than inventing a clear ray or an empty world.

Navigation returns `{effect="started",intent=id}` using step::start_move and retains the existing replacement/cancel semantics. Starting or cancelling the invoking object's navigation changes its movement intent, so neither marks world facts changed. Completion comes from space's actual intent progress. The driver turns denied plot bounds into the existing forbidden travel polygons. Requested speed is an optional float32 speed_m_s field in step::Intent, persisted in QUEU and passed to space::MoveRequest each step. start_move appends an optional parameter defaulting to absent. Absence uses the authored CharacterSpec maximum; a supplied speed is finite/nonnegative and clamps to that maximum. No native charge/slow-mode rule is added.

`self:transform(handle)` returns the actual `{position=vector,rotation={x,y,z,w}}` pose through the same read-only acquired-handle boundary as position. `self:pose_batch` accepts up to shared-config POSE_BATCH_MAX rows (1024 by default), each with object, position and optional unit quaternion. All rows are checked before any mutation: live acquired handle, independent non-solid Kinematic physical object, not Body, finite pose and ordinary installed-script Move permission or declared-own-part authority. Direct body authority is never borrowed for this batch. Omitted rotations preserve the current actual rotation. Changed poses are submitted to real Jolt bodies and ordinary relation updates; the return value is the number changed. The manifest's explicit physics.collision_solid=false controls the non-solid mechanical profile. A controller can therefore move hundreds of ordinary independently replicated objects with one Lua call, while its Lua code owns the path/bounds behavior.

`self:move_part(part, position, rotation)` teleports one actual declared descendant of the invoking object to a world-space pose. Position is a finite Luau vector and rotation is a unit `{x,y,z,w}` quaternion. The target must be an independent Kinematic physical part and cannot be a Body or the invoking root itself. A parent/grasp relation or foreign Move grant does not make an object a declared part. Solid parts are supported, and set_pose updates their actual Jolt body and attachment offset. Authored motion that must stop at obstructions uses sweep instead.

Sound emits from the invoking object or its local emission site. The default channel queues a System record named sound, including the typed payload, actual world position, radius and script attribution. An explicit options argument `{kind='speech'}` queues an S7 Speech record instead; payload text and action names never select that classification. Speech in a validated immediate body invocation names its actual player actor; autonomous scripted emitters retain the script actor. Script author attribution remains available in either case. Hearing remains distance-based for either channel. The record draft uses the invocation's existing causal Origin when composition appends completed effects. Audio records have result Applied and changed=false: an event-only sound or speech does not mutate stored world state. Emitting sound preserves any actual stored state change already made by the same handler.

Attachment/grasp returns the achieved relationship or a concrete obstruction/unsupported result. AttachmentBinding receives configured reach_m. The binding supplies local pose data resolved by the script; it does not search for a seat by name. A pose uses `{position=vector.create(x,y,z), rotation={x=0,y=0,z=0,w=1}}`; omitted pose fields use identity. Positions, directions and velocities use native Luau vectors. Custody extraction of multipart goods uses space::custody's explicit declared-descendant closure.

A physical root or explicitly registered logical root can be promoted through space::assemble using the complete unique declared descendant span. This constructs a real pinned Jolt StaticCompoundShape from the root's own convex geometry, when present, and ordinary children's convex shapes. "Static" refers to the immutable child layout, not a requirement for a static physical body: a convex compound is movable. It has one rigid root body, summed disjoint own masses, mass-weighted solid friction and maximum solid restitution. Child identities/transforms/parent relations remain ordinary; their retained native shapes are query-only while the compound supplies physical collision. A physical root retains its original mesh as a separate query-only shape. Optical rays use those individual original shapes/material transmission and omit the aggregate collider. No assembly volume is counted again.

Grasp automatically prepares this compound before calling ordinary attach. Release uses real dynamic flight and collision. Independent scripted member motion while compounded returns UnsupportedMotion before changing geometry. Structural authoring instead preflights and extracts the complete mechanical assembly, then rebuilds its actual bodies, ordinary attachments and aggregate while preserving velocity. Independently placed scripted parts remain independent; dynamic or grasped assemblies retain their rigid layout. Newly created runtime compound geometry is reused across regrasp and reclaimed on root retirement. Snap restoration recreates ordinary objects/attachments first, calls assemble for saved promoted roots, restores the saved root motion mode, then restores MOVE velocities. Composition owns that order.

The existing sweep is translational. A rotational extension would use actual Jolt overlap/cast checks over bounded angular substeps, not pretend a linear cast covers angular motion. A sliding physical part satisfies the prototype door workload. Rotation by a plain transform edit remains an administrative placement edit, not a collision-checked scripted sweep. This explicit mechanical limit is preferable to introducing a bespoke continuous angular collision system without a measured workload.

## Conserved transfer

The primitive invokes existing react::transfer_state only after rights preflight. It accepts one declared conserved state quantity with no combiner and a finite nonnegative amount. Both handles must be live. Identical endpoints return0. A missing destination starts at zero; a missing source supplies zero. Evaluate any processes at current step, clamp to available source and write equal decrease/increase without yielding. Return actual moved SI quantity. The effect list records both changed answers in call order. Any process continuation/bounds follow react's existing transfer semantics; no quantity disappears in an endpoint's clamping.

This does not recognize containers/liquids, apply capacity policy, mix substances or create spills. An authored handler reads measured capacity and its declared stored term, computes accepted amount and chooses remainder policy. Failure to create an authorized spill object leaves the untransferred amount at the source. Body own-state initialization/set/rate remain ordinary S4 operations; there is no invented global source/sink authority layer.

Example complete trace: a direct authored body action selects vesselB, reads held vesselA via its acquired/current grasp handle, reads A's declared stored term and B's capacity, requests transfer(A,B,term,amount), then receives actual moved amount. The host checks A Take and B Use under the bounded immediate body actor before calling the generic transfer. It does not emit impact. If B's Use fails, neither quantity changes; prior unrelated handler effects remain. A later timer has only installed-script authority and cannot reuse that player's Take/Use.

## Explicit avatar consent

`self:allow_avatar(selected, script_id, rights, seconds)` is allowed only in an avatar_control-enabled direct body invocation. selected must match that envelope, the script must be either its current model default or an explicit instance installation, rights are limited to bit0 animate/bit1 camera and expiry is required. Issuer/controller is the immediate actor's own body Avatar scope. The adapter calls ordinary perm::create_grant and returns its GrantId; the body's flat self.state can retain that integer for later explicit revocation on another direct action. It grants nothing merely because attachment occurred.

`self:revoke_avatar(grant_id)` likewise requires the immediate own-body envelope and a matching issuer/Avatar scope; it may revoke an earlier consent even if the current selected target is absent. Timers have no immediate actor and cannot impersonate it. The ordinary expiry timer and permission revocation paths remain authoritative. A selected seat script later uses normal check_scope for animate/camera; its author's player identity supplies no authority.

## Validation

Focused Luau tests install two differently named body actions that compose these same primitives. Removing/replacing a handler changes behavior with no native edit. Test allowed selected/held endpoints and denied unrelated acquired handles, denied second transfer endpoint with no half-write, process-backed conservation, revocation while holding, timer loss of direct authority, installed-script instance isolation, own-part sweep and real chair obstruction, actual impulse/release flight, distance hearing and explicit avatar consent/expiry. Reuse real space geometry/controller tests and independent perm tests; avoid mocking collision or replacing the VM with direct C++ effects. Benchmark handler+binding calls and multi-observer queries under the actual fixed-step load.
