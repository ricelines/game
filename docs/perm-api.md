# Permissions, player wallets and goods swaps

The native `server/perm` library implements S9 and Technicals §§13–14. It links store, snapshot codecs and the standard library. Input/VM adapters receive the exact projections and tables for their operation; the permission library does not simulate movement, execute scripts or access a world aggregate.

## Tables and identities

`Holder` is a player ID or `(script ID, full installed-object handle)`. Script permission never inherits its author's or installed object's owner's groups or wallet authority.

* `Players` contains sorted group memberships, grants held/given, inventory and mailbox. Session profiles remain separate.
* `Groups` contains group ID, controller and descriptive name; membership is on players.
* `Grants` contains monotonic grant rows plus the `(holder, scope)` lookup. Revocation maintains both player lists and removes delegated children.
* `Ledger::balance` is a vector of integer currency balances indexed by `PlayerId`, with reserved zero row. Its length matches `Players::rows`; each player has one wallet. Objects and groups have no balances. An object-handle Lua selector resolves only to its current owner's player wallet and supplies no Spend authority.
* `Offers` and `Trades` contain monotonic sale rows and bilateral confirmation rows.
* `OwnerLimits` is a sparse inherited Edit/Copy/Transfer ceiling. Missing means those three rights are allowed. Creation starts with full owner rights. A transfer intersects the previous ceiling with the next-owner bits. Everyone/group presets and future grants cannot widen that ceiling. Owners retain Use/Take/Move/Destroy, so unwanted goods remain removable. NoCopy independently prevents duplication while allowing transfer of the sole instance.

The ceiling resolves the conflict between S9's unconditional owner shortcut and Technicals' next-owner restrictions. Its owner move/delete behavior follows the [design's Second Life permission reference](https://wiki.secondlife.com/wiki/Permission). No separate first-sale exception is added.

## Exact financial API

```cpp
Decision check_account(Holder, PlayerId wallet, int64_t amount,
                       const Grants&, Step);
int64_t balance(const Ledger&, PlayerId);
TransferResult pay(Ledger&, Grants&, Holder, PlayerId payer,
                   PlayerId recipient, int64_t amount, Step);
TransferResult issue(Ledger&, Grants&, Holder, PlayerId recipient,
                     int64_t amount, Step);
Scope Scope::account(PlayerId);
```

Players can spend their own wallets. Other players and installed scripts require an explicit Spend grant on that player's wallet. A script's object ownership, installation or wallet read does not authorize payment. Grant and parent caps are checked without mutation; both counters commit only after balance/overflow checks pass. Unknown wallets do not create ledger rows. Failed payments preserve both balances and counters.

Issuance uses a separate architect-provisioned, listable, capped, period-bound and expiring grant. Owning an object or wallet cannot mint money. The authored issuer supplies policy, timing and recipients.

## Permission operations

Adapters resolve a live `Target` from identity/authority/owner-limit read projections. It contains only handle, owner, group, preset, plot, flags and effective ceiling. `check` tests owner, everyone, group binary search, direct grant and owned-on-plot grant. A script has implicit Move/Edit on its installed self; Transfer/Destroy/Spend need grants. Land checks include bans, Build/Enter/RunScripts and explicitly granted Transfer.

`create_grant` takes current `ScopeControl` and `HolderControl` descriptors, derived by an authorized adapter rather than the remote caller. Financial scopes and script holders require future expiry. One-level delegation may narrow verbs/cap/expiry only to the issuer's own installed script. Children share the original parent's spending cap; they cannot redelegate. Checking an expired grant rejects it before cleanup runs. Revoke requires issuer or architect authorization at its input adapter. Grant IDs are not reused.

## Goods and custody

`offer` checks Transfer on placed goods and their complete declared-part closure, sets escrow on every member and retains the current owner as seller. Sale income always enters that seller's player wallet. There is no caller-selected proceeds destination. `Buyer` supplies actual actor, beneficiary player, payer player, current custodian and presence; a script buyer still needs an explicit payer-wallet Spend grant.

Acceptance rechecks current ownership, complete assembly membership, inherited ceilings, escrow, expiry, designated buyer, delivery capacity and payment before any valuable mutation. Successful acceptance changes payment, ownership, grants and inventory together. Space custody/capacity is preflighted by the adapter before calling perm, then mirrored in the same input without yielding. No fallible geometry job follows the financial commit.

An assembly is its root plus declared parts, not arbitrary spatial parents. Root Transfer authority covers that item; every part must retain compatible ownership and inherited transfer permission. Sale/gift applies each member's own next-owner ceiling and clears grants scoped to any member or held by its installed scripts. Installed behavior survives. Inventory/mailbox contains only the root. No-copy goods transfer without duplication.

Direct trade requires both players present. Editing either side resets both confirmations. The second confirmation rechecks both complete sides and balances before atomically assigning final balances and transferring goods; a trade side cannot be accepted as an ordinary offer. Cancelling restores recorded prior custody. Retiring any escrow member cancels affected offers/trades before slot reuse and returns all released custody deltas for the physical owner to apply.

Giving to an absent player checks mailbox capacity before ownership changes; overflow retains the sender's item. Login moves mailbox roots into inventory and returns explicit custody deltas. Destruction revokes object/installed-script grants and removes inventory references; there is no object financial state to settle.

## Acceptance checks

Real-table tests cover owner/group/everyone checks; full script-installation identity; ceilings/NoCopy; one-level delegation/shared caps; failed-payment atomicity; wallet overflow and invalid IDs; capped issuance/revocation; expiry; direct/vendor sales into the seller wallet; bilateral confirmation reset; offline bounce; whole-assembly transfer; retirement/reuse; and snapshot continuation. Hot checks perform fixed lookups plus at most one parent, with no world scan. Benchmarks measure the actual denied, direct and owned-on-plot paths.
