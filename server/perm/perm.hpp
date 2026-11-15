#pragma once

#include "store/store.hpp"

#include <array>
#include <compare>
#include <map>
#include <set>
#include <span>
#include <unordered_map>

namespace amber::perm {

enum class Verb : std::uint8_t { Use, Take, Move, Edit, Copy, Transfer, Destroy };
using Rights = std::uint32_t;
constexpr Rights bit(Verb verb) {
  return Rights{1} << static_cast<unsigned>(verb);
}
inline constexpr Rights all_rights = (Rights{1} << 7) - 1;
inline constexpr Rights inherited_rights = bit(Verb::Edit) | bit(Verb::Copy) | bit(Verb::Transfer);

enum class HolderKind : std::uint8_t { Player, Script };
struct Holder {
  HolderKind kind = HolderKind::Player;
  std::uint32_t id = 0;
  Handle installed_on;
  static Holder player(PlayerId id) { return {HolderKind::Player, id, {}}; }
  static Holder script(ScriptId id, Handle object) { return {HolderKind::Script, id, object}; }
  bool operator==(const Holder &) const = default;
};

enum class AccountKind : std::uint8_t { Player, Object, Group };
struct Account {
  AccountKind kind = AccountKind::Player;
  std::uint64_t subject = 0;
  static Account player(PlayerId id) { return {AccountKind::Player, id}; }
  static Account object(Handle object) { return {AccountKind::Object, object.bits}; }
  static Account group(GroupId id) { return {AccountKind::Group, id}; }
  auto operator<=>(const Account &) const = default;
};
std::string account_name(Account);
struct AccountTarget {
  Account account;
  PlayerId controller = 0;
  static AccountTarget player(PlayerId id) { return {Account::player(id), id}; }
};

enum class ScopeKind : std::uint8_t { Object, OwnedOnPlot, Account, Avatar, Plot, Issuance };
struct Scope {
  ScopeKind kind = ScopeKind::Object;
  std::uint64_t subject = 0;
  PlayerId owner = 0;
  AccountKind account_kind = AccountKind::Player;
  static Scope object(Handle handle) { return {ScopeKind::Object, handle.bits, 0}; }
  static Scope owned_on_plot(PlayerId owner, PlotId plot) {
    return {ScopeKind::OwnedOnPlot, plot, owner};
  }
  static Scope account(Account account) {
    return {ScopeKind::Account, account.subject, 0, account.kind};
  }
  Account account() const { return {account_kind, subject}; }
  static Scope avatar(Handle body) { return {ScopeKind::Avatar, body.bits, 0}; }
  static Scope plot(PlotId id) { return {ScopeKind::Plot, id, 0}; }
  static Scope issuance() { return {ScopeKind::Issuance, 0, 0}; }
  bool operator==(const Scope &) const = default;
};

struct GrantKey {
  Holder holder;
  Scope scope;
  bool operator==(const GrantKey &) const = default;
};
struct GrantKeyHash {
  std::size_t operator()(const GrantKey &) const;
};

struct Player {
  std::vector<GroupId> groups;
  std::vector<GrantId> grants_held, grants_given;
  std::vector<Handle> inventory, mailbox;
};
struct Players {
  std::vector<Player> rows{Player{}};
};
struct Group {
  GroupId id = 0;
  PlayerId controller = 0;
  std::string name;
};
struct Groups {
  std::vector<Group> rows{Group{}};
};
struct Grant {
  GrantId id = 0;
  PlayerId issuer = 0;
  GrantId parent = 0;
  Holder holder;
  Scope scope;
  Rights verbs = 0;
  std::int64_t cap = 0, spent = 0;
  Step period_steps = 0, period_start = 0, expires_step = 0;
  bool live = false;
};
struct Grants {
  std::vector<Grant> rows{Grant{}};
  std::unordered_map<GrantKey, GrantId, GrantKeyHash> by_key;
  // Each live deadline has one ordered (step,id) entry, erased on removal.
  std::set<std::pair<Step, GrantId>> expiring, reminders;
};
struct Ledger {
  std::map<Account, std::int64_t> balance;
};
using OwnerLimits = SparseColumn<Rights>;

struct Target {
  Handle object;
  PlayerId owner = 0;
  GroupId group = 0;
  Rights preset = 0;
  PlotId plot = 0;
  std::uint32_t flags = 0;
  Rights owner_limit = inherited_rights;
};

enum class Reason {
  Allowed,
  UnknownHolder,
  MissingRight,
  InheritedLimit,
  NoCopy,
  Expired,
  Allowance,
  InsufficientBalance,
  BadAmount,
  BalanceOverflow,
  Duplicate,
  InvalidDelegation,
  ExpiryRequired,
  InvalidScope,
  NotController,
  Banned,
  NotGoods,
  NotPlaced,
  InEscrow,
  OfferGone,
  WrongBuyer,
  MailboxFull,
  StaleGoods,
  NotParticipant,
  NotPresent
};
std::string_view reason_text(Reason);

enum class LandVerb : std::uint8_t { Build, Enter, RunScripts, Transfer = 5 };
struct Plot {
  PlotId id = 0;
  PlayerId owner = 0;
  GroupId group = 0;
  Rights preset = 0;
  std::array<float, 4> bounds{}; // min x, min y, max x, max y
  std::vector<PlayerId> banned;  // sorted
};
struct Plots {
  std::vector<Plot> rows{Plot{}};
};
// Refresh only changed scene objects. Hidden inventory and mailboxes have no plot.
std::vector<Handle> update_plots(IdentityRead, TransformRead, PlotWrite, const Plots &,
                                 std::span<const Handle>, bool in_world);
struct Decision {
  Reason reason = Reason::MissingRight;
  GrantId grant = 0;
  explicit operator bool() const { return reason == Reason::Allowed; }
};
Decision check_land(Holder, LandVerb, const Plot &, const Players &, const Grants &, Step);

// Targets are resolved from live projections before calling these operations.
Target target(IdentityRead, AuthorityRead, const OwnerLimits &, Handle);
Decision check(Holder, Verb, const Target &, const Players &, const Grants &, Step);
Decision check_scope(Holder, Scope, Rights required, const Grants &, Step);
Decision check_account(Holder, AccountTarget, std::int64_t amount, const Grants &, Step);
std::int64_t balance(const Ledger &, Account);
std::int64_t allowance(const Grants &, GrantId, Step);

struct GrantRequest {
  PlayerId issuer = 0;
  Holder holder;
  Scope scope;
  Rights verbs = 0;
  std::int64_t cap = 0;
  Step period_steps = 0, expires_step = 0;
};
struct ScopeControl {
  Scope scope;
  PlayerId controller = 0;
  Rights available = 0;
  PlotId plot = 0;
  bool issuer_is_architect = false;
};
struct HolderControl {
  Holder holder;
  PlayerId controller = 0;
  bool installed = false;
};
struct GrantResult {
  Reason reason = Reason::InvalidScope;
  GrantId grant = 0;
  explicit operator bool() const { return reason == Reason::Allowed; }
};

GrantResult create_grant(Grants &, Players &, Holder issuer, const GrantRequest &,
                         const ScopeControl &, const HolderControl &, Step);
// Input bindings authorize issuer/architect before invoking mutation helpers.
std::vector<GrantId> revoke(Grants &, Players &, GrantId);
std::vector<GrantId> revoke_many(Grants &, Players &, std::span<const GrantId>);
std::vector<GrantId> revoke_object(Grants &, Players &, Handle);
std::vector<GrantId> revoke_objects(Grants &, Players &, std::span<const Handle>);
struct PlotTransferResult {
  Reason reason = Reason::MissingRight;
  PlayerId previous_owner = 0;
  std::vector<GrantId> revoked;
  explicit operator bool() const { return reason == Reason::Allowed; }
};
PlotTransferResult transfer_plot(Plots &, Grants &, Players &, Holder, PlotId, PlayerId recipient,
                                 Step);
struct GrantDeadlines {
  std::vector<GrantId> reminders, expired;
};
GrantDeadlines advance_grants(Grants &, Players &, Step now, Step reminder_steps,
                              std::size_t max_entries);
void rebuild_grant_lookup(Grants &);

struct TransferResult {
  Reason reason = Reason::BadAmount;
  std::int64_t amount = 0;
  explicit operator bool() const { return reason == Reason::Allowed; }
};
TransferResult pay(Ledger &, Grants &, Holder, AccountTarget payer, Account recipient,
                   std::int64_t amount, Step);
TransferResult issue(Ledger &, Grants &, Holder, Account recipient, std::int64_t amount, Step);
struct AccountRefund {
  Handle object;
  PlayerId owner = 0;
  std::int64_t amount = 0;
};
Reason can_refund_object_accounts(const Ledger &, AuthorityRead, std::span<const Handle>);
std::vector<AccountRefund> refund_object_accounts(Ledger &, AuthorityRead, std::span<const Handle>);

// Changes only authority/permission-owned columns; custody is mirrored separately.
std::vector<GrantId> transfer_owner(AuthorityWrite, OwnerLimits &, PermissionFlags, Grants &,
                                    Players &, const Target &, PlayerId recipient);

using OfferId = std::uint32_t;
struct Offer {
  OfferId id = 0;
  PlayerId seller = 0, buyer = 0;
  Handle goods;
  std::int64_t price = 0;
  Step expires_step = 0;
  std::uint32_t trade = 0;
  bool live = false;
  std::vector<Handle> parts;
};
struct Offers {
  std::vector<Offer> rows{Offer{}};
  std::set<std::pair<Step, OfferId>> expiring;
};
struct CustodyChange {
  Handle goods, parent;
  bool in_world = false;
};
struct OfferResult {
  Reason reason = Reason::OfferGone;
  OfferId offer = 0;
  std::vector<CustodyChange> custody;
  explicit operator bool() const { return reason == Reason::Allowed; }
};
OfferResult offer(Offers &, PermissionFlags, Holder, const Target &, const Players &,
                  const Grants &, std::int64_t price, PlayerId buyer, Step expiry, bool placed,
                  Step now, std::span<const Target> parts = {});
OfferResult cancel_offer(Offers &, PermissionFlags, Holder, const Target &, const Players &,
                         const Grants &, OfferId, Step, std::span<const Target> parts = {});
// Deadline lookup is derived, rebuilt once from authoritative rows on restore.
void rebuild_offer_deadlines(Offers &);
std::vector<OfferId> advance_offers(Offers &, IdentityRead, PermissionFlags, Step now,
                                    std::size_t max_entries);

// Authenticated adapters supply explicit financial and delivery descriptors.
struct Buyer {
  Holder actor;
  PlayerId beneficiary = 0;
  AccountTarget payer;
  Handle custodian;
  bool present = false;
};
struct SwapResult {
  Reason reason = Reason::OfferGone;
  std::vector<CustodyChange> custody;
  std::vector<GrantId> revoked;
  explicit operator bool() const { return reason == Reason::Allowed; }
};
SwapResult accept(Offers &, Ledger &, Grants &, Players &, AuthorityWrite, PermissionFlags,
                  OwnerLimits &, const Buyer &, const Target &goods, OfferId,
                  std::size_t mailbox_max, Step, std::span<const Target> parts = {});
SwapResult give(Players &, Grants &, AuthorityWrite, PermissionFlags, OwnerLimits &, Holder sender,
                const Target &, PlayerId recipient, Handle recipient_body, bool present,
                std::size_t mailbox_max, Step, std::span<const Target> parts = {});
SwapResult store_goods(Players &, const Grants &, Holder, const Target &,
                       std::span<const Target> parts, PlayerId recipient, Handle recipient_body,
                       bool present, std::size_t mailbox_max, Step);
std::vector<CustodyChange> deliver_mailbox(Players &, PlayerId, Handle body);

using TradeId = std::uint32_t;
struct TradeSide {
  OfferId offer = 0;
  std::int64_t money = 0;
  bool confirmed = false;
  CustodyChange prior_custody;
};
struct Trade {
  TradeId id = 0;
  std::array<PlayerId, 2> players{};
  std::array<TradeSide, 2> sides{};
  bool live = true;
};
struct Trades {
  std::vector<Trade> rows{Trade{}};
};
struct TradeResult {
  Reason reason = Reason::OfferGone;
  TradeId trade = 0;
  bool completed = false;
  std::vector<CustodyChange> custody;
  std::vector<GrantId> revoked;
  explicit operator bool() const { return reason == Reason::Allowed; }
};
TradeResult open_trade(Trades &, const Players &, PlayerId, PlayerId);
// current_goods is null for a money-only side. Prior custody comes from space's current mirror.
TradeResult change_trade(Trades &, Offers &, PermissionFlags, const Players &, const Grants &,
                         TradeId, PlayerId, const Target *current_goods, std::int64_t money,
                         CustodyChange prior_custody, Step, std::span<const Target> parts = {});
// The adapter resolves both current goods and both present bodies before each confirmation.
TradeResult confirm_trade(Trades &, Offers &, Ledger &, Grants &, Players &, AuthorityWrite,
                          PermissionFlags, OwnerLimits &, TradeId, PlayerId,
                          const std::array<Target, 2> &, const std::array<Handle, 2> &bodies, Step,
                          const std::array<std::span<const Target>, 2> &parts = {});
TradeResult cancel_trade(Trades &, Offers &, PermissionFlags, TradeId, PlayerId);
// Invalidates quotations touching a full goods/part handle and restores prior custody.
// Call before lifetime retirement or after a successful definition rebind.
std::vector<CustodyChange> invalidate_goods_offers(Offers &, Trades &, IdentityRead,
                                                   PermissionFlags, Handle);

} // namespace amber::perm
