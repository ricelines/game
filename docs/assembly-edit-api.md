# Physical assembly replacement

The engine receives immutable geometry/profile descriptors and ordinary handles. Lifecycle owns named parts, identity generations, catalog bindings and authority. Space owns native bodies, physical attachment records, controller state and velocity. A replacement must rebuild the native assembly as one mechanical operation; changing only model metadata is insufficient.

Rigid promotion uses one Jolt compound body with summed disjoint own masses. A root with real geometry contributes its own convex shape and mass; a geometry-free root contributes neither. Every declared physical child retains its ordinary handle and individual query shape. A physical root additionally retains its original mesh as a query-only native body, associated with the same root handle. Visible rays skip the aggregate collider, so root/child materials and real mesh openings remain observable without duplicate transmission. This extra root query body costs one native slot, explicitly included in capacity checks. Non-solid parts contribute actual mass without becoming solid.

The alternative is a fixed-constraint body graph. It retains individual collision shapes but introduces constraint tuning, solver convergence and a different behavior for geometry-free roots. The existing immutable compound representation is the smaller consistent choice. A metadata-only replacement or independently teleporting parts would lose actual collision/mass truth and is excluded.

```cpp
struct EditedMotion {
    Handle object;
    Motion motion;
    Vec3 linear_velocity_m_s, angular_velocity_rad_s;
    bool asleep;
};
struct AssemblyEdit {
    Handle root;
    bool in_scene, promoted;
    std::vector<Handle> before; // root + complete declared descendants
    std::vector<EditedMotion> motion;
};
AssemblyEdit extract_assembly(Space&, IdentityRead, Handle root,
                              std::span<const Handle> declared_descendants);
Handle assembly_root(const Space&, IdentityRead, Handle);
BodyPreflight can_rebuild_assembly(const Space&, std::span<const BodySpec> future_bodies,
    std::size_t sensor_count, std::span<const Handle> before,
    bool promoted);
OperationResult rebuild_assembly(Space&, IdentityRead, TransformWrite, ParentWrite,
    SpaceFlags, PhysicalRelations&, const AssemblyEdit&,
    std::span<const BodySpec> new_bodies, std::span<const Handle> logical_roots,
    std::span<const VolumeSpec> new_sensors,
    std::span<const Attachment> internal_attachments,
    std::vector<RelationChange>&);
```

`before` and descendants are unique complete handles. Future BodySpecs may have null handles during preflight; missing logical roots are separate because they require zero native bodies. Preflight validates supported geometry/profile modes, counts the resulting bodies/sensors and possible aggregate collider, and credits all old bodies (including a physical root's query body) and owned sensors. It reserves no transaction ticket: the single writer applies the prepared operation without yielding. Heap exhaustion is ordinary process failure.

For an edit selecting an existing rigid member, assembly_root returns its enclosing mechanical root. Rebuilding that complete rigid span may include descriptors outside the selected authored subtree; those outer descriptors remain unchanged. Permission, resource and catalog mutation scope does not widen. Preflight rejects an incomplete existing aggregate before extraction. Credits count each complete handle once, including its separate root query body and owned sensors. A compound containing only non-solid physical members is valid: it has a query-only dynamic aggregate with real summed mass. A promoted assembly with no physical geometry at all fails preflight.

Composition order is:

1. Resolve and import every new immutable geometry off the simulation step; resolve all profile, material and sensor descriptors. Determine whether the resulting assembly needs rigid promotion: it was already promoted, is currently grasped, or has dynamic root motion with descendants. Ordinary scripted moving parts remain independent while their root is merely placed.
2. Preflight the complete new native demand and permissions/resources. Capture/extract the complete old native assembly. Extraction removes its colliders/controllers/sensors and invalidates affected navigation/optical bounds, preserving authoritative transforms/parents/physical relation rows and native attachment records. Motion DTOs preserve current modes, velocities and sleep state. Root pose stays actual, including when held or in flight.
3. Retire removed declared subtrees through normal lifecycle cleanup while their identities are valid. External avatars attached to removed parts detach in place; retained part attachments remain. Apply lifecycle's replacement, preserving matching handles/authority and creating only genuinely new named parts. Update root/part catalog bindings and the new local poses.
4. Rebuild with actual new handles and descriptors. New internal attachment DTOs come from the ordinary declared graph and new actual transforms. Retained external root/grasp/seat attachments keep their prior parent, local pose and grasp mode. Retained moving parts keep supported motion state; newly added parts use their declared profiles. Promote the complete new assembly when required, restoring the root's actual mode/velocities after the new geometry/mass exists. Follow retained external occupants with the new parent pose.
5. Re-register the separate presentation/light/zone/replication/watch adapters using the resulting live handles. Ordinary changed facts and causal records belong to composition, not Space.

Inventory/mailbox replacement remains hidden. The captured in_scene flag is false when custody removed all native geometry and set Held. Rebuild updates ordinary parent/attached relation rows and keeps all new descendants Held; it creates no query-visible collider, volume or attachment cache for stored goods. A physically grasped root is distinguishable because it still has actual native bodies, despite also carrying Held. Geometry-free↔physical root edits update explicit logical-root registration accordingly.

Root physical query shapes are updated only when their real aggregate moves, using the native active/dirty sets. Inert assemblies add no per-step full-world scan. Native extraction destroys both root aggregate and retained original query bodies. Disassembly restores the root's own shape/mass and ordinary independent child bodies, reclaiming the aggregate shape cache. Snapshot reconstruction continues to recreate ordinary objects/attachments, promote saved rigid assemblies, restore modes and then restore velocities.

Extraction retains native attachment records until the caller retires removed parts. Ordinary remove therefore still detaches an external occupant of a removed seat even though the seat's collider has already been extracted. Retained attachment records survive. Motion capture emits one row per authoritative handle; the root aggregate and its original query shape never produce duplicate MOVE rows. Removing an individual compounded member outside replacement rebuilds the remaining aggregate rather than dropping the enclosing root collider.

Tests cover a physical10kg root plus two10kg children (30kg total), root/child distinct transmission, non-solid members, actual impulse/flight, held root geometry replacement with retained grasp, a retained seated avatar, a removed seat part detaching only its external occupant, added/removed part topology, dynamic root velocity continuation, geometry-free↔physical root transitions, offline custody remaining invisible and native-capacity failure before mutation. Benchmark replacement separately from steady physics; no root/body/model spelling selects any behavior.
