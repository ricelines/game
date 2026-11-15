#pragma once
namespace amber::perm {
struct State;
}

#include "snap/snap.hpp"
#include "store/store.hpp"

#include <array>
#include <compare>
#include <map>
#include <set>
#include <span>
#include <unordered_map>

namespace amber::perm {

struct Gate {
  bool allowed = false;
  std::string reason;
};
std::string_view required_right(std::string_view, Caller, const Target &);
Gate check_right(std::string_view, Caller, const Target &, const Players &, const Grants &, Step);

std::string account_name(Account);

std::string_view reason_text(Reason);

// Refresh only changed scene objects. Hidden inventory and mailboxes have no plot.
std::vector<Handle> update_plots(IdentityRead, TransformRead, PlotWrite, const Plots &,
                                 std::span<const Handle>, bool in_world);

Decision check_land(Holder, LandVerb, const Plot &, const Players &, const Grants &, Step);

// Targets are resolved from live projections before calling these operations.
Target target(IdentityRead, AuthorityRead, const OwnerLimits &, Handle);
Decision check(Holder, Verb, const Target &, const Players &, const Grants &, Step);
Decision check_scope(Holder, Scope, Rights required, const Grants &, Step);
Decision check_account(Holder, AccountTarget, std::int64_t amount, const Grants &, Step);
std::int64_t balance(const Ledger &, Account);
std::int64_t allowance(const Grants &, GrantId, Step);

GrantResult create_grant(Grants &, Players &, Holder issuer, const GrantRequest &,
                         const ScopeControl &, const HolderControl &, Step);
// Input bindings authorize issuer/architect before invoking mutation helpers.
std::vector<GrantId> revoke(Grants &, Players &, GrantId);
std::vector<GrantId> revoke_many(Grants &, Players &, std::span<const GrantId>);
std::vector<GrantId> revoke_object(Grants &, Players &, Handle);
std::vector<GrantId> revoke_objects(Grants &, Players &, std::span<const Handle>);

GrantDeadlines advance_grants(Grants &, Players &, Step now, Step reminder_steps,
                              std::size_t max_entries);
void rebuild_grant_lookup(Grants &);

TransferResult pay(Ledger &, Grants &, Holder, AccountTarget payer, Account recipient,
                   std::int64_t amount, Step);
TransferResult issue(Ledger &, Grants &, Holder, Account recipient, std::int64_t amount, Step);

Reason can_refund_object_accounts(const Ledger &, AuthorityRead, std::span<const Handle>);
std::vector<AccountRefund> refund_object_accounts(Ledger &, AuthorityRead, std::span<const Handle>);

// Changes only authority/permission-owned columns; custody is mirrored separately.
std::vector<GrantId> transfer_owner(AuthorityWrite, OwnerLimits &, PermissionFlags, Grants &,
                                    Players &, const Target &, PlayerId recipient);

OfferResult offer(amber::perm::State &perm_state, PermissionFlags, Holder, const Target &,
                  std::int64_t price, PlayerId buyer, Step expiry, bool placed, Step now,
                  std::span<const Target> parts = {});
OfferResult cancel_offer(amber::perm::State &perm_state, PermissionFlags, Holder, const Target &,
                         OfferId, Step, std::span<const Target> parts = {});
// Deadline lookup is derived, rebuilt once from authoritative rows on restore.
void rebuild_offer_deadlines(Offers &);
std::vector<OfferId> advance_offers(Offers &, IdentityRead, PermissionFlags, Step now,
                                    std::size_t max_entries);

// Authenticated adapters supply explicit financial and delivery descriptors.

SwapResult accept(amber::perm::State &perm_state, amber::ObjectState &objects_state, const Buyer &,
                  const Target &goods, OfferId, std::size_t mailbox_max, Step,
                  std::span<const Target> parts = {});
SwapResult give(amber::perm::State &perm_state, amber::ObjectState &objects_state, Holder sender,
                const Target &, PlayerId recipient, Handle recipient_body, bool present,
                std::size_t mailbox_max, Step, std::span<const Target> parts = {});
SwapResult store_goods(amber::perm::State &perm_state, Holder, const Target &,
                       std::span<const Target> parts, PlayerId recipient, Handle recipient_body,
                       bool present, std::size_t mailbox_max, Step);
std::vector<CustodyChange> deliver_mailbox(Players &, PlayerId, Handle body);

TradeResult open_trade(Trades &, const Players &, PlayerId, PlayerId);
// current_goods is null for a money-only side; escrow preserves current visibility.
TradeResult change_trade(amber::perm::State &perm_state, PermissionFlags, TradeId, PlayerId,
                         const Target *current_goods, std::int64_t money, bool in_world, Step,
                         std::span<const Target> parts = {});
// The adapter resolves both current goods and both present bodies before each confirmation.
TradeResult confirm_trade(amber::perm::State &perm_state, amber::ObjectState &objects_state,
                          TradeId, PlayerId, const std::array<Target, 2> &,
                          const std::array<Handle, 2> &bodies, Step,
                          const std::array<std::span<const Target>, 2> &parts = {});
TradeResult cancel_trade(Trades &, Offers &, PermissionFlags, TradeId, PlayerId);
// Invalidates quotations touching a full goods/part handle; goods stay where they are.
// Call before lifetime retirement or after a successful definition rebind.
std::vector<Handle> invalidate_goods_offers(Offers &, Trades &, IdentityRead, PermissionFlags,
                                            Handle);

} // namespace amber::perm

namespace amber::perm {

SnapshotSelection capture_authority(AuthorityRead, std::span<const Handle>,
                                    std::span<const PlayerId> actors,
                                    const amber::perm::State &perm_state);
void write_players(snap::Output &, const Players &, const Ledger &, const Plots &, const Groups &,
                   const SnapshotSelection * = nullptr);
void read_players(snap::Input &, Players &, Ledger &, Plots &, Groups &);
void write_grants(snap::Output &, const Grants &, const Offers &, const Trades &,
                  const OwnerLimits &, const SnapshotSelection * = nullptr);
void read_grants(snap::Input &, Grants &, Offers &, Trades &, OwnerLimits &,
                 std::size_t object_slots);

} // namespace amber::perm

namespace amber::perm {
struct State {
  Players players;
  Groups groups;
  Plots plots;
  Grants grants;
  Ledger ledger;
  OwnerLimits owner_limits;
  Offers offers;
  Trades trades;
};
} // namespace amber::perm
