# Native permissions, exchange and inventory

This slice exposes the existing `perm` operations through ordinary S6 native actions. It follows Technicals §13–14 and Specs S9, including the next-owner resolution in [perm-api.md](perm-api.md). It adds no pricing, billing, issuance schedule, constructor resource formula, institution policy or workflow engine.

## Actions and values

`declare_commerce(Terms&, NativeActions&) -> Declarations` uses `command::declare_native`; run it before registry pinning/script preparation, and reuse the same immutable definitions on restore. Each operation has its own validator, native function and local context. Structured `inputs_schema` supplies descriptions/examples directly to the existing actions tool.

Identifiers use `p:N`, `g:N`, `s:N`, full `o:SLOT.GENERATION`, `offer:N`, `trade:N`, and `grant:N`. Money/caps are nonnegative signed 64-bit whole smallest currency units. Durations are finite nonnegative world seconds converted once with configured step frequency. Wallets are identified only by `p:N`; a script holder never indexes the player table as its actor.

| Action | Target / arguments | Authority and behavior |
| --- | --- | --- |
| `engine.pay` | `{to,amount,from?}` | Defaults player payer to self; script payer selector to its installed object's current owner wallet. Script payment requires an explicit ordinary capped Spend grant on that player wallet. |
| `engine.balance` | `{account?}` | Read a valid player wallet balance; default follows the same payer identity. |
| `engine.inventory` | `{}` | Player's inventory/mailbox roots, preserving full handles. |
| `engine.give` | goods target; `{recipient}` | Ordinary Transfer on complete declared assembly; current recipient delivery descriptor; offline mailbox cap preflight. |
| `engine.offer` | placed goods target; `{price,buyer?,expires_s?}` | Ordinary Transfer, current full part closure, no Body/escrow/inventory goods. Income enters the actual seller's player wallet. Goods remain visible in ordinary escrow custody. |
| `engine.offers` | optional goods target; `{}` | Active ordinary offers public to this buyer, or created by this seller; includes IDs, goods, price, buyer, expiry. |
| `engine.accept` | `{offer,payer?,beneficiary?}` | Recheck current offer/parts, payer authority/cap/balance, buyer restriction and delivery capacity before any mutation. Player defaults beneficiary to self; script must explicitly name beneficiary and still has only its own grants. |
| `engine.cancel_offer` | `{offer}` | Seller's current ordinary Transfer check; release escrow and keep placed goods at their actual pose. |
| `engine.trade_open` | `{other}` | Both player bodies present; create bilateral row. |
| `engine.trade_change` | `{trade,money,goods?}` | Participant's current goods/parts and prior custody; money-only side allowed; clears both confirmations. |
| `engine.trade_confirm` | `{trade}` | Both present bodies, current complete goods and both balances rechecked; second confirmation commits the one swap. |
| `engine.trade_cancel` | `{trade}` | Participant releases each side to prior inventory or world custody. |
| `engine.grant` | `{holder,scope,rights,cap?,period_s?,expires_s?}` | Player issuer; shared readonly scope decoder resolves current controller/ceiling. Script holder must be actually installed. Ordinary one-level delegation, owned recipient and cap/expiry rules remain `perm::create_grant`. |
| `engine.revoke` | `{grant}` or `{holder}` | Issuer or architect; removes children. Holder form revokes this issuer's grants to that holder. |
| `engine.list_grants` | `{}` | Indexed given/held rows, scope/holder, remaining allowance and expiry. |
| `engine.preset` | goods target; `{everyone?,group_rights?,next_owner?,group?,no_copy?}` | Current owner only, no escrow edits; inherited edit/copy/transfer ceiling cannot be widened. Group membership checked; no-copy stays separate from transfer. |
| `engine.place_existing` | inventory goods target; `{position,rotation?}` | Existing owner inventory/mailbox custody, ordinary caller Move and destination Build; preserve all root/part IDs and author/model/limits. Rebuild actual geometry after native capacity preflight; no creation or resource debit. |
| `engine.group_create` | `{name}` | Player creates a named group with self as controller. |
| `engine.group_member` | `{group,player,member}` | Controller changes sorted membership, or a member leaves themself. |
| `engine.plot_permissions` | `{plot,everyone?,group_rights?,group?,banned?}` | Plot owner changes build/enter/run-scripts bits, assigned group and sorted ban list. |

Player administration (trade confirmation, grants, presets, groups and plot settings) requires a player caller. Payment, gifts and ordinary vendor offers/acceptance also accept authenticated installed-script callers under their actual grants. Native definitions name no player, resource, model, place or institution.

## Administration registration and input details

`server/bindings/commerce/admin.hpp` is independent of the exchange adapter. The consolidated `amber_bindings` library exports `declare_admin(Terms&, NativeActions&) -> AdminDeclarations` and seven matching `validate_NAME(Json)` / `NAME(void*, NativeCall)` functions. Call the declaration builder before pinning the registry, then register each ID with `drive::register_native(operations, ids.NAME, {validate_NAME, NAME, &its_context})`. The builder reuses and verifies existing reserved definitions on restore. The contexts below contain no session or world aggregate.

| Context | Required data |
| --- | --- |
| `GrantBinding` | Mutable Grants/Players; readonly `authority::ScopeRead`, Models, Scripts, explicit InstanceScripts, RuleVersions and ValueRead. The latter two verify actual currently applicable approved world-rule holders, as well as ordinary default/instance installations. |
| `RevokeBinding` | Mutable Grants/Players; readonly Profiles to check the actual architect role. |
| `ListGrantsBinding` | Readonly Grants/Players, using the caller's given/held indexes. |
| `PresetBinding` | Mutable group/preset columns through `PresetColumns`, masked PermissionFlags; readonly IdentityRead/AuthorityRead, owner limits, Players/Groups. It cannot write owner or plot membership. |
| `GroupCreateBinding` | Mutable Groups/Players membership. |
| `GroupMemberBinding` | Readonly Groups; mutable Players membership. |
| `PlotPermissionsBinding` | Mutable Plots; readonly Players/Groups. |

Mutating contexts carry `AdminInvalidation{local, changed(void*)}`. The composition refreshes permission-dependent views after an actual change; this rare edit callback receives no mutable world data. Grant replies include the actual new `grant:N` ID and details; grant creation also indexes its deadline in the permission owner. Current checks and list responses reject expired rights immediately, even before cleanup removes the row.

`holder` is either `"p:N"` or `{"script":"s:N","on":"o:SLOT.GENERATION"}`. Scope has exactly one field: `{"object":"o:S.G"}`, `{"owned_on_plot":{"owner":"p:N","plot":N}}`, `{"account":"p:N"}`, `{"avatar":"o:S.G"}`, `{"plot":N}` or `{"issuance":true}`. The account scope is always an existing player's wallet. Script installation and scope controller claims come from live tables, never the input. A script grant must name a currently installed script; revocation may name a retained full holder handle after that installation has gone away.

Grant duration fields are relative world seconds. Omitted or zero `expires_s` means no expiry, which the underlying permission check rejects for spending, issuance and script holders. Financial grants also need a positive `period_s` and have a whole-unit `cap` (default zero). `list_grants` returns separate `held` and `given` arrays containing only rights currently in force. Rows include typed IDs, holder, scope, named rights, parent, cap, effective remaining allowance including the parent's cap, period seconds and remaining expiry seconds; non-financial cap values are null. Holder revocation affects grants issued by the caller, while a specific grant can also be revoked by an actual architect.

Preset right fields are arrays of names and replace only that field. A group assignment is `"g:N"` or null to clear it; assigning a group requires the caller's current membership. Goods right names are use/take/move/edit/copy/transfer/destroy; plot presets accept only build/enter/run_scripts. An inherited ceiling can be narrowed for the next owner but cannot be widened, including by clearing no-copy after inherited Copy was lost. Group member input uses `{"group":"g:N","player":"p:N","member":true|false}`. Plot IDs are positive integers; bans are arrays of `p:N`, normalized to a sorted unique list after validating every player. Omitted fields preserve their current values, and all fields are checked before any preset/plot mutation.

## Exact access boundaries

Shared parsing comes from `bindings/authority/descriptors`: `ScopeRead` contains only const identity, authority, owner limits, players, plots and profiles. It parses account/scope/rights/IDs/durations without mutation. A separate holder decoder receives only model/default-script, explicit installations, script declarations and object authority needed to prove actual installation/controller. No shared administration context contains mutable subsystems.

| Operation context | Mutable data | Read projections |
| --- | --- | --- |
| Payment | Ledger, Grants | Identity, authority, players |
| Inventory/balance/offer reads | None | Exact respective players/ledger/offers and identity/player definitions |
| Give | Players, Grants, authority, permission flags, owner limits, physical custody | Current target/part identity, delivery bodies |
| Offer/cancel | Offers, permission flags, physical custody | Target/parts, players, grants, authority/limits |
| Accept | Offers, Ledger, Grants, Players, authority, permission flags, limits, physical custody | Current target/parts, player wallet IDs, delivery bodies |
| Trade open | Trades | Players, delivery bodies |
| Trade change/cancel | Trades, Offers, permission flags, physical custody | Participants, current goods/parts, authority/grants/limits as needed |
| Trade confirm | Trades, Offers, Ledger, Grants, Players, authority, permission flags, limits, physical custody | Current complete goods and delivery bodies |
| Grant/revoke | Grants, Players | Exact scope/holder definitions; profiles for architect role |
| Preset | Group/preset columns, permission flags | Current owner/limits, players/groups |
| Existing placement | Space, poses/parent/space flags, physical relations, Players inventory/mailbox | Actual imported geometry/catalog bindings, measured mass/material, current identity/authority, plots/grants, named parts |
| Groups | Groups, Players membership | Authenticated actor |
| Plot settings | Plots | Actor, players/groups |

Physical custody is one narrow mechanical context: Space, identity, transforms/parents, space flags, four physical relations, declared parts, relation-change outputs and a reference to the fixed `SceneProjection` owner. It has no ledger, grants, text, VM or catalog. Existing-item reveal has its separate geometry-bound context; ordinary hiding/exchange does not need geometry import tables.

## Commit order and scene truth

Copy complete declared-part and current target descriptors before mutation. Inventory/mailbox stores only roots; ordinary attached occupants are not parts and survive collection. Reject stale parts, escrow members and mismatched owners through the existing permission preflight.

Before a valuable operation can mutate money, grants or owners, preflight every resulting physical custody change. `space::can_custody` shares the exact live/cycle predicates of `custody`; ownership/ledger mutations cannot change those predicates in the non-yielding single writer. Thus successful permission commit is followed immediately by a mechanically non-failing custody mirror. No rollback, reservation ticket or transaction framework is introduced. Hidden deliveries remove actual query-visible geometry. Placed escrow detach preserves actual world poses and scene membership; cancellation restores the side's recorded ordinary custody. Physical relation drafts enter the current operation effects. Actual hide/reveal calls the fixed `scene_visible` operation immediately, so light/zone/process/replication registrations cannot be omitted by a caller.

Existing placement is the reverse administration operation: prepare current binding's real geometry/logical roots/volumes and rigidly transform the whole declared assembly from old root pose to requested pose. Preflight land permission and native geometry capacity, then restore/rebuild the actual scene, clear inventory custody and remove root from inventory/mailbox. Preserve independent authored part motion, inherited limits, no-copy and all identity/attribution fields. Placement into the scene does not allocate new object identities or repeat the construction material/energy debit.

Return denied results with actual reason/needed right; successful native completion carries explicit custody, revoked grants and relevant IDs in its reply. Its completed action and follow-up effects use the ordinary Journal origin. Scheduled acceptance and grant checks reject expired rights immediately through current time, even before bounded deadline cleanup. Native wrappers do not claim failed valuable operations applied.

## Validation

Test through the actual native registry and real Jolt scene, rather than fake custody callbacks. Cover capped delegated payment and failed balance/cap counter preservation; placed no-copy assembly sale, owner/grant clearing and actual geometry hiding; offline mailbox bounce; direct trade confirmation reset and atomic money/goods exchange; cancellation of placed and inventory sides; absent/foreign/expired grants; inherited preset restrictions; group membership and land bans; and placing accepted inventory back into actual ray queries with unchanged full root/part identities. Capacity or permission failure must leave money, ownership, custody and inventory unchanged. Full live-world tests remain distinct from these integration checks.

Existing placement accepts ordinary installed-script holders under their actual Move and Build rights; it never borrows the inventory owner’s grants. After a plot change it reconciles the moved contributor’s priority dependency scopes even when the effective numeric priority is unchanged. Structural model rebinding uses the separate `RebindPriorities` adapter for the same reason.


## Login mailbox delivery

`MailboxDelivery { Players&, AuthorityRead, Custody& }` and `deliver_mailbox(binding, player, authenticatedBody, step, Origin, DeferredEffects&) -> size_t` implement S9.4's mailbox-to-inventory transfer at successful authenticated login. Composition updates current presence and calls it on the writer. The current complete body handle must be live, mechanically tagged Body and owned by that player. Every mailbox item receives full declared-closure custody preflight before any mailbox/inventory changes. The existing permission helper then transfers the rows once; actual hidden Space custody and scene projection move the goods under the body without changing object identity, maker, model, permissions or owner.

Hidden inventory delivery allocates no native bodies, so a full Jolt pool still admits it. A failed authority/full-handle/custody preflight leaves the whole mailbox and physical parents unchanged. Empty/repeated login is a no-op. A completed `mailbox_delivery` System event and relation effects are appended to the caller's deferred outputs; composition reconciles/publishes them with the supplied login cause.

## Model-definition changes invalidate quotes

A sale or bilateral trade quotes a complete part graph. `preflight_rebound_offers(void const*, span<const Handle> affected, span<const Handle> removed, vector<space::VisibleCustody>&, string& error)` checks all recorded prior custody without mutation and returns the future visible parent/grasp descriptors. Native model-revision preflight validates retained parents, new hulls and aggregate capacity for those prospective grasps before authorizing the edit. After all structural/resource/lifetime preflights, `rebind_offers(void*, span<const Handle> affected, InvocationInput const&, DeferredEffects&)` uses `RebindOffersBinding { Offers&, Trades&, PermissionFlags, Custody& }` to cancel affected quotes and both confirmations **before extraction**. Extraction then preserves the restored actual custody through geometry replacement. This ordering also ensures removed quoted parts invalidate their sale before any slot is freed, without applying custody to an extracted root. Unrelated quotes, money and ownership remain unchanged. The generic `perm::invalidate_goods_offers` is shared with ordinary pre-retirement cleanup. The completed System record carries reason `definition_changed` and the parent invocation cause.

Real Jolt regressions cover a visible sale with a newly authored part, an unrelated still-live sale, idempotent completion, and a bilateral visible-grasp/hidden-inventory trade cancelled at zero remaining native-body capacity. The latter preserves both prior scene memberships, exact poses, inventory, wallets and live identities while clearing confirmations and escrow.


## Deadline maintenance

`Grants` and `Offers` own derived deadline indexes, rebuilt from authoritative rows during restore. Ordinary offer creation indexes its finite expiry once; accepted, cancelled, invalidated and trade rows are discarded when their stale token comes due. `perm::advance_offers(offers, identity, flags, now, max_entries)` consumes at most that many due entries, including stale tokens, and clears only Escrow on valid full root/part handles. It does not change custody, geometry, ownership, money or other flags. Excess due entries remain queued for later steps.

`commerce::advance_offer_expiry(OfferExpiryBinding&, now, max_entries)` returns completed `{offer,seller,goods,cleared}` values. Main runs it before ordinary inputs, invalidates the affected projections, notifies the actual seller, and writes one nonchosen System record. The binding receives only Offers, IdentityRead and PermissionFlags. It needs no physics or custody context because a placed sale remains at its actual pose throughout expiry.

The shared `permissions.DEADLINES_PER_STEP` work quota bounds grant and offer maintenance independently. Current-time permission and acceptance checks remain exact at the deadline. Under a very large simultaneous expiry backlog, housekeeping and notifications may arrive later. The quota does not discard pending work.

`tests/game_deadlines.py` is the separate live acceptance driver: it starts an owned native world, exercises a real sale and temporary wallet grant through authenticated inputs, checks warning/expiry pushes and exact S7 choice/kind/ID metadata, and verifies that released goods can be offered again. It also submits actual CAD and checks an ordinary newly built disconnected platform's navigation notification. Its results are reported separately from the module tests and benchmarks.
