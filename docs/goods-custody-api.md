# Custody and placed-plot queries

Visible administrative custody uses separate `space::VisibleCustody{goods,parent,grasp}`.
Its preflight checks live identities, resulting cycles, supported geometry and
temporary native capacity for attachment or unrelated occupant release. Commit
preserves actual poses/native geometry, detaches external occupants, retains
declared members, and clears or restores the explicit root parent/grasp. It has
no ledger/ownership access and performs no collision-gated authored throw.
Hidden inventory reveal remains a prepared geometry operation. Hidden custody
retires goods first so an exact-full native pool can use a released slot when
restoring unrelated occupants to ordinary motion.

Status: approved and implemented in `server/bindings/goods.*` and owner-owned `perm::prepare_store_goods/commit_store_goods`, with focused real-VM tests in `tests/goods_test.cpp`. The operation names are generic mechanical calls. Authored scripts own rent, abandonment, grace periods, return and salvage policies. A world rule, architect approval or plot ownership supplies no authority over goods.

`self:plot_objects(plot_id)` returns acquired handles for currently placed ordinary goods roots assigned to that plot. It excludes Body, Held and declared child objects. Held also covers inventory/mailbox rows under the existing space custody contract. Filtering declared children uses the separate declared-parts relation, not arbitrary physical parents, so attached external occupants cannot become assembly authority. Returned handles permit subsequent reads, not writes. An invalid plot ID is an input error.

```cpp
struct PlotObjectsBinding {
    IdentityRead identity;
    const std::vector<PlotId>& plot;
    const Relation& declared_parts;
    const perm::Plots& plots;
};
int plot_objects_binding(lua_State*);
```

The implementation scans the identity/plot projections only when the query is called. Rent queries occur on authored timers, not every simulation step; this adds no recurring inert-object work or placement-index synchronization. A release probe with100k unrelated rows measured191us p50/252us p95/291us maximum over100 actual Luau calls on the shared host. A derived plot-to-handle index is the alternative if measured query frequency makes this scan material; it would require birth, retirement, plot reassignment and scene-custody hooks. A spatial box query is unsuitable because assigned plot membership, rather than overlap with an AABB, is the requested fact.

`self:hold_goods(handle, 'p:N')` removes one complete declared assembly from the scene and delivers its root to its current owner's inventory or mailbox. The recipient must equal the current goods owner. It preserves owner IDs, next-owner ceilings, ordinary script grants and the internal part graph. It does not call a same-owner gift: that operation revokes grants and would accidentally destroy explicitly delegated later salvage authority. Body and escrowed goods are rejected. Each descendant must be live, have the same owner and retain Transfer under its own ceiling. A root Transfer grant covers its ordinary declared assembly under the existing permission contract.

Authority is always the current installed ScriptHolder `{script,installation}`. No direct-body actor is borrowed, and no native branch recognizes rent, a treasury, a plot name or an issuer's architect role. The initial plan must supply an owner-issued narrow grant such as OwnedOnPlot(owner,plot) Transfer to the actually installed controller. Later salvage independently requires Destroy under an owner-approved scope. Transferring the plot does not confer either right over the former owner's goods.

The binding needs only:

```cpp
struct DeliveryTargets {
    std::unordered_map<PlayerId, Handle> present_bodies; // derived, updated on join/leave
};
struct HoldGoodsBinding {
    space::Space& space;
    IdentityRead identity;
    AuthorityRead authority;
    ParentWrite parents;
    SpaceFlags flags;
    space::PhysicalRelations& relations;
    const Relation& declared_parts;
    const perm::OwnerLimits& limits;
    const perm::Grants& grants;
    perm::Players& players;
    const DeliveryTargets& delivery;
    std::size_t mailbox_max;
    std::vector<space::RelationChange>& relation_changes;
    SceneProjection& scene;
};
int hold_goods_binding(lua_State*);
```

DeliveryTargets is a narrow derived presence projection, not access to authentication keys, sessions or a broad player service. Its owner supplies an actual live body for currently present players; absent recipients use the existing offline mailbox. Inventory/mailbox holds the assembly root only. The parent of an online root is that body; offline parent is null. Every declared descendant becomes non-query-visible while internal ordinary parents remain intact. External occupants detach and remain in the scene through existing space::custody semantics. After actual physical custody, the binding calls `scene_visible(scene, closure, false)` immediately. That fixed operation removes replica/light/zone/process projections and invalidates observation metadata; no separate removal buffer waits for a later phase.

Permission/inventory ownership should remain in perm. The proposed owner-owned split is:

```cpp
struct StoreGoodsPlan {
    perm::Reason reason;
    Handle goods, custodian;
    PlayerId recipient;
    bool present;
};
StoreGoodsPlan prepare_store_goods(const perm::Players&, const perm::Grants&, perm::Holder,
    const perm::Target&, std::span<const perm::Target> declared_parts,
    PlayerId recipient, Handle recipient_body, bool present, std::size_t mailbox_max, Step);
void commit_store_goods(perm::Players&, const StoreGoodsPlan&);
```

The binding resolves the actual complete declared closure and delivery descriptor, preflights all permission/delivery constraints, calls real space::custody, then commits the already checked inventory change without yielding. Native stale/cycle failures leave inventory untouched. Successful preparation makes commit deterministic; no reauthorization or new delivery decision occurs between these calls. Allocation failure is ordinary process failure, not a transaction subsystem. Commit removes any previous duplicate root delivery before inserting the accepted location, making repeated custody requests idempotent. An already stored item in the requested delivery location returns held with a NoEffect record and no changed flag. It never mutates grants, owner ceilings or money.

Return `{effect='held',object,recipient,location='inventory'|'mailbox'}` or `{effect='denied',reason,...}` using the permission owner's existing reason strings; native geometry/cycle failures report their actual status. Completed effects include an ordinary causally attributed System record. Tests must use a real Luau handler and Space: owner-granted success, plot Transfer alone denied, grant expiry/revocation denied, explicit instance isolation, assembly descendants hidden with parents preserved, external seated body remains, online/offline delivery, repeated delivery without duplicates, recipient mismatch, Body/Escrow rejection and mailbox-full failure with no half mutation. No native rent policy tests belong here.
