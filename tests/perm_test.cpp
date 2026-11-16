#include "perm/perm.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace amber;
using namespace amber::perm;

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct Fixture {
  amber::perm::State perm_state;

  Players &players = perm_state.players;
  Grants &grants = perm_state.grants;
  Ledger &ledger = perm_state.ledger;
  Fixture() { players.rows.resize(8); }
  GrantId grant(PlayerId issuer, Holder holder, Scope scope, PlayerId controller, Rights rights,
                std::int64_t cap = 0, Step period = 0, Step expiry = 1000,
                PlayerId holder_controller = 0) {
    const auto result =
        create_grant(grants, players, perm::Holder::player(issuer),
                     {issuer, holder, scope, rights, cap, period, expiry},
                     {scope, controller, all_rights}, {holder, holder_controller, true}, 1);
    require(static_cast<bool>(result), "fixture grant creation failed");
    return result.grant;
  }
};

void moved_goods_leave_plot_scoped_authority() {
  Fixture f;
  Objects objects;
  ObjectInit init;
  init.model = 1;
  init.owner = 1;
  init.position = {-1, 0, 0};
  const auto item = create(objects, init);
  OwnerLimits limits;
  Plots plots;
  plots.rows.push_back({1, 1, 0, 0, {-5, -5, 0, 5}, {}});
  plots.rows.push_back({2, 2, 0, 0, {0, -5, 5, 5}, {}});
  const std::array selected{item, Handle::make(item.slot(), item.generation() + 1)};
  auto refresh = [&](bool visible) {
    return update_plots(identity(objects), transforms(objects), plot_membership(objects), plots,
                        selected, visible);
  };
  require(refresh(true) == std::vector<Handle>{item}, "plot refresh accepted stale generation");
  const auto script = Holder::script(7, Handle::make(30, 1));
  f.grant(1, script, Scope::owned_on_plot(1, 1), 1, bit(Verb::Move));
  auto allowed = [&] {
    return static_cast<bool>(check(script, Verb::Move,
                                   target(identity(objects), authority(objects), limits, item),
                                   f.players, f.grants, 2));
  };
  require(allowed(), "on-plot grant did not cover initial owned goods");
  objects.position[item.slot()].x = 0;
  require(refresh(true) == std::vector<Handle>{item} && objects.plot[item.slot()] == 2 &&
              !allowed(),
          "moved foreign goods retained previous plot authority");
  objects.position[item.slot()].x = 10;
  require(refresh(true) == std::vector<Handle>{item} && objects.plot[item.slot()] == 0,
          "out-of-plot goods retained a plot");
  objects.position[item.slot()].x = -1;
  refresh(true);
  require(refresh(false) == std::vector<Handle>{item} && objects.plot[item.slot()] == 0 &&
              !allowed(),
          "hidden goods retained placed-object authority");
  require(refresh(false).empty(), "unchanged membership produced a spurious change");
}

void object_checks_do_not_inherit_owner_or_groups_for_scripts() {
  Fixture f;
  f.players.rows[2].groups = {4, 9};
  Target target{Handle::make(10, 1), 1, 9, bit(Verb::Edit) << 8, 3};
  require(
      static_cast<bool>(check(Holder::player(1), Verb::Transfer, target, f.players, f.grants, 1)),
      "owner denied");
  require(static_cast<bool>(check(Holder::player(2), Verb::Edit, target, f.players, f.grants, 1)),
          "group member denied");
  const auto script = Holder::script(2000000000, Handle::make(20, 1));
  require(!check(script, Verb::Edit, target, f.players, f.grants, 1),
          "script borrowed player groups");
  const auto id = f.grant(1, script, Scope::object(target.object), 1, bit(Verb::Use));
  require(static_cast<bool>(check(script, Verb::Use, target, f.players, f.grants, 1)),
          "script grant denied");
  require(!check(Holder::script(script.id, Handle::make(21, 1)), Verb::Use, target, f.players,
                 f.grants, 1),
          "script grant leaked to another installation");
  require(!check(Holder::script(script.id, Handle::make(20, 2)), Verb::Use, target, f.players,
                 f.grants, 1),
          "script grant leaked across generation reuse");
  require(!check(script, Verb::Use, target, f.players, f.grants, 1000),
          "expired grant remained usable");
  const auto expired = advance_grants(f.grants, f.players, 1000, 0, 16);
  require(expired.expired == std::vector<GrantId>{id}, "due grant was not expired");
  require(f.grants.by_key.empty() && f.players.rows[1].grants_given.empty(),
          "expiry did not clean lookup/list");
  require(static_cast<bool>(
              check(Holder::script(7, target.object), Verb::Edit, target, f.players, f.grants, 1)),
          "script implicit self edit denied");
  require(!check(Holder::script(7, target.object), Verb::Destroy, target, f.players, f.grants, 1),
          "script gained implicit destroy");
  target.preset |= bit(Verb::Use);
  require(static_cast<bool>(check(script, Verb::Use, target, f.players, f.grants, 1)),
          "public script use denied");
}

void delegated_spending_shares_one_allowance_and_failure_is_atomic() {
  Fixture f;
  const PlayerId wallet = 1;
  const auto scope = Scope::account(Account::player(wallet));
  f.ledger.balance[Account::player(wallet)] = 1000;
  const auto parent = f.grant(1, Holder::player(2), scope, 1, 1, 100, 100, 500);
  const auto first = Holder::script(10, Handle::make(20, 1));
  const auto second = Holder::script(10, Handle::make(21, 1));
  const auto first_id = f.grant(2, first, scope, 1, 1, 100, 100, 400, 2);
  const auto second_id = f.grant(2, second, scope, 1, 1, 100, 100, 400, 2);
  require(f.grants.rows[first_id].parent == parent && f.grants.rows[second_id].parent == parent,
          "delegation did not retain parent");
  require(static_cast<bool>(pay(f.ledger, f.grants, first, AccountTarget::player(wallet),
                                Account::player(3), 70, 2)),
          "first payment failed");
  const auto denied =
      pay(f.ledger, f.grants, second, AccountTarget::player(wallet), Account::player(3), 40, 3);
  require(denied.reason == Reason::Allowance, "children enlarged parent allowance");
  require(balance(f.ledger, Account::player(wallet)) == 930 &&
              f.grants.rows[second_id].spent == 0 && f.grants.rows[parent].spent == 70,
          "failed cap check mutated balances or counters");
  require(allowance(f.grants, second_id, 3) == 30, "listing overstated a child's usable allowance");
  f.ledger.balance[Account::player(wallet)] = 5;
  const auto poor =
      pay(f.ledger, f.grants, second, AccountTarget::player(wallet), Account::player(3), 20, 4);
  require(poor.reason == Reason::InsufficientBalance && f.grants.rows[parent].spent == 70 &&
              f.grants.rows[second_id].spent == 0,
          "failed balance check consumed allowance");
  f.ledger.balance[Account::player(wallet)] = 1000;
  require(static_cast<bool>(pay(f.ledger, f.grants, second, AccountTarget::player(wallet),
                                Account::player(3), 100, 101)),
          "period reset failed");
  require(f.grants.rows[parent].spent == 100 && f.grants.rows[parent].period_start == 101,
          "parent period accounting differs");
  const auto removed = revoke(f.grants, f.players, parent);
  require(removed.size() == 3 && f.grants.by_key.empty() && f.players.rows[2].grants_held.empty() &&
              f.players.rows[2].grants_given.empty(),
          "revocation did not remove children and lists");
  require(
      !pay(f.ledger, f.grants, first, AccountTarget::player(wallet), Account::player(3), 1, 102),
      "revoked script still spent");
}

void delegation_cannot_expand_or_leave_its_owner() {
  Fixture f;
  const auto scope = Scope::account(Account::player(1));
  f.grant(1, Holder::player(2), scope, 1, 1, 100, 100, 500);
  const auto holder = Holder::script(10, Handle::make(20, 1));
  GrantRequest request{2, holder, scope, 1, 101, 100, 400};
  ScopeControl control{scope, 1, all_rights};
  HolderControl ownership{holder, 2, true};
  require(!create_grant(f.grants, f.players, perm::Holder::player(request.issuer), request, control,
                        ownership, 2),
          "expanded cap accepted");
  request.cap = 100;
  request.expires_step = 501;
  require(!create_grant(f.grants, f.players, perm::Holder::player(request.issuer), request, control,
                        ownership, 2),
          "expanded expiry accepted");
  request.expires_step = 400;
  ownership.controller = 3;
  require(!create_grant(f.grants, f.players, perm::Holder::player(request.issuer), request, control,
                        ownership, 2),
          "delegated to another owner's script");
  ownership.controller = 2;
  request.expires_step = 0;
  require(create_grant(f.grants, f.players, perm::Holder::player(request.issuer), request, control,
                       ownership, 2)
                  .reason == Reason::ExpiryRequired,
          "script accepted nonexpiring spending");
}

void scripts_delegate_only_actual_parent_authority() {
  Fixture f;
  const auto institution = Holder::script(50, Handle::make(20, 1));
  const auto land = Scope::plot(7);
  const auto root = f.grant(1, institution, land, 1, 7, 0, 0, 100, 1);
  const ScopeControl land_control{land, 1, 7, 7};
  const auto tenant = Holder::player(2);
  const auto lease = create_grant(f.grants, f.players, institution, {1, tenant, land, 3, 0, 0, 90},
                                  land_control, {tenant, 2, true}, 2);
  require(lease && f.grants.rows[lease.grant].parent == root &&
              check_scope(tenant, land, 3, f.grants, 2),
          "an institutional script cannot delegate its actual bounded land grant to a tenant");
  const auto stranger = Holder::script(51, Handle::make(21, 1));
  const auto delegated = create_grant(f.grants, f.players, tenant, {2, stranger, land, 1, 0, 0, 80},
                                      land_control, {stranger, 2, true}, 2);
  require(!delegated, "a delegated tenant grant became a second-level parent");
  require(!create_grant(f.grants, f.players, institution, {1, stranger, land, 1, 0, 0, 101},
                        land_control, {stranger, 3, true}, 2),
          "script delegation widened the parent's expiry");
  require(!create_grant(f.grants, f.players, institution,
                        {1, stranger, land, bit(Verb::Transfer), 0, 0, 90}, land_control,
                        {stranger, 3, true}, 2),
          "script delegation widened the parent's rights");
  const auto avatar = Scope::avatar(Handle::make(22, 1));
  const auto body = Holder::script(52, Handle::make(22, 1));
  const auto consent = f.grant(2, body, avatar, 2, 3, 0, 0, 100, 2);
  const auto seat = Holder::script(53, Handle::make(23, 1));
  const auto seated = create_grant(f.grants, f.players, body, {2, seat, avatar, 3, 0, 0, 50},
                                   {avatar, 2, 3}, {seat, 3, true}, 2);
  require(seated && f.grants.rows[seated.grant].parent == consent &&
              check_scope(seat, avatar, 3, f.grants, 2),
          "body consent cannot grant bounded animation/camera to a foreign seat script");
  const auto ungranted = Scope::object(Handle::make(24, 1));
  require(!create_grant(f.grants, f.players, institution,
                        {1, stranger, ungranted, bit(Verb::Use), 0, 0, 90},
                        {ungranted, 1, all_rights}, {stranger, 1, true}, 2),
          "a script borrowed its object's owner's grant-issuing authority");
  require(!create_grant(f.grants, f.players, Holder::player(2),
                        {1, stranger, ungranted, bit(Verb::Use), 0, 0, 90},
                        {ungranted, 1, all_rights}, {stranger, 1, true}, 2),
          "a caller named another player's grant issuer");
  const auto wallet = Scope::account(Account::player(1));
  const auto spending = f.grant(1, institution, wallet, 1, 1, 50, 100, 100, 1);
  const auto allowance =
      create_grant(f.grants, f.players, institution, {1, tenant, wallet, 1, 50, 100, 90},
                   {wallet, 1, 1}, {tenant, 2, true}, 2);
  require(allowance && f.grants.rows[allowance.grant].parent == spending,
          "script spending delegation lost its actual parent");
  f.ledger.balance[Account::player(1)] = 100;
  require(pay(f.ledger, f.grants, tenant, AccountTarget::player(1), Account::player(3), 30, 3) &&
              f.grants.rows[spending].spent == 30 &&
              !pay(f.ledger, f.grants, institution, AccountTarget::player(1), Account::player(3),
                   21, 3),
          "a foreign recipient enlarged the script's shared parent allowance");
}

void ineligible_object_grant_does_not_shadow_plot_delegation() {
  for (const bool expired_exact : {false, true}) {
    Fixture f;
    const auto object = Handle::make(55, 1);
    const auto own_scope = Scope::object(object);
    const auto plot_scope = Scope::owned_on_plot(1, 3);
    f.grant(1, Holder::player(2), own_scope, 1, bit(Verb::Use), 0, 0, expired_exact ? 2 : 500);
    const auto valid_parent = f.grant(1, Holder::player(2), plot_scope, 1, bit(Verb::Edit));
    const auto child = Holder::script(10, Handle::make(20, 1));
    const auto delegated = create_grant(f.grants, f.players, perm::Holder::player(2),
                                        {2, child, own_scope, bit(Verb::Edit), 0, 0, 400},
                                        {own_scope, 1, all_rights, 3}, {child, 2, true}, 3);
    require(static_cast<bool>(delegated) && f.grants.rows[delegated.grant].parent == valid_parent,
            "ineligible direct grant shadowed eligible plot grant");
  }
}

void expiry_batches_do_not_remove_future_grants() {
  Fixture f;
  const auto first =
      f.grant(1, Holder::player(2), Scope::object(Handle::make(3, 1)), 1, bit(Verb::Use), 0, 0, 10);
  const auto second =
      f.grant(1, Holder::player(2), Scope::object(Handle::make(4, 1)), 1, bit(Verb::Use), 0, 0, 20);
  const auto future =
      f.grant(1, Holder::player(2), Scope::object(Handle::make(5, 1)), 1, bit(Verb::Use), 0, 0, 30);
  require(advance_grants(f.grants, f.players, 20, 5, 16).expired ==
                  std::vector<GrantId>{first, second} &&
              f.players.rows[2].grants_held == std::vector<GrantId>{future},
          "batched expiry removed a future grant or left a stale list");
}

void reminder_deadlines_ignore_revocation_and_notify_once() {
  Fixture f;
  const auto scope = Scope::account(Account::player(1));
  const auto parent = f.grant(1, Holder::player(2), scope, 1, 1, 100, 100, 100);
  const auto child =
      f.grant(2, Holder::script(10, Handle::make(20, 1)), scope, 1, 1, 50, 100, 80, 2);
  const auto revoked = f.grant(1, Holder::script(11, Handle::make(21, 1)),
                               Scope::object(Handle::make(9, 1)), 1, bit(Verb::Use), 0, 0, 90);
  const auto ordinary =
      f.grant(1, Holder::player(3), Scope::object(Handle::make(8, 1)), 1, bit(Verb::Use), 0, 0, 75);
  revoke(f.grants, f.players, revoked);
  require(advance_grants(f.grants, f.players, 49, 30, 16).reminders.empty(),
          "grant reminder fired before its window");
  require(advance_grants(f.grants, f.players, 50, 30, 16).reminders == std::vector<GrantId>{child},
          "script grant did not receive its before-expiry reminder");
  require(advance_grants(f.grants, f.players, 60, 30, 16).reminders.empty(),
          "grant reminder repeated or a revoked grant was reminded");
  require(advance_grants(f.grants, f.players, 70, 30, 16).reminders == std::vector<GrantId>{parent},
          "spending grant did not receive its own reminder");
  const auto due = advance_grants(f.grants, f.players, 100, 30, 16);
  require(due.reminders.empty() && due.expired == std::vector<GrantId>({parent, child, ordinary}) &&
              f.grants.by_key.empty() && f.grants.expiring.empty() && f.grants.reminders.empty() &&
              f.players.rows[1].grants_given.empty() && f.players.rows[2].grants_given.empty() &&
              f.players.rows[2].grants_held.empty() && f.players.rows[3].grants_held.empty(),
          "due expiration did not clean children, stale deadlines and player lists together");
  require(advance_grants(f.grants, f.players, 101, 30, 16).expired.empty(),
          "expired grant was processed twice");
}

void overdue_grant_cleanup_waits_without_extending_authority() {
  Fixture f;
  const auto scope = Scope::account(Account::player(1));
  const auto parent = f.grant(1, Holder::player(2), scope, 1, 1, 100, 100, 100);
  const auto early = Holder::script(10, Handle::make(20, 1));
  const auto later = Holder::script(11, Handle::make(21, 1));
  const auto first = f.grant(2, early, scope, 1, 1, 100, 100, 90, 2);
  const auto second = f.grant(2, later, scope, 1, 1, 100, 100, 100, 2);
  require(advance_grants(f.grants, f.players, 100, 30, 0).expired.empty() &&
              f.grants.expiring.size() == 3,
          "zero grant work quota consumed pending deadlines");
  require(advance_grants(f.grants, f.players, 100, 30, 1).expired == std::vector<GrantId>{first} &&
              f.grants.rows[parent].live && f.grants.rows[second].live &&
              !check_account(Holder::player(2), AccountTarget::player(1), 1, f.grants, 100) &&
              !check_account(later, AccountTarget::player(1), 1, f.grants, 100),
          "deferred cleanup extended expired authority or exceeded its quota");
  require(advance_grants(f.grants, f.players, 100, 30, 1).expired == std::vector<GrantId>{parent} &&
              f.grants.rows[second].live,
          "automatic parent expiry bypassed the bounded deadline work lane");
  require(advance_grants(f.grants, f.players, 100, 30, 1).expired == std::vector<GrantId>{second} &&
              f.grants.by_key.empty() && f.players.rows[2].grants_given.empty(),
          "pending child expiry did not continue on the next pass");
}

void repeated_revocation_keeps_only_live_deadlines() {
  Fixture f;
  for (unsigned i = 0; i < 256; ++i) {
    const auto root =
        f.grant(1, Holder::player(2), Scope::account(Account::player(1)), 1, 1, 100, 100, 100000);
    const auto child = f.grant(2, Holder::script(8, Handle::make(20, 1)),
                               Scope::account(Account::player(1)), 1, 1, 50, 100, 90000, 2);
    require(f.grants.expiring.size() == 2 && f.grants.reminders.size() == 2,
            "live grants lack their exact deadline entries");
    require(revoke(f.grants, f.players, root).size() == 2 && !f.grants.rows[child].live &&
                f.grants.expiring.empty() && f.grants.reminders.empty(),
            "revocation retained long-lived expiry/reminder entries");
  }
}

void wallets_and_root_require_explicit_authority() {
  Fixture f;
  const auto object = Handle::make(7, 1);
  const auto script = Holder::script(10, object);
  f.ledger.balance[Account::player(2)] = 100;
  f.ledger.balance[Account::player(7)] = 200;
  require(!pay(f.ledger, f.grants, script, AccountTarget::player(2), Account::player(3), 10, 1),
          "script inherited its installed object's owner wallet");
  require(!pay(f.ledger, f.grants, script, AccountTarget::player(0), Account::player(2), 10, 1),
          "root payments need an explicit spend grant");
  const auto root = Scope::account(Account::player(0));
  const auto id = static_cast<GrantId>(f.grants.rows.size());
  f.grants.rows.push_back({id, 0, 0, script, root, 1, 50, 0, 100, 0, 200, true});
  f.grants.by_key.emplace(GrantKey{script, root}, id);
  f.players.rows[0].grants_given.push_back(id);
  require(
      bool(pay(f.ledger, f.grants, script, AccountTarget::player(0), Account::player(2), 40, 2)) &&
          balance(f.ledger, Account::player(2)) == 140 &&
          !f.ledger.balance.contains(Account::player(0)),
      "root creates money without keeping a balance");
  require(
      pay(f.ledger, f.grants, script, AccountTarget::player(0), Account::player(2), 11, 3).reason ==
              Reason::Allowance &&
          f.grants.rows[id].spent == 40,
      "root spending is capped without consuming a failed payment");
  require(
      bool(pay(f.ledger, f.grants, script, AccountTarget::player(0), Account::player(0), 7, 3)) &&
          f.grants.rows[id].spent == 40 && !f.ledger.balance.contains(Account::player(0)),
      "root paying itself changes neither circulation nor allowance");
  require(bool(pay(f.ledger, f.grants, Holder::player(2), AccountTarget::player(2),
                   Account::player(0), 5, 4)) &&
              balance(f.ledger, Account::player(2)) == 135 &&
              !f.ledger.balance.contains(Account::player(0)),
          "payments to root destroy money");
  require(
      bool(pay(f.ledger, f.grants, script, AccountTarget::player(0), Account::player(2), 5, 100)) &&
          f.grants.rows[id].spent == 5 && balance(f.ledger, Account::player(2)) == 140,
      "root spending allowance resets for the next period");
  require(!pay(f.ledger, f.grants, Holder::player(0), AccountTarget::player(0), Account::player(2),
               1, 101),
          "root can never act");
  require(!create_grant(f.grants, f.players, Holder::player(2),
                        {2, Holder::player(0), Scope::account(Account::player(2)), 1, 5, 100, 200},
                        {Scope::account(Account::player(2)), 2, 1}, {}, 1),
          "root can never hold a grant");
  const auto spend = f.grant(2, script, Scope::account(Account::player(2)), 2, 1, 50, 100, 200);
  f.ledger.balance[Account::player(3)] = std::numeric_limits<std::int64_t>::max();
  require(
      pay(f.ledger, f.grants, script, AccountTarget::player(2), Account::player(3), 1, 5).reason ==
              Reason::BalanceOverflow &&
          balance(f.ledger, Account::player(2)) == 140 && f.grants.rows[spend].spent == 0,
      "overflowing payment mutated source or allowance");
  require(
      pay(f.ledger, f.grants, Holder::player(2), AccountTarget::player(2), Account::group(0), 0, 5)
                  .reason == Reason::UnknownHolder &&
          pay(f.ledger, f.grants, Holder::player(2), AccountTarget::player(2), Account::player(3),
              -1, 5)
                  .reason == Reason::BadAmount,
      "invalid wallet or amount accepted");
  require(static_cast<bool>(pay(f.ledger, f.grants, Holder::player(2), AccountTarget::player(2),
                                Account::player(7), 5, 6)) &&
              balance(f.ledger, Account::player(2)) == 135 &&
              balance(f.ledger, Account::player(7)) == 205,
          "player could not pay directly from their own wallet");
}

void next_owner_limits_preserve_fair_use_and_clear_old_script_grants() {
  Fixture f;
  Objects objects;
  OwnerLimits limits;
  ObjectInit init;
  init.model = 1;
  init.owner = 1;
  init.preset = (all_rights & ~bit(Verb::Edit) & ~bit(Verb::Copy)) << 16;
  const auto object = create(objects, init);
  const auto script = Holder::script(10, object);
  f.grant(1, Holder::player(2), Scope::object(object), 1, bit(Verb::Use));
  f.grant(1, script, Scope::account(Account::player(1)), 1, 1, 100, 100, 500);
  transfer_owner(authority(objects), limits, permission_flags(objects), f.grants, f.players,
                 target(identity(objects), authority(objects), limits, object), 3);
  const auto sold = target(identity(objects), authority(objects), limits, object);
  require(sold.owner == 3 && (sold.flags & NoCopy), "owner/next-owner copy limit not applied");
  for (auto verb : {Verb::Use, Verb::Take, Verb::Move, Verb::Destroy, Verb::Transfer})
    require(static_cast<bool>(check(Holder::player(3), verb, sold, f.players, f.grants, 2)),
            "new owner lost permitted use/transfer or cannot remove junk");
  require(!check(Holder::player(3), Verb::Edit, sold, f.players, f.grants, 2) &&
              !check(Holder::player(3), Verb::Copy, sold, f.players, f.grants, 2),
          "new owner bypassed inherited edit/copy limit");
  require(f.grants.by_key.empty() && f.players.rows[1].grants_given.empty(),
          "sale retained old grants");
  require(static_cast<bool>(check(script, Verb::Edit, sold, f.players, f.grants, 2)),
          "installed behavior lost implicit own-object edit after sale");
  limits.erase(object.slot()); // Explicit owner cleanup precedes storage destruction and reuse.
  destroy(objects, object);
  const auto replacement = create(objects, init);
  require(!check(script, Verb::Use,
                 target(identity(objects), authority(objects), limits, replacement), f.players,
                 f.grants, 2),
          "reused slot inherited old script grant");
}

struct GoodsFixture : Fixture {
  amber::ObjectState objects_state;

  Objects &objects = objects_state.objects;
  OwnerLimits &limits = perm_state.owner_limits;
  Offers &offers = perm_state.offers;
  Handle goods;
  GoodsFixture() {
    ObjectInit init;
    init.model = 1;
    init.owner = 1;
    init.preset = all_rights << 16;
    goods = create(objects, init);
  }
  Target current() { return target(identity(objects), authority(objects), limits, goods); }
  OfferResult sell(std::int64_t price = 50, PlayerId buyer = 0) {
    return offer(perm_state, permission_flags(objects), Holder::player(1), current(), price, buyer,
                 100, true, 2);
  }
  SwapResult buy(OfferId id, Buyer buyer, std::size_t mailbox_max = 100) {
    return accept(perm_state, objects_state, buyer, current(), id, mailbox_max, 3);
  }
};

void land_bans_override_presets_and_scripts_use_their_own_grants() {
  Fixture f;
  Plot plot{3, 1, 8, Rights{2} | (Rights{1} << 8), {}, {4}};
  f.players.rows[2].groups = {8};
  require(static_cast<bool>(
              check_land(Holder::player(2), LandVerb::Build, plot, f.players, f.grants, 2)),
          "land group build denied");
  require(check_land(Holder::player(4), LandVerb::Enter, plot, f.players, f.grants, 2).reason ==
              Reason::Banned,
          "public enter bypassed ban");
  const auto script = Holder::script(17, Handle::make(8, 2));
  require(!check_land(script, LandVerb::Build, plot, f.players, f.grants, 2),
          "script inherited land group");
  const auto id = f.grant(1, script, Scope::plot(3), 1, Rights{1});
  require(static_cast<bool>(check_land(script, LandVerb::Build, plot, f.players, f.grants, 2)),
          "explicit script land grant denied");
  revoke(f.grants, f.players, id);
  require(!check_land(script, LandVerb::Build, plot, f.players, f.grants, 2),
          "revoked land grant remained usable");
}

void offers_preflight_before_money_ownership_and_allowances_change() {
  GoodsFixture f;
  const auto script = Holder::script(11, Handle::make(45, 1));
  const auto allowance = f.grant(2, script, Scope::account(Account::player(2)), 2, 1, 100, 100);
  const auto old_grant = f.grant(1, Holder::player(3), Scope::object(f.goods), 1, bit(Verb::Use));
  const auto offered = f.sell(50, 2);
  require(static_cast<bool>(offered) && (f.current().flags & Escrow) &&
              offered.custody.front().in_world && !offered.custody.front().parent,
          "offer did not escrow placed goods");
  require(f.sell().reason == Reason::InEscrow, "goods offered twice");
  const Buyer buyer{script, 2, AccountTarget::player(2), Handle::make(80, 1), true};
  f.ledger.balance[buyer.payer.account] = 49;
  const auto poor = f.buy(offered.offer, buyer);
  require(poor.reason == Reason::InsufficientBalance && f.current().owner == 1 &&
              f.offers.rows[offered.offer].live && (f.current().flags & Escrow) &&
              f.grants.rows[allowance].spent == 0 && f.grants.rows[old_grant].live &&
              f.players.rows[2].inventory.empty() && balance(f.ledger, buyer.payer.account) == 49,
          "failed acceptance changed valuable state");
  f.ledger.balance[buyer.payer.account] = 100;
  auto wrong = buyer;
  wrong.beneficiary = 3;
  require(f.buy(offered.offer, wrong).reason == Reason::WrongBuyer,
          "private offer accepted by another beneficiary");
  const auto bought = f.buy(offered.offer, buyer);
  require(static_cast<bool>(bought) && f.current().owner == 2 && !(f.current().flags & Escrow) &&
              !f.offers.rows[offered.offer].live && balance(f.ledger, buyer.payer.account) == 50 &&
              balance(f.ledger, Account::player(1)) == 50 && f.grants.rows[allowance].spent == 50 &&
              !f.grants.rows[old_grant].live &&
              f.players.rows[2].inventory == std::vector<Handle>{f.goods} &&
              bought.custody.front().parent == buyer.custodian && !bought.custody.front().in_world,
          "accepted offer did not commit complete swap");
  require(f.buy(offered.offer, buyer).reason == Reason::OfferGone, "offer accepted twice");
}

void vendors_need_explicit_rights_and_buyers_need_explicit_wallet_authority() {
  GoodsFixture f;
  const auto vendor = Holder::script(12, Handle::make(40, 1));
  auto make_offer = [&] {
    return offer(f.perm_state, permission_flags(f.objects), vendor, f.current(), 5, 0, 100, true,
                 2);
  };
  require(!make_offer(), "vendor inherited its owner's transfer right");
  f.grant(1, vendor, Scope::object(f.goods), 1, bit(Verb::Transfer));
  const auto offered = make_offer();
  require(static_cast<bool>(offered), "authorized vendor could not offer");
  f.ledger.balance[Account::player(2)] = 10;
  const auto buyer_script = Holder::script(13, Handle::make(41, 1));
  Buyer buyer{buyer_script, 2, AccountTarget::player(2), {}, false};
  require(f.buy(offered.offer, buyer).reason == Reason::MissingRight,
          "buyer script borrowed player's wallet");
  f.grant(2, buyer_script, Scope::account(buyer.payer.account), 2, 1, 5, 100);
  require(static_cast<bool>(f.buy(offered.offer, buyer)) &&
              balance(f.ledger, Account::player(1)) == 5 &&
              f.players.rows[2].mailbox == std::vector<Handle>{f.goods},
          "scripted vendor swap failed");
}

void gift_and_zero_price_offers_preserve_offline_custody_and_no_copy() {
  GoodsFixture f;
  permission_flags(f.objects).set(f.goods.slot(), NoCopy);
  f.players.rows[2].mailbox.push_back(Handle::make(90, 1));
  const auto offered = f.sell(0);
  const Buyer buyer{Holder::player(2), 2, AccountTarget::player(2), {}, false};
  require(f.buy(offered.offer, buyer, 1).reason == Reason::MailboxFull && f.current().owner == 1,
          "full mailbox lost goods");
  f.players.rows[2].mailbox.clear();
  require(static_cast<bool>(f.buy(offered.offer, buyer)) && (f.current().flags & NoCopy) &&
              std::all_of(f.ledger.balance.begin(), f.ledger.balance.end(),
                          [](auto value) { return value.second == 0; }),
          "free no-copy transfer needed funds or duplicated goods");
  const auto delivered = deliver_mailbox(f.players, 2, Handle::make(80, 1));
  require(delivered.size() == 1 && f.players.rows[2].mailbox.empty() &&
              f.players.rows[2].inventory == std::vector<Handle>{f.goods},
          "mailbox delivery lost or duplicated goods");
  require(deliver_mailbox(f.players, 2, Handle::make(80, 1)).empty(), "mail delivered twice");
  auto send = [&](std::size_t maximum) {
    return give(f.perm_state, f.objects_state, Holder::player(2), f.current(), 3, {}, false,
                maximum, 4);
  };
  require(send(0).reason == Reason::MailboxFull && f.current().owner == 2 &&
              f.players.rows[2].inventory.size() == 1,
          "bounced gift changed sender custody");
  require(static_cast<bool>(send(1)) && f.current().owner == 3 &&
              f.players.rows[2].inventory.empty() &&
              f.players.rows[3].mailbox == std::vector<Handle>{f.goods},
          "gift did not transfer");
}

void transferred_mailbox_goods_cannot_be_delivered_by_the_previous_owner() {
  GoodsFixture f;
  f.players.rows[1].inventory.clear();
  f.players.rows[1].mailbox = {f.goods};
  const auto offered = f.sell(0);
  const Buyer buyer{Holder::player(2), 2, AccountTarget::player(2), {}, false};
  require(static_cast<bool>(f.buy(offered.offer, buyer)) && f.players.rows[1].mailbox.empty() &&
              f.players.rows[2].mailbox == std::vector<Handle>{f.goods},
          "sale left a stale mailbox entry belonging to its previous owner");
  auto give = [&](std::size_t maximum) {
    return amber::perm::give(f.perm_state, f.objects_state, Holder::player(2), f.current(), 3, {},
                             false, maximum, 4);
  };
  require(give(0).reason == Reason::MailboxFull &&
              f.players.rows[2].mailbox == std::vector<Handle>{f.goods},
          "failed delivery removed goods from the sender's mailbox");
  require(static_cast<bool>(give(1)) && f.players.rows[2].mailbox.empty() &&
              f.players.rows[3].mailbox == std::vector<Handle>{f.goods} &&
              deliver_mailbox(f.players, 1, Handle::make(80, 1)).empty() &&
              deliver_mailbox(f.players, 2, Handle::make(81, 1)).empty(),
          "gift left a stale mailbox entry that could redeliver foreign goods");
}

void cancelling_and_expiry_clear_only_owned_escrow() {
  GoodsFixture f;
  const auto offered = f.sell();
  require(!cancel_offer(f.perm_state, permission_flags(f.objects), Holder::player(2), f.current(),
                        offered.offer, 3),
          "stranger cancelled escrow");
  require(
      advance_offers(f.offers, identity(f.objects), permission_flags(f.objects), 99, 256).empty() &&
          (f.current().flags & Escrow),
      "offer expired early");
  require(advance_offers(f.offers, identity(f.objects), permission_flags(f.objects), 100, 256) ==
                  std::vector<OfferId>{offered.offer} &&
              !(f.current().flags & Escrow) &&
              advance_offers(f.offers, identity(f.objects), permission_flags(f.objects), 101, 256)
                  .empty(),
          "indexed offer deadline clears escrow once at its actual due step");
}

void offer_deadlines_skip_completed_quotes_and_rebuild_on_restore() {
  const auto advance = [](GoodsFixture &f, Step step) {
    return advance_offers(f.offers, identity(f.objects), permission_flags(f.objects), step, 256);
  };
  {
    GoodsFixture f;
    const auto offered = f.sell(0);
    require(bool(f.buy(offered.offer, {Holder::player(2), 2, AccountTarget::player(2),
                                       Handle::make(80, 1), true})),
            "accepted deadline fixture");
    const auto owner = f.current().owner;
    const auto inventory = f.players.rows[2].inventory;
    require(f.offers.expiring.empty() && advance(f, 100).empty() && f.current().owner == owner &&
                f.players.rows[2].inventory == inventory,
            "old sale deadline does not expire already purchased goods or repeat a transfer");
  }
  {
    GoodsFixture f;
    const auto offered = f.sell();
    require(bool(cancel_offer(f.perm_state, permission_flags(f.objects), Holder::player(1),
                              f.current(), offered.offer, 3)),
            "cancelled deadline fixture");
    require(f.offers.expiring.empty() && advance(f, 100).empty() && !(f.current().flags & Escrow),
            "cancelled offer retains no future deadline");
  }
  {
    GoodsFixture f;
    const auto offered = f.sell();
    Trades trades;
    invalidate_goods_offers(f.offers, trades, identity(f.objects), permission_flags(f.objects),
                            f.goods);
    destroy(f.objects, f.goods);
    require(advance(f, 100).empty() && !f.offers.rows[offered.offer].live,
            "normal retirement invalidates a quoted item before its old deadline is consumed");
  }
  {
    GoodsFixture f;
    const auto offered = f.sell();
    const auto old = f.goods;
    destroy(f.objects, old);
    ObjectInit init;
    init.model = 1;
    init.owner = 2;
    init.flags = Escrow;
    f.goods = create(f.objects, init);
    require(f.goods.slot() == old.slot() && f.goods != old &&
                advance(f, 100) == std::vector<OfferId>{offered.offer} &&
                (f.current().flags & Escrow),
            "a delayed quote cannot clear escrow on a reused generation even with a stale row");
  }
  {
    GoodsFixture f;
    const auto first = f.sell();
    ObjectInit init;
    init.model = 1;
    init.owner = 1;
    const auto other = create(f.objects, init);
    const auto second = offer(f.perm_state, permission_flags(f.objects), Holder::player(1),
                              target(identity(f.objects), authority(f.objects), f.limits, other), 1,
                              0, 100, true, 2);
    require(first && second &&
                advance_offers(f.offers, identity(f.objects), permission_flags(f.objects), 100, 0)
                    .empty() &&
                f.offers.expiring.size() == 2,
            "zero deadline budget keeps real pending offers untouched");
    require(advance_offers(f.offers, identity(f.objects), permission_flags(f.objects), 100, 1) ==
                    std::vector<OfferId>{first.offer} &&
                f.offers.expiring.size() == 1 && f.offers.rows[second.offer].live,
            "bounded deadline processing consumes only one real due offer");
    Trades trades;
    snap::Buffer bytes;
    snap::Output out{bytes};
    write_grants(out, f.grants, f.offers, trades, f.limits);
    bytes.resize(out.adapter().writtenBytesCount());
    GoodsFixture restored;
    restored.objects = f.objects;
    snap::Input in{bytes.data(), bytes.size()};
    read_grants(in, restored.grants, restored.offers, trades, restored.limits,
                restored.objects.generation.size());
    snap::check_complete(in);
    require(restored.offers.expiring.size() == 1 && advance(restored, 99).empty() &&
                advance(restored, 100) == std::vector<OfferId>{second.offer} &&
                advance(restored, 101).empty() &&
                !(permission_flags(restored.objects).get(other.slot()) & Escrow),
            "restoration rebuilds only live deadlines and expires the remaining sale once");
  }
}

void direct_trade_rechecks_both_sides_resets_confirmation_and_cannot_be_bought_piecemeal() {
  GoodsFixture f;
  auto &perm_state = f.perm_state;
  Trades &trades = perm_state.trades;
  ObjectInit init;
  init.model = 2;
  init.owner = 2;
  init.preset = all_rights << 16;
  const auto second = create(f.objects, init);
  const std::array<Handle, 2> bodies{Handle::make(70, 1), Handle::make(80, 1)};
  f.players.rows[1].inventory.push_back(f.goods);
  f.players.rows[2].inventory.push_back(second);
  f.ledger.balance[Account::player(1)] = 100;
  f.ledger.balance[Account::player(2)] = 50;
  const auto id = open_trade(trades, f.players, 1, 2).trade;
  const auto first_target = f.current();
  auto second_target = target(identity(f.objects), authority(f.objects), f.limits, second);
  require(static_cast<bool>(change_trade(perm_state, permission_flags(f.objects), id, 1,
                                         &first_target, 10, false, 2)) &&
              static_cast<bool>(change_trade(perm_state, permission_flags(f.objects), id, 2,
                                             &second_target, 5, false, 2)),
          "direct trade sides were not offered");
  const auto offered = trades.rows[id].sides[0].offer;
  const Buyer buyer{Holder::player(2), 2, AccountTarget::player(2), bodies[1], true};
  require(f.buy(offered, buyer).reason == Reason::OfferGone,
          "one side of direct trade accepted independently");
  auto targets = [&] {
    return std::array<Target, 2>{
        f.current(), target(identity(f.objects), authority(f.objects), f.limits, second)};
  };
  auto confirm = [&](PlayerId player, const std::array<Handle, 2> &present) {
    return confirm_trade(perm_state, f.objects_state, id, player, targets(), present, 3);
  };
  require(confirm(3, bodies).reason == Reason::NotParticipant, "stranger confirmed trade");
  require(confirm(1, {bodies[0], {}}).reason == Reason::NotPresent, "absent body confirmed trade");
  require(static_cast<bool>(confirm(1, bodies)) && !trades.rows[id].sides[1].confirmed &&
              f.current().owner == 1,
          "first confirmation changed valuables");
  second_target = targets()[1];
  require(static_cast<bool>(change_trade(perm_state, permission_flags(f.objects), id, 2,
                                         &second_target, 6, false, 3)) &&
              !trades.rows[id].sides[0].confirmed && !trades.rows[id].sides[1].confirmed,
          "editing trade did not reset both confirmations");
  require(!confirm(2, bodies).completed && f.current().owner == 1,
          "edit reused a stale confirmation");
  f.ledger.balance[Account::player(1)] = 9;
  require(confirm(1, bodies).reason == Reason::InsufficientBalance && f.current().owner == 1 &&
              targets()[1].owner == 2 && balance(f.ledger, Account::player(2)) == 50 &&
              trades.rows[id].live && (f.current().flags & Escrow),
          "failed second confirmation partly committed");
  f.ledger.balance[Account::player(1)] = 100;
  const auto completed = confirm(1, bodies);
  require(completed.completed && !trades.rows[id].live && f.current().owner == 2 &&
              targets()[1].owner == 1 && balance(f.ledger, Account::player(1)) == 96 &&
              balance(f.ledger, Account::player(2)) == 54 &&
              f.players.rows[1].inventory == std::vector<Handle>{second} &&
              f.players.rows[2].inventory == std::vector<Handle>{f.goods},
          "bilateral trade did not commit both items and both money amounts");
  require(confirm(1, bodies).reason == Reason::OfferGone, "completed trade applied twice");
  const auto cancel_id = open_trade(trades, f.players, 1, 2).trade;
  const auto returned_target = targets()[1];
  const auto changed = change_trade(perm_state, permission_flags(f.objects), cancel_id, 1,
                                    &returned_target, 0, false, 4);
  require(static_cast<bool>(changed), "second trade could not offer goods");
  const auto cancelled = cancel_trade(trades, f.offers, permission_flags(f.objects), cancel_id, 2);
  require(static_cast<bool>(cancelled) && cancelled.custody.empty() &&
              !(targets()[1].flags & Escrow) && targets()[1].owner == 1,
          "cancelling trade changed valuables or attempted custody undo");
}

void retired_trade_goods_cannot_change_a_reused_slot() {
  for (bool replace_side : {false, true}) {
    GoodsFixture f;
    auto &perm_state = f.perm_state;
    Trades &trades = perm_state.trades;
    const auto id = open_trade(trades, f.players, 1, 2).trade;
    const auto current = f.current();
    const auto offered_trade =
        change_trade(perm_state, permission_flags(f.objects), id, 1, &current, 0, true, 2);
    require(static_cast<bool>(offered_trade), "could not offer retiring trade");
    const auto retired = invalidate_goods_offers(f.offers, trades, identity(f.objects),
                                                 permission_flags(f.objects), f.goods);
    require(retired.size() == 1 && !trades.rows[id].live,
            "destruction did not cancel enclosing trade before reuse");
    destroy(f.objects, f.goods);
    ObjectInit init;
    init.model = 1;
    init.owner = 1;
    init.flags = Static | Escrow;
    const auto replacement = create(f.objects, init);
    require(replacement.slot() == f.goods.slot(), "fixture did not reuse slot");
    const auto result =
        replace_side
            ? change_trade(perm_state, permission_flags(f.objects), id, 1, nullptr, 1, false, 3)
            : cancel_trade(trades, f.offers, permission_flags(f.objects), id, 2);
    require(result.reason == Reason::OfferGone && result.custody.empty() &&
                (identity(f.objects).flags[replacement.slot()] & Escrow),
            "retired trade modified replacement custody/flags");
  }
}

void assembly_offers_preflight_and_transfer_every_declared_part() {
  GoodsFixture f;
  ObjectInit init;
  init.model = 2;
  init.owner = 1;
  init.parent = f.goods;
  init.preset = (all_rights & ~bit(Verb::Copy)) << 16;
  const auto part = create(f.objects, init);
  auto components = [&] {
    return std::array<Target, 1>{target(identity(f.objects), authority(f.objects), f.limits, part)};
  };
  const auto component_grant =
      f.grant(1, Holder::script(20, part), Scope::account(Account::player(1)), 1, 1, 100, 100);
  const auto vendor = Holder::script(25, Handle::make(90, 1));
  f.grant(1, vendor, Scope::object(f.goods), 1, bit(Verb::Transfer));
  const auto offered = offer(f.perm_state, permission_flags(f.objects), vendor, f.current(), 25, 0,
                             100, true, 2, components());
  require(static_cast<bool>(offered) && (components()[0].flags & Escrow),
          "assembly offer left component outside escrow");
  f.ledger.balance[Account::player(2)] = 50;
  const Buyer buyer{Holder::player(2), 2, AccountTarget::player(2), Handle::make(80, 1), true};
  require(f.buy(offered.offer, buyer).reason == Reason::StaleGoods,
          "assembly accepted with missing component descriptor");
  f.limits.set(part.slot(), inherited_rights & ~bit(Verb::Transfer));
  auto buy_assembly = [&] {
    return accept(f.perm_state, f.objects_state, buyer, f.current(), offered.offer, 100, 3,
                  components());
  };
  require(buy_assembly().reason == Reason::InheritedLimit && f.current().owner == 1 &&
              components()[0].owner == 1 && balance(f.ledger, buyer.payer.account) == 50 &&
              f.grants.rows[component_grant].live,
          "restricted component allowed partial assembly sale");
  f.limits.erase(part.slot());
  require(static_cast<bool>(buy_assembly()) && f.current().owner == 2 &&
              components()[0].owner == 2 && !(components()[0].flags & Escrow) &&
              (components()[0].flags & NoCopy) && !f.grants.rows[component_grant].live &&
              f.players.rows[2].inventory == std::vector<Handle>{f.goods},
          "assembly sale left old component owner/grants or made separate inventory goods");
  const auto given = give(f.perm_state, f.objects_state, Holder::player(2), f.current(), 3, {},
                          false, 100, 4, components());
  require(static_cast<bool>(given) && f.current().owner == 3 && components()[0].owner == 3 &&
              f.players.rows[3].mailbox == std::vector<Handle>{f.goods},
          "assembly gift separated component ownership or delivery");
}

void storing_existing_goods_checks_then_changes_custody_directly() {
  GoodsFixture f;
  const auto script = Holder::script(50, Handle::make(50, 1));
  const auto store = [&](PlayerId recipient, Handle body, bool present, std::size_t maximum) {
    return store_goods(f.perm_state, script, f.current(), {}, recipient, body, present, maximum, 2);
  };
  require(store(1, {}, false, 1).reason == Reason::MissingRight &&
              f.players.rows[1].mailbox.empty(),
          "holding goods inherited script owner authority");
  f.grant(1, script, Scope::object(f.goods), 1, bit(Verb::Transfer));
  require(store(2, {}, false, 1).reason == Reason::NotController && f.current().owner == 1 &&
              f.players.rows[2].mailbox.empty(),
          "hold redirected goods to an arbitrary recipient");
  require(store(1, {}, false, 0).reason == Reason::MailboxFull && f.players.rows[1].mailbox.empty(),
          "failed holding changed the owner's custody lists");
  auto held = store(1, {}, false, 1);
  require(held && held.custody.front().goods == f.goods && !held.custody.front().in_world &&
              f.players.rows[1].mailbox == std::vector<Handle>{f.goods},
          "direct store did not publish the actual owner custody delta");
  const auto body = Handle::make(80, 1);
  held = store(1, body, true, 0);
  require(held && held.custody.front().parent == body && f.players.rows[1].mailbox.empty() &&
              f.players.rows[1].inventory == std::vector<Handle>{f.goods} &&
              store(1, body, true, 0) && f.players.rows[1].inventory.size() == 1 &&
              f.current().owner == 1,
          "direct holding duplicated goods or altered existing ownership");
}

void typed_accounts_preserve_custody_identity_and_bound_refunds() {
  GoodsFixture f;
  ObjectInit init;
  init.model = 1;
  init.owner = 1;
  const auto other = create(f.objects, init);
  const auto account = Account::object(f.goods);
  const auto group = Account::group(1);
  const auto script = Holder::script(77, f.goods);
  f.ledger.balance[account] = 100;
  f.ledger.balance[group] = 60;
  f.ledger.balance[Account::player(1)] = 20;
  require(!pay(f.ledger, f.grants, script, {account, 1}, group, 10, 2),
          "an installed script inherited its object's spending authority");
  const auto root = f.grant(1, Holder::player(2), Scope::account(account), 1, 1, 30, 100);
  const auto child = f.grant(2, script, Scope::account(account), 1, 1, 30, 100, 500, 2);
  require(pay(f.ledger, f.grants, script, {account, 1}, group, 21, 2) &&
              !pay(f.ledger, f.grants, Holder::player(2), {account, 1}, group, 10, 3) &&
              f.grants.rows[root].spent == 21 && f.grants.rows[child].spent == 21 &&
              balance(f.ledger, group) == 81 && balance(f.ledger, Account::player(1)) == 20,
          "typed object/group payment lost shared caps or aliased the owner's wallet");
  transfer_owner(authority(f.objects), f.limits, permission_flags(f.objects), f.grants, f.players,
                 f.current(), 3);
  require(balance(f.ledger, account) == 79 && !f.grants.rows[root].live &&
              !f.grants.rows[child].live &&
              !pay(f.ledger, f.grants, script, {account, 3}, group, 1, 4) &&
              pay(f.ledger, f.grants, Holder::player(3), {account, 3}, group, 5, 4),
          "sale moved object funds or retained the previous controller's account grants");
  const auto copy = create(f.objects, init);
  require(balance(f.ledger, Account::object(copy)) == 0,
          "copy or new object inherited another object's funds");
  f.objects.owner[other.slot()] = 3;
  f.ledger.balance[Account::object(other)] = 7;
  const std::array removed{f.goods, other};
  f.ledger.balance[Account::player(3)] = std::numeric_limits<std::int64_t>::max() - 80;
  const auto before = f.ledger.balance;
  require(can_refund_object_accounts(f.ledger, authority(f.objects), removed) ==
                  Reason::BalanceOverflow &&
              f.ledger.balance == before,
          "refund preflight checked each object separately instead of their summed owner credit");
  f.ledger.balance[Account::player(3)] = 0;
  require(can_refund_object_accounts(f.ledger, authority(f.objects), removed) == Reason::Allowed,
          "valid aggregate refund refused");
  const auto refunded = refund_object_accounts(f.ledger, authority(f.objects), removed);
  require(refunded.size() == 2 && balance(f.ledger, Account::player(3)) == 81 &&
              !f.ledger.balance.contains(account) &&
              !f.ledger.balance.contains(Account::object(other)) && balance(f.ledger, group) == 86,
          "destruction failed to refund current owner exactly and remove object balances");
  const auto old = f.goods;
  destroy(f.objects, old);
  const auto reused = create(f.objects, init);
  require(reused.slot() == old.slot() && reused.generation() != old.generation() &&
              balance(f.ledger, Account::object(reused)) == 0,
          "generation reuse inherited a destroyed object's balance");
}

void snapshot_keeps_shared_allowances_escrow_and_pending_confirmation() {
  GoodsFixture f;
  auto &trades = f.perm_state.trades;
  auto &plots = f.perm_state.plots;
  auto &groups = f.perm_state.groups;
  plots.rows.push_back({1, 1, 4, Rights{2}, {-1, -2, 3, 4}, {3}});
  groups.rows.push_back({1, 1, "Authored group"});
  const PlayerId wallet = 1;
  f.ledger.balance[Account::player(wallet)] = 100;
  f.ledger.balance[Account::player(2)] = 25;
  const auto parent =
      f.grant(1, Holder::player(3), Scope::account(Account::player(wallet)), 1, 1, 50, 100);
  const auto script = Holder::script(31, Handle::make(55, 2));
  const auto child =
      f.grant(3, script, Scope::account(Account::player(wallet)), 1, 1, 50, 100, 500, 3);
  require(static_cast<bool>(pay(f.ledger, f.grants, script, AccountTarget::player(wallet),
                                Account::player(4), 40, 2)),
          "snapshot spending setup failed");
  f.limits.set(f.goods.slot(), inherited_rights & ~bit(Verb::Edit));
  const auto trade = open_trade(trades, f.players, 1, 2).trade;
  const auto goods = f.current();
  change_trade(f.perm_state, permission_flags(f.objects), trade, 1, &goods, 0, true, 3);
  change_trade(f.perm_state, permission_flags(f.objects), trade, 2, nullptr, 10, false, 3);
  const auto object_account = Account::object(f.goods);
  const auto group_account = Account::group(1);
  f.ledger.balance[object_account] = 17;
  f.ledger.balance[group_account] = 4;
  const auto object_script = Holder::script(32, f.goods);
  const auto object_grant =
      f.grant(1, object_script, Scope::account(object_account), 1, 1, 25, 100);
  const std::array<Handle, 2> bodies{Handle::make(60, 1), Handle::make(61, 1)};
  confirm_trade(f.perm_state, f.objects_state, trade, 1, {f.current(), Target{}}, bodies, 4);
  snap::Buffer bytes;
  snap::Output out{bytes};
  write_players(out, f.players, f.ledger, plots, groups);
  write_grants(out, f.grants, f.offers, trades, f.limits);
  bytes.resize(out.adapter().writtenBytesCount());
  GoodsFixture restored;
  restored.objects = f.objects;
  auto &restored_trades = restored.perm_state.trades;
  Plots restored_plots;
  Groups restored_groups;
  snap::Input in{bytes.data(), bytes.size()};
  read_players(in, restored.players, restored.ledger, restored_plots, restored_groups);
  read_grants(in, restored.grants, restored.offers, restored_trades, restored.limits,
              restored.objects.generation.size());
  snap::check_complete(in);
  require(restored_plots.rows[1].banned == std::vector<PlayerId>{3} &&
              restored_groups.rows[1].name == "Authored group" &&
              restored.grants.rows[child].parent == parent &&
              restored.grants.rows[parent].spent == 40,
          "permission snapshot lost land/group/delegation state");
  require(restored.grants.rows[object_grant].scope.account() == object_account &&
              balance(restored.ledger, object_account) == 17 &&
              balance(restored.ledger, group_account) == 4 &&
              pay(restored.ledger, restored.grants, object_script, {object_account, 1},
                  group_account, 5, 5) &&
              balance(restored.ledger, object_account) == 12 &&
              balance(restored.ledger, group_account) == 9,
          "snapshot lost typed account keys, object Spend scope or continued payment");
  require(pay(restored.ledger, restored.grants, script, AccountTarget::player(wallet),
              Account::player(4), 11, 5)
                  .reason == Reason::Allowance,
          "restored child enlarged shared allowance");
  const auto result = confirm_trade(restored.perm_state, restored.objects_state, trade, 2,
                                    {restored.current(), Target{}}, bodies, 5);
  require(result.completed && restored.current().owner == 2 &&
              balance(restored.ledger, Account::player(2)) == 15 &&
              !(restored.current().owner_limit & bit(Verb::Edit)),
          "restored bilateral confirmation/escrow/next-owner ceiling did not continue");
  require(advance_grants(restored.grants, restored.players, 470, 30, 16).reminders ==
                  std::vector<GrantId>{child} &&
              advance_grants(restored.grants, restored.players, 500, 30, 16).expired ==
                  std::vector<GrantId>{child} &&
              restored.grants.rows[parent].live,
          "restored grant deadlines did not resume reminders and exact expiry");
}
} // namespace

int main() {
  try {
    moved_goods_leave_plot_scoped_authority();
    object_checks_do_not_inherit_owner_or_groups_for_scripts();
    delegated_spending_shares_one_allowance_and_failure_is_atomic();
    delegation_cannot_expand_or_leave_its_owner();
    scripts_delegate_only_actual_parent_authority();
    ineligible_object_grant_does_not_shadow_plot_delegation();
    expiry_batches_do_not_remove_future_grants();
    reminder_deadlines_ignore_revocation_and_notify_once();
    overdue_grant_cleanup_waits_without_extending_authority();
    repeated_revocation_keeps_only_live_deadlines();
    wallets_and_root_require_explicit_authority();
    next_owner_limits_preserve_fair_use_and_clear_old_script_grants();
    land_bans_override_presets_and_scripts_use_their_own_grants();
    offers_preflight_before_money_ownership_and_allowances_change();
    vendors_need_explicit_rights_and_buyers_need_explicit_wallet_authority();
    gift_and_zero_price_offers_preserve_offline_custody_and_no_copy();
    transferred_mailbox_goods_cannot_be_delivered_by_the_previous_owner();
    cancelling_and_expiry_clear_only_owned_escrow();
    offer_deadlines_skip_completed_quotes_and_rebuild_on_restore();
    direct_trade_rechecks_both_sides_resets_confirmation_and_cannot_be_bought_piecemeal();
    retired_trade_goods_cannot_change_a_reused_slot();
    assembly_offers_preflight_and_transfer_every_declared_part();
    storing_existing_goods_checks_then_changes_custody_directly();
    typed_accounts_preserve_custody_identity_and_bound_refunds();
    snapshot_keeps_shared_allowances_escrow_and_pending_confirmation();
    std::cout << "permission tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
