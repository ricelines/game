# Physical material edits

`bindings::MaterialRefresh` reads the imported geometry map, immutable
model/parameter/assets tables and `react::ValueRead`, together with explicit
physical role IDs and defaults. Its only mutable endpoint is Space. It has no
access to grants, scripts, prose, ledger or inventories.

`prepare_material_refresh(context, changed_material_ids, step, hz)` performs a
rare edit-time identity/material scan. It resolves each affected object's
actual parameter asset and uses the ordinary `describe_physics` path. The
returned plan contains every affected handle, including logical roots and
hidden goods, and numeric property updates for present physical bodies. It
rejects invalid mass/surface inputs before applying any update. ValueRead can
reference prospective material rows, allowing preparation before registry
publication.

`apply_material_refresh(space, identity, plan)` calls native `set_properties`
without yielding. Existing bodies, shapes, ordinary handles, attachment
offsets, motion and velocities remain in place. A promoted assembly preserves
its root query material and each ordinary child's optical truth. Member mass
updates change aggregate mass/inertia, mass-weighted friction and maximum
restitution. CharacterVirtual receives its updated configured mass. VM state,
timers and authority are untouched.

Hidden goods have no native update; their current catalog material is used on
later placement. Their affected handles still invalidate derived mass
immediately. Empty-root aggregate mass comes from its physical members; an
empty root never acquires fabricated positive own mass. Physical properties
emit ordinary dirty bounds so shadows/observer caches update normally. The
caller uses affected handles for mass/projection invalidation and ordinary
intrinsic fact changes for scripts. There is no per-tick material scan.

Recreating bodies would consume temporary capacity and disturb held
relationships or motion. Pinned Jolt supports mass/inertia changes in place,
so reconstruction remains reserved for actual geometry/topology edits.
Compound inertia is scaled proportionally, the same prototype approximation
as initial compound construction; disjoint total mass and optical geometry
remain exact.

The real structural regression changes a root/child from 1016kg to 508kg,
verifies unchanged native slots/attachments/velocity, and proves the new mass
with a Jolt impulse. It also checks optical transmission and a hidden-custody
edit that creates no collider.
