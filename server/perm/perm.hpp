#pragma once
namespace amber::perm {
struct State;
}

#include "snap/snap.hpp"
#include "store/store.hpp"

#include <array>
#include <compare>
#include <map>
#include <nlohmann/json.hpp>
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
struct AccountTarget {
  Account account;
  PlayerId controller = 0;
  static AccountTarget player(PlayerId id) { return {Account::player(id), id}; }
};
enum class ScopeKind : std::uint8_t { Object, OwnedOnPlot, Account, Avatar, Plot, Owned, AllLand };
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
  static Scope owned(PlayerId owner) { return {ScopeKind::Owned, 0, owner}; }
  static Scope all_land() { return {ScopeKind::AllLand, 0, 0}; }
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
  std::vector<nlohmann::json> notices;
  std::uint32_t notices_dropped = 0;
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
  bool on_lot = false;
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
enum class LandVerb : std::uint8_t { Build, Enter, RunScripts, Transfer, Return };
enum class PlotKind : std::uint8_t { Street, Alley, Lot };
struct Plot {
  PlotId id = 0;
  PlayerId owner = 0;
  GroupId group = 0;
  Rights preset = 0;
  std::array<float, 4> bounds{}; // min x, min y, max x, max y
  std::vector<PlayerId> banned;  // sorted
  PlotKind kind = PlotKind::Lot;
};
struct Plots {
  std::vector<Plot> rows{Plot{}};
};
struct Decision {
  Reason reason = Reason::MissingRight;
  GrantId grant = 0;
  explicit operator bool() const { return reason == Reason::Allowed; }
};
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
  bool on_lot = false;
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
struct GrantDeadlines {
  std::vector<GrantId> reminders, expired;
};
struct TransferResult {
  Reason reason = Reason::BadAmount;
  std::int64_t amount = 0;
  explicit operator bool() const { return reason == Reason::Allowed; }
};
struct AccountRefund {
  Handle object;
  PlayerId owner = 0;
  std::int64_t amount = 0;
};
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
using TradeId = std::uint32_t;
struct TradeSide {
  OfferId offer = 0;
  std::int64_t money = 0;
  bool confirmed = false;
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
struct SnapshotSelection {
  snap::ObjectSelection objects{std::span<const Handle>{}};
  std::unordered_set<PlayerId> players;
  std::unordered_set<GroupId> groups;
  std::unordered_set<PlotId> plots;
  std::unordered_set<GrantId> grants;
  std::unordered_set<OfferId> offers;
  std::unordered_set<TradeId> trades;
};

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
Target target(IdentityRead, AuthorityRead, const OwnerLimits &, const Plots &, Handle);
Decision check(Holder, Verb, const Target &, const Players &, const Grants &, Step);
Decision check_scope(Holder, Scope, Rights required, const Grants &, Step);
Decision check_account(Holder, AccountTarget, std::int64_t amount, const Grants &, Step);
std::int64_t balance(const Ledger &, Account);
std::int64_t allowance(const Grants &, GrantId, Step);

GrantResult create_grant(Grants &, Players &, Holder issuer, const GrantRequest &,
                         const ScopeControl &, const HolderControl &, Step);
GrantResult create_root_grant(Grants &, Players &, const GrantRequest &, const HolderControl &,
                              Step);
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
