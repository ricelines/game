#include "perm.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace amber::perm {
std::string_view reason_text(Reason reason) {
  static constexpr std::array names{"allowed",
                                    "unknown holder",
                                    "no grant",
                                    "next-owner ceiling",
                                    "no-copy",
                                    "expired",
                                    "spending allowance exceeded",
                                    "insufficient balance",
                                    "invalid amount",
                                    "balance overflow",
                                    "already exists",
                                    "invalid delegation",
                                    "expiry required",
                                    "invalid scope",
                                    "not controller",
                                    "banned",
                                    "body is not goods",
                                    "not placed",
                                    "in escrow",
                                    "offer gone",
                                    "wrong buyer",
                                    "mailbox full",
                                    "stale goods",
                                    "not a participant",
                                    "not present"};
  static_assert(names.size() == static_cast<std::size_t>(Reason::NotPresent) + 1);
  return names[static_cast<std::size_t>(reason)];
}
namespace {
std::uint64_t mix(std::uint64_t value) {
  value ^= value >> 30;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27;
  value *= 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

const Grant *find(const Grants &grants, Holder holder, Scope scope) {
  const auto found = grants.by_key.find({holder, scope});
  return found == grants.by_key.end() ? nullptr : &grants.rows[found->second];
}

bool expired(const Grant &grant, Step now) {
  return grant.expires_step != 0 && now >= grant.expires_step;
}

std::int64_t current_spent(const Grant &grant, Step now) {
  return grant.period_steps != 0 && now - grant.period_start >= grant.period_steps ? 0
                                                                                   : grant.spent;
}

Decision usable(const Grant *grant, Rights required, Step now) {
  if (!grant || !grant->live || (grant->verbs & required) != required)
    return {Reason::MissingRight};
  if (expired(*grant, now))
    return {Reason::Expired};
  return {Reason::Allowed, grant->id};
}

Decision budget(const Grants &grants, const Grant *grant, std::int64_t amount, Step now) {
  auto decision = usable(grant, 1, now);
  if (!decision)
    return decision;
  if (amount > grant->cap - current_spent(*grant, now))
    return {Reason::Allowance, grant->id};
  if (grant->parent) {
    const auto &parent = grants.rows[grant->parent];
    if (!parent.live || expired(parent, now))
      return {Reason::Expired, parent.id};
    if (amount > parent.cap - current_spent(parent, now))
      return {Reason::Allowance, parent.id};
  }
  return decision;
}

void spend(Grants &grants, GrantId id, std::int64_t amount, Step now) {
  if (id == 0)
    return;
  auto &grant = grants.rows[id];
  if (grant.period_steps != 0 && now - grant.period_start >= grant.period_steps) {
    grant.spent = 0;
    grant.period_start = now;
  }
  grant.spent += amount;
  if (grant.parent) {
    auto &parent = grants.rows[grant.parent];
    if (parent.period_steps != 0 && now - parent.period_start >= parent.period_steps) {
      parent.spent = 0;
      parent.period_start = now;
    }
    parent.spent += amount;
  }
}

std::vector<GrantId> erase_marked(Grants &grants, Players &players,
                                  const std::vector<bool> &marked) {
  std::vector<GrantId> removed;
  std::vector<PlayerId> affected;
  for (auto &grant : grants.rows) {
    if (!grant.live || !marked[grant.id])
      continue;
    grants.by_key.erase({grant.holder, grant.scope});
    grant.live = false;
    removed.push_back(grant.id);
    affected.push_back(grant.issuer);
    if (grant.holder.kind == HolderKind::Player)
      affected.push_back(grant.holder.id);
  }
  std::sort(affected.begin(), affected.end());
  affected.erase(std::unique(affected.begin(), affected.end()), affected.end());
  for (auto id : affected) {
    std::erase_if(players.rows[id].grants_given, [&](GrantId grant) { return marked[grant]; });
    std::erase_if(players.rows[id].grants_held, [&](GrantId grant) { return marked[grant]; });
  }
  return removed;
}

std::vector<GrantId> remove_marked(Grants &grants, Players &players, std::vector<bool> &marked) {
  // Explicit revocation closes over children immediately. Automatic expiration
  // can drain its index in batches: a child's expiry never exceeds its parent's.
  for (const auto &grant : grants.rows)
    if (grant.live && grant.parent && marked[grant.parent])
      marked[grant.id] = true;
  return erase_marked(grants, players, marked);
}

bool covered(const Grant &parent, const ScopeControl &control) {
  return parent.scope == control.scope ||
         (parent.scope.kind == ScopeKind::OwnedOnPlot && control.scope.kind == ScopeKind::Object &&
          parent.scope.owner == control.controller && parent.scope.subject == control.plot);
}

bool monetary(Scope scope) {
  return scope.kind == ScopeKind::Account || scope.kind == ScopeKind::Issuance;
}
} // namespace

std::size_t GrantKeyHash::operator()(const GrantKey &key) const {
  return mix(key.holder.id | (std::uint64_t{static_cast<unsigned>(key.holder.kind)} << 32)) ^
         mix(key.holder.installed_on.bits) ^ mix(key.scope.subject) ^
         mix(key.scope.owner | (std::uint64_t{static_cast<unsigned>(key.scope.kind)} << 32));
}

Target target(IdentityRead identity, AuthorityRead authority, const OwnerLimits &limits, Handle h) {
  assert(identity.valid(h));
  const auto slot = h.slot();
  const auto *limit = limits.get(slot);
  return {h,
          authority.owner[slot],
          authority.group[slot],
          authority.preset[slot],
          authority.plot[slot],
          identity.flags[slot],
          limit ? *limit : inherited_rights};
}

Decision check(Holder actor, Verb verb, const Target &target, const Players &players,
               const Grants &grants, Step now) {
  if (actor.id == 0 || (actor.kind == HolderKind::Player && actor.id >= players.rows.size()))
    return {Reason::UnknownHolder};
  if (actor.kind == HolderKind::Script && actor.installed_on == target.object &&
      (verb == Verb::Move || verb == Verb::Edit))
    return {Reason::Allowed};
  const auto required = bit(verb);
  if (verb == Verb::Copy && (target.flags & NoCopy))
    return {Reason::NoCopy};
  if ((required & inherited_rights) && !(target.owner_limit & required))
    return {Reason::InheritedLimit};
  if (actor.kind == HolderKind::Player && actor.id == target.owner)
    return {Reason::Allowed};
  if (target.preset & required)
    return {Reason::Allowed};
  if (actor.kind == HolderKind::Player && target.group && (target.preset & (required << 8))) {
    const auto &groups = players.rows[actor.id].groups;
    if (std::binary_search(groups.begin(), groups.end(), target.group))
      return {Reason::Allowed};
  }
  const auto direct = usable(find(grants, actor, Scope::object(target.object)), required, now);
  if (direct)
    return direct;
  const auto plot =
      usable(find(grants, actor, Scope::owned_on_plot(target.owner, target.plot)), required, now);
  if (plot)
    return plot;
  return direct.reason == Reason::Expired ? direct : plot;
}

Decision check_scope(Holder actor, Scope scope, Rights required, const Grants &grants, Step now) {
  return usable(find(grants, actor, scope), required, now);
}

Decision check_land(Holder actor, LandVerb verb, const Plot &plot, const Players &players,
                    const Grants &grants, Step now) {
  if (actor.id == 0 || (actor.kind == HolderKind::Player && actor.id >= players.rows.size()))
    return {Reason::UnknownHolder};
  if (actor.kind == HolderKind::Player) {
    if (std::binary_search(plot.banned.begin(), plot.banned.end(), actor.id))
      return {Reason::Banned};
    if (actor.id == plot.owner)
      return {Reason::Allowed};
  }
  const auto required = Rights{1} << static_cast<unsigned>(verb);
  // Transfer is an explicit grant, not an additional public land-preset bit.
  if (verb != LandVerb::Transfer) {
    if (plot.preset & required)
      return {Reason::Allowed};
    if (actor.kind == HolderKind::Player && plot.group && (plot.preset & (required << 8))) {
      const auto &groups = players.rows[actor.id].groups;
      if (std::binary_search(groups.begin(), groups.end(), plot.group))
        return {Reason::Allowed};
    }
  }
  return check_scope(actor, Scope::plot(plot.id), required, grants, now);
}

Decision check_account(Holder actor, PlayerId wallet, std::int64_t amount, const Grants &grants,
                       Step now) {
  if (amount < 0)
    return {Reason::BadAmount};
  if (!wallet || !actor.id)
    return {Reason::UnknownHolder};
  if (actor.kind == HolderKind::Player && actor.id == wallet)
    return {Reason::Allowed};
  return budget(grants, find(grants, actor, Scope::account(wallet)), amount, now);
}

std::int64_t balance(const Ledger &ledger, PlayerId player) {
  return player < ledger.balance.size() ? ledger.balance[player] : 0;
}

std::int64_t allowance(const Grants &grants, GrantId id, Step now) {
  const auto &grant = grants.rows[id];
  if (!grant.live || expired(grant, now))
    return 0;
  auto remaining = grant.cap - current_spent(grant, now);
  if (grant.parent) {
    const auto &parent = grants.rows[grant.parent];
    if (!parent.live || expired(parent, now))
      return 0;
    remaining = std::min(remaining, parent.cap - current_spent(parent, now));
  }
  return remaining;
}

GrantResult create_grant(Grants &grants, Players &players, const GrantRequest &request,
                         const ScopeControl &control, const HolderControl &holder, Step now) {
  if (request.issuer == 0 || request.issuer >= players.rows.size() || request.holder.id == 0 ||
      (request.holder.kind == HolderKind::Player && request.holder.id >= players.rows.size()))
    return {Reason::UnknownHolder};
  if (control.scope != request.scope || request.verbs == 0 ||
      (request.scope.kind == ScopeKind::Account &&
       (!request.scope.subject || request.scope.subject >= players.rows.size() ||
        control.controller != request.scope.subject)))
    return {Reason::InvalidScope};
  const Rights scope_mask = request.scope.kind == ScopeKind::Avatar ? Rights{3}
                            : request.scope.kind == ScopeKind::Plot
                                ? Rights{7} | bit(Verb::Transfer)
                            : monetary(request.scope) ? Rights{1}
                                                      : all_rights;
  if (request.verbs & ~scope_mask)
    return {Reason::InvalidScope};
  if ((request.verbs & control.available) != request.verbs)
    return {Reason::InheritedLimit};
  if (grants.by_key.contains({request.holder, request.scope}))
    return {Reason::Duplicate};
  if (request.holder.kind == HolderKind::Script &&
      (holder.holder != request.holder || !holder.installed || !request.holder.installed_on))
    return {Reason::InvalidScope};
  const bool needs_expiry = monetary(request.scope) || request.holder.kind == HolderKind::Script;
  if (needs_expiry && request.expires_step == 0)
    return {Reason::ExpiryRequired};
  if ((request.expires_step && request.expires_step <= now) || request.cap < 0 ||
      (monetary(request.scope) && request.period_steps == 0))
    return {Reason::InvalidScope};

  GrantId parent_id = 0;
  const bool controls = request.scope.kind == ScopeKind::Issuance
                            ? control.issuer_is_architect
                            : request.issuer == control.controller;
  if (!controls) {
    const Holder issuer = Holder::player(request.issuer);
    auto eligible = [&](const Grant *candidate) {
      return candidate && candidate->live && !expired(*candidate, now) && !candidate->parent &&
             covered(*candidate, control) && (request.verbs & candidate->verbs) == request.verbs &&
             request.holder.kind == HolderKind::Script && holder.controller == request.issuer &&
             (!monetary(request.scope) || request.cap <= candidate->cap) &&
             (!candidate->expires_step ||
              (request.expires_step && request.expires_step <= candidate->expires_step));
    };
    const Grant *parent = find(grants, issuer, request.scope);
    if (!eligible(parent) && request.scope.kind == ScopeKind::Object)
      parent = find(grants, issuer, Scope::owned_on_plot(control.controller, control.plot));
    if (!eligible(parent))
      return {Reason::InvalidDelegation};
    parent_id = parent->id;
  }
  const auto id = static_cast<GrantId>(grants.rows.size());
  grants.rows.push_back({id, request.issuer, parent_id, request.holder, request.scope,
                         request.verbs, request.cap, 0, request.period_steps, now,
                         request.expires_step, true});
  grants.by_key.emplace(GrantKey{request.holder, request.scope}, id);
  if (request.expires_step) {
    grants.expiring.emplace(request.expires_step, id);
    if (needs_expiry)
      grants.reminders.emplace(request.expires_step, id);
  }
  players.rows[request.issuer].grants_given.push_back(id);
  if (request.holder.kind == HolderKind::Player)
    players.rows[request.holder.id].grants_held.push_back(id);
  return {Reason::Allowed, id};
}

std::vector<GrantId> revoke(Grants &grants, Players &players, GrantId id) {
  if (id == 0 || id >= grants.rows.size() || !grants.rows[id].live)
    return {};
  return revoke_many(grants, players, std::span(&id, 1));
}

std::vector<GrantId> revoke_many(Grants &grants, Players &players, std::span<const GrantId> ids) {
  std::vector<bool> marked(grants.rows.size());
  for (auto id : ids)
    if (id != 0 && id < grants.rows.size())
      marked[id] = true;
  return remove_marked(grants, players, marked);
}

std::vector<GrantId> revoke_object(Grants &grants, Players &players, Handle object) {
  return revoke_objects(grants, players, std::span(&object, 1));
}

std::vector<GrantId> revoke_objects(Grants &grants, Players &players,
                                    std::span<const Handle> objects) {
  std::unordered_set<std::uint64_t> handles;
  handles.reserve(objects.size());
  for (auto handle : objects)
    handles.insert(handle.bits);
  std::vector<bool> marked(grants.rows.size());
  for (const auto &grant : grants.rows) {
    const bool scope =
        (grant.scope.kind == ScopeKind::Object || grant.scope.kind == ScopeKind::Avatar) &&
        handles.contains(grant.scope.subject);
    const bool installed =
        grant.holder.kind == HolderKind::Script && handles.contains(grant.holder.installed_on.bits);
    if (grant.live && (scope || installed))
      marked[grant.id] = true;
  }
  return remove_marked(grants, players, marked);
}

PlotTransferResult transfer_plot(Plots &plots, Grants &grants, Players &players, Holder actor,
                                 PlotId id, PlayerId recipient, Step now) {
  if (!id || id >= plots.rows.size() || plots.rows[id].id != id)
    return {Reason::InvalidScope, 0, {}};
  if (!recipient || recipient >= players.rows.size())
    return {Reason::UnknownHolder, 0, {}};
  auto &plot = plots.rows[id];
  const auto decision = check_land(actor, LandVerb::Transfer, plot, players, grants, now);
  if (!decision)
    return {decision.reason, 0, {}};
  const auto previous = plot.owner;
  if (previous == recipient)
    return {Reason::Allowed, previous, {}};
  std::vector<GrantId> scoped;
  for (const auto &grant : grants.rows)
    if (grant.live && grant.scope == Scope::plot(id))
      scoped.push_back(grant.id);
  auto revoked = revoke_many(grants, players, scoped);
  plot.owner = recipient;
  return {Reason::Allowed, previous, std::move(revoked)};
}

GrantDeadlines advance_grants(Grants &grants, Players &players, Step now, Step reminder_steps,
                              std::size_t max_entries) {
  GrantDeadlines result;
  std::size_t consumed = 0;
  if (!grants.expiring.empty() && grants.expiring.begin()->first <= now && max_entries) {
    std::vector<bool> marked(grants.rows.size());
    while (consumed < max_entries && !grants.expiring.empty() &&
           grants.expiring.begin()->first <= now) {
      const auto id = grants.expiring.begin()->second;
      grants.expiring.erase(grants.expiring.begin());
      marked[id] = grants.rows[id].live;
      ++consumed;
    }
    result.expired = erase_marked(grants, players, marked);
  }
  const auto reminder_end = now > std::numeric_limits<Step>::max() - reminder_steps
                                ? std::numeric_limits<Step>::max()
                                : now + reminder_steps;
  while (consumed < max_entries && !grants.reminders.empty() &&
         grants.reminders.begin()->first <= reminder_end) {
    const auto id = grants.reminders.begin()->second;
    grants.reminders.erase(grants.reminders.begin());
    const auto &grant = grants.rows[id];
    if (grant.live && !expired(grant, now))
      result.reminders.push_back(id);
    ++consumed;
  }
  return result;
}

void rebuild_grant_lookup(Grants &grants) {
  grants.by_key.clear();
  grants.expiring.clear();
  grants.reminders.clear();
  for (const auto &grant : grants.rows)
    if (grant.live) {
      if (grant.expires_step) {
        grants.expiring.emplace(grant.expires_step, grant.id);
        if (monetary(grant.scope) || grant.holder.kind == HolderKind::Script)
          grants.reminders.emplace(grant.expires_step, grant.id);
      }
      grants.by_key.emplace(GrantKey{grant.holder, grant.scope}, grant.id);
    }
}

TransferResult pay(Ledger &ledger, Grants &grants, Holder actor, PlayerId payer, PlayerId recipient,
                   std::int64_t amount, Step now) {
  if (!payer || payer >= ledger.balance.size() || !recipient || recipient >= ledger.balance.size())
    return {Reason::UnknownHolder};
  const auto permission = check_account(actor, payer, amount, grants, now);
  if (!permission)
    return {permission.reason};
  if (ledger.balance[payer] < amount)
    return {Reason::InsufficientBalance};
  if (payer == recipient || amount == 0)
    return {Reason::Allowed, 0};
  if (amount > std::numeric_limits<std::int64_t>::max() - ledger.balance[recipient])
    return {Reason::BalanceOverflow};
  spend(grants, permission.grant, amount, now);
  ledger.balance[payer] -= amount;
  ledger.balance[recipient] += amount;
  return {Reason::Allowed, amount};
}

TransferResult issue(Ledger &ledger, Grants &grants, Holder actor, PlayerId recipient,
                     std::int64_t amount, Step now) {
  if (!recipient || recipient >= ledger.balance.size())
    return {Reason::UnknownHolder};
  if (amount < 0)
    return {Reason::BadAmount};
  const auto permission = budget(grants, find(grants, actor, Scope::issuance()), amount, now);
  if (!permission)
    return {permission.reason};
  if (amount > std::numeric_limits<std::int64_t>::max() - ledger.balance[recipient])
    return {Reason::BalanceOverflow};
  spend(grants, permission.grant, amount, now);
  ledger.balance[recipient] += amount;
  return {Reason::Allowed, amount};
}

namespace {
void apply_owner(AuthorityWrite authority, OwnerLimits &limits, PermissionFlags flags,
                 const Target &target, PlayerId recipient) {
  if (target.owner == recipient)
    return;
  const auto ceiling = target.owner_limit & ((target.preset >> 16) & inherited_rights);
  limits.set(target.object.slot(), ceiling);
  const auto unrestricted = all_rights & ~inherited_rights;
  const auto allowed = unrestricted | ceiling;
  const auto everyone = (target.preset & all_rights) & allowed;
  const auto group = ((target.preset >> 8) & all_rights) & allowed;
  authority.owner[target.object.slot()] = recipient;
  authority.group[target.object.slot()] = 0;
  authority.preset[target.object.slot()] = everyone | (group << 8) | (allowed << 16);
  if (!(ceiling & bit(Verb::Copy)))
    flags.set(target.object.slot(), flags.get(target.object.slot()) | NoCopy);
}
} // namespace
std::vector<GrantId> transfer_owner(AuthorityWrite authority, OwnerLimits &limits,
                                    PermissionFlags flags, Grants &grants, Players &players,
                                    const Target &target, PlayerId recipient) {
  if (target.owner == recipient)
    return {};
  apply_owner(authority, limits, flags, target, recipient);
  return revoke_object(grants, players, target.object);
}

namespace {
Decision check_parts(const Target &root, std::span<const Target> parts, bool escrow = false) {
  for (const auto &part : parts) {
    if (part.owner != root.owner || (part.flags & Body))
      return {Reason::NotGoods};
    if (static_cast<bool>(part.flags & Escrow) != escrow)
      return {Reason::InEscrow};
    if (!(part.owner_limit & bit(Verb::Transfer)))
      return {Reason::InheritedLimit};
  }
  return {Reason::Allowed};
}
bool same_parts(const Offer &offer, std::span<const Target> parts) {
  return std::equal(offer.parts.begin(), offer.parts.end(), parts.begin(), parts.end(),
                    [](Handle old, const Target &now) { return old == now.object; });
}
std::vector<Handle> part_handles(std::span<const Target> parts) {
  std::vector<Handle> handles;
  handles.reserve(parts.size());
  for (const auto &part : parts)
    handles.push_back(part.object);
  return handles;
}
void escrow_parts(PermissionFlags flags, std::span<const Handle> parts, bool set) {
  for (auto handle : parts) {
    const auto previous = flags.get(handle.slot());
    flags.set(handle.slot(), set ? previous | Escrow : previous & ~Escrow);
  }
}
std::vector<GrantId> transfer_goods(AuthorityWrite authority, OwnerLimits &limits,
                                    PermissionFlags flags, Grants &grants, Players &players,
                                    const Target &root, std::span<const Target> parts,
                                    PlayerId recipient) {
  std::erase(players.rows[root.owner].mailbox, root.object);
  for (const auto &part : parts)
    std::erase(players.rows[part.owner].mailbox, part.object);
  if (root.owner == recipient)
    return {};
  std::vector<Handle> transferred{root.object};
  transferred.reserve(parts.size() + 1);
  apply_owner(authority, limits, flags, root, recipient);
  for (const auto &part : parts) {
    apply_owner(authority, limits, flags, part, recipient);
    transferred.push_back(part.object);
  }
  return revoke_objects(grants, players, transferred);
}
} // namespace

OfferResult offer(Offers &offers, PermissionFlags flags, Holder seller, const Target &goods,
                  const Players &players, const Grants &grants, std::int64_t price, PlayerId buyer,
                  Step expiry, bool placed, Step now, std::span<const Target> parts) {
  const auto permission = check(seller, Verb::Transfer, goods, players, grants, now);
  if (!permission)
    return {permission.reason, 0, {}};
  if (goods.flags & Body)
    return {Reason::NotGoods, 0, {}};
  if (goods.flags & Escrow)
    return {Reason::InEscrow, 0, {}};
  if (!placed || (goods.flags & Held))
    return {Reason::NotPlaced, 0, {}};
  if (price < 0)
    return {Reason::BadAmount, 0, {}};
  if (expiry != 0 && expiry <= now)
    return {Reason::Expired, 0, {}};
  if (buyer >= players.rows.size())
    return {Reason::UnknownHolder, 0, {}};
  const auto components = check_parts(goods, parts);
  if (!components)
    return {components.reason, 0, {}};
  const auto id = static_cast<OfferId>(offers.rows.size());
  offers.rows.push_back(
      {id, goods.owner, buyer, goods.object, price, expiry, 0, true, part_handles(parts)});
  if (expiry)
    offers.expiring.emplace(expiry, id);
  flags.set(goods.object.slot(), flags.get(goods.object.slot()) | Escrow);
  escrow_parts(flags, offers.rows.back().parts, true);
  return {Reason::Allowed, id, {{goods.object, {}, true}}};
}

OfferResult cancel_offer(Offers &offers, PermissionFlags flags, Holder actor, const Target &goods,
                         const Players &players, const Grants &grants, OfferId id, Step now,
                         std::span<const Target> parts) {
  if (id == 0 || id >= offers.rows.size() || !offers.rows[id].live || offers.rows[id].trade)
    return {Reason::OfferGone, 0, {}};
  auto &row = offers.rows[id];
  if (row.goods != goods.object || row.seller != goods.owner || !(goods.flags & Escrow) ||
      !same_parts(row, parts))
    return {Reason::StaleGoods, 0, {}};
  const auto permission = check(actor, Verb::Transfer, goods, players, grants, now);
  if (!permission)
    return {permission.reason, 0, {}};
  row.live = false;
  flags.set(goods.object.slot(), flags.get(goods.object.slot()) & ~Escrow);
  escrow_parts(flags, row.parts, false);
  return {Reason::Allowed, id, {{goods.object, {}, true}}};
}

void rebuild_offer_deadlines(Offers &offers) {
  offers.expiring.clear();
  for (const auto &row : offers.rows)
    if (row.id && row.live && !row.trade && row.expires_step)
      offers.expiring.emplace(row.expires_step, row.id);
}

std::vector<OfferId> advance_offers(Offers &offers, IdentityRead identity, PermissionFlags flags,
                                    Step now, std::size_t max_entries) {
  std::vector<OfferId> expired;
  for (std::size_t processed = 0;
       processed < max_entries && !offers.expiring.empty() && offers.expiring.begin()->first <= now;
       ++processed) {
    const auto [due, id] = *offers.expiring.begin();
    offers.expiring.erase(offers.expiring.begin());
    auto &row = offers.rows[id];
    if (!row.live || row.trade || row.expires_step != due)
      continue;
    row.live = false;
    if (identity.valid(row.goods))
      flags.set(row.goods.slot(), flags.get(row.goods.slot()) & ~Escrow);
    for (auto part : row.parts)
      if (identity.valid(part))
        flags.set(part.slot(), flags.get(part.slot()) & ~Escrow);
    expired.push_back(id);
  }
  return expired;
}

SwapResult accept(Offers &offers, Ledger &ledger, Grants &grants, Players &players,
                  AuthorityWrite authority, PermissionFlags flags, OwnerLimits &limits,
                  const Buyer &buyer, const Target &goods, OfferId id, std::size_t mailbox_max,
                  Step now, std::span<const Target> parts) {
  if (id == 0 || id >= offers.rows.size() || !offers.rows[id].live || offers.rows[id].trade)
    return {Reason::OfferGone, {}, {}};
  auto &row = offers.rows[id];
  if (row.goods != goods.object || row.seller != goods.owner || !(goods.flags & Escrow) ||
      !same_parts(row, parts))
    return {Reason::StaleGoods, {}, {}};
  if (row.expires_step && now >= row.expires_step)
    return {Reason::Expired, {}, {}};
  if (buyer.beneficiary == 0 || buyer.beneficiary >= players.rows.size())
    return {Reason::UnknownHolder, {}, {}};
  if (row.buyer && row.buyer != buyer.beneficiary)
    return {Reason::WrongBuyer, {}, {}};
  // Escrow's seller remains the owner; a newly narrowed next-owner limit must still apply.
  const auto permission =
      check(Holder::player(row.seller), Verb::Transfer, goods, players, grants, now);
  if (!permission)
    return {permission.reason, {}, {}};
  const auto components = check_parts(goods, parts, true);
  if (!components)
    return {components.reason, {}, {}};
  auto &recipient = players.rows[buyer.beneficiary];
  if (!buyer.present && recipient.mailbox.size() >= mailbox_max)
    return {Reason::MailboxFull, {}, {}};
  if (row.price) {
    const auto payment = pay(ledger, grants, buyer.actor, buyer.payer, row.seller, row.price, now);
    if (!payment)
      return {payment.reason, {}, {}};
  }
  auto revoked =
      transfer_goods(authority, limits, flags, grants, players, goods, parts, buyer.beneficiary);
  std::erase(players.rows[row.seller].inventory, goods.object);
  (buyer.present ? recipient.inventory : recipient.mailbox).push_back(goods.object);
  flags.set(goods.object.slot(), flags.get(goods.object.slot()) & ~Escrow);
  escrow_parts(flags, row.parts, false);
  row.live = false;
  return {Reason::Allowed,
          {{goods.object, buyer.present ? buyer.custodian : Handle{}, false}},
          std::move(revoked)};
}

SwapResult give(Players &players, Grants &grants, AuthorityWrite authority, PermissionFlags flags,
                OwnerLimits &limits, Holder sender, const Target &goods, PlayerId recipient,
                Handle body, bool present, std::size_t mailbox_max, Step now,
                std::span<const Target> parts) {
  const auto permission = check(sender, Verb::Transfer, goods, players, grants, now);
  if (!permission)
    return {permission.reason, {}, {}};
  if (goods.flags & Body)
    return {Reason::NotGoods, {}, {}};
  if (goods.flags & Escrow)
    return {Reason::InEscrow, {}, {}};
  const auto components = check_parts(goods, parts);
  if (!components)
    return {components.reason, {}, {}};
  if (recipient == 0 || recipient >= players.rows.size())
    return {Reason::UnknownHolder, {}, {}};
  auto &delivery = players.rows[recipient];
  if (!present && delivery.mailbox.size() >= mailbox_max)
    return {Reason::MailboxFull, {}, {}};
  auto revoked = transfer_goods(authority, limits, flags, grants, players, goods, parts, recipient);
  std::erase(players.rows[goods.owner].inventory, goods.object);
  (present ? delivery.inventory : delivery.mailbox).push_back(goods.object);
  return {Reason::Allowed, {{goods.object, present ? body : Handle{}, false}}, std::move(revoked)};
}

StoreGoodsPlan prepare_store_goods(const Players &players, const Grants &grants, Holder actor,
                                   const Target &goods, std::span<const Target> parts,
                                   PlayerId recipient, Handle body, bool present,
                                   std::size_t mailbox_max, Step now) {
  if (goods.flags & Body)
    return {Reason::NotGoods, {}, {}, 0, false};
  if (goods.flags & Escrow)
    return {Reason::InEscrow, {}, {}, 0, false};
  if (!recipient || recipient >= players.rows.size())
    return {Reason::UnknownHolder, {}, {}, 0, false};
  if (recipient != goods.owner)
    return {Reason::NotController, {}, {}, 0, false};
  auto permission = check(actor, Verb::Transfer, goods, players, grants, now);
  if (!permission)
    return {permission.reason, {}, {}, 0, false};
  auto components = check_parts(goods, parts);
  if (!components)
    return {components.reason, {}, {}, 0, false};
  const auto &mailbox = players.rows[recipient].mailbox;
  if (!present && mailbox.size() >= mailbox_max &&
      std::find(mailbox.begin(), mailbox.end(), goods.object) == mailbox.end())
    return {Reason::MailboxFull, {}, {}, 0, false};
  return {Reason::Allowed, goods.object, present ? body : Handle{}, recipient, present};
}
void commit_store_goods(Players &players, const StoreGoodsPlan &plan) {
  assert(plan);
  auto &player = players.rows[plan.recipient];
  std::erase(player.inventory, plan.goods);
  std::erase(player.mailbox, plan.goods);
  (plan.present ? player.inventory : player.mailbox).push_back(plan.goods);
}

std::vector<CustodyChange> deliver_mailbox(Players &players, PlayerId id, Handle body) {
  auto &player = players.rows[id];
  std::vector<CustodyChange> changes;
  changes.reserve(player.mailbox.size());
  for (auto object : player.mailbox) {
    player.inventory.push_back(object);
    changes.push_back({object, body, false});
  }
  player.mailbox.clear();
  return changes;
}

TradeResult open_trade(Trades &trades, const Players &players, PlayerId first, PlayerId second) {
  if (!first || !second || first >= players.rows.size() || second >= players.rows.size())
    return {Reason::UnknownHolder, 0, false, {}, {}};
  if (first == second)
    return {Reason::NotParticipant, 0, false, {}, {}};
  const auto id = static_cast<TradeId>(trades.rows.size());
  trades.rows.push_back({id, {first, second}, {}, true});
  return {Reason::Allowed, id, false, {}, {}};
}

TradeResult change_trade(Trades &trades, Offers &offers, PermissionFlags flags,
                         const Players &players, const Grants &grants, TradeId id, PlayerId actor,
                         const Target *goods, std::int64_t money, CustodyChange prior, Step now,
                         std::span<const Target> parts) {
  if (!id || id >= trades.rows.size() || !trades.rows[id].live)
    return {Reason::OfferGone, id, false, {}, {}};
  auto &trade = trades.rows[id];
  if (actor != trade.players[0] && actor != trade.players[1])
    return {Reason::NotParticipant, id, false, {}, {}};
  if (money < 0)
    return {Reason::BadAmount, id, false, {}, {}};
  const std::size_t side = actor == trade.players[0] ? 0 : 1;
  auto &previous = trade.sides[side];
  const Handle old_goods = previous.offer && offers.rows[previous.offer].live
                               ? offers.rows[previous.offer].goods
                               : Handle{};
  if (goods) {
    if (goods->owner != actor)
      return {Reason::NotController, id, false, {}, {}};
    const auto permission =
        check(Holder::player(actor), Verb::Transfer, *goods, players, grants, now);
    if (!permission)
      return {permission.reason, id, false, {}, {}};
    if (goods->flags & Body)
      return {Reason::NotGoods, id, false, {}, {}};
    if ((goods->flags & Escrow) && goods->object != old_goods)
      return {Reason::InEscrow, id, false, {}, {}};
    if (old_goods == goods->object && !same_parts(offers.rows[previous.offer], parts))
      return {Reason::StaleGoods, id, false, {}, {}};
    const auto components = check_parts(*goods, parts, goods->object == old_goods);
    if (!components)
      return {components.reason, id, false, {}, {}};
    assert(prior.goods == goods->object);
    if (goods->object == old_goods)
      prior = previous.prior_custody;
  }
  TradeResult result{Reason::Allowed, id, false, {}, {}};
  if (previous.offer) {
    offers.rows[previous.offer].live = false;
    if (old_goods && (!goods || old_goods != goods->object)) {
      flags.set(old_goods.slot(), flags.get(old_goods.slot()) & ~Escrow);
      escrow_parts(flags, offers.rows[previous.offer].parts, false);
      result.custody.push_back(previous.prior_custody);
    }
  }
  const auto offer_id = static_cast<OfferId>(offers.rows.size());
  // Direct-trade money is the amount this side gives, independent of a goods sale price.
  offers.rows.push_back({offer_id, actor, trade.players[1 - side], goods ? goods->object : Handle{},
                         0, 0, id, true, part_handles(parts)});
  previous = {offer_id, money, false, prior};
  trade.sides[1 - side].confirmed = false;
  if (goods) {
    flags.set(goods->object.slot(), flags.get(goods->object.slot()) | Escrow);
    escrow_parts(flags, offers.rows.back().parts, true);
    result.custody.push_back({goods->object, {}, prior.in_world});
  }
  return result;
}

std::vector<CustodyChange> invalidate_goods_offers(Offers &offers, Trades &trades,
                                                   [[maybe_unused]] IdentityRead identity,
                                                   PermissionFlags flags, Handle object) {
  std::vector<CustodyChange> custody;
  for (auto &row : offers.rows) {
    if (!row.live || (row.goods != object &&
                      std::find(row.parts.begin(), row.parts.end(), object) == row.parts.end()))
      continue;
    if (row.trade) {
      auto cancelled = cancel_trade(trades, offers, flags, row.trade, row.seller);
      custody.insert(custody.end(), cancelled.custody.begin(), cancelled.custody.end());
    } else {
      assert(identity.valid(row.goods));
      flags.set(row.goods.slot(), flags.get(row.goods.slot()) & ~Escrow);
      for (auto part : row.parts) {
        assert(identity.valid(part));
        flags.set(part.slot(), flags.get(part.slot()) & ~Escrow);
      }
      custody.push_back({row.goods, {}, true});
      row.live = false;
    }
  }
  return custody;
}

TradeResult confirm_trade(Trades &trades, Offers &offers, Ledger &ledger, Grants &grants,
                          Players &players, AuthorityWrite authority, PermissionFlags flags,
                          OwnerLimits &limits, TradeId id, PlayerId actor,
                          const std::array<Target, 2> &goods, const std::array<Handle, 2> &bodies,
                          Step now, const std::array<std::span<const Target>, 2> &parts) {
  if (!id || id >= trades.rows.size() || !trades.rows[id].live)
    return {Reason::OfferGone, id, false, {}, {}};
  auto &trade = trades.rows[id];
  if (actor != trade.players[0] && actor != trade.players[1])
    return {Reason::NotParticipant, id, false, {}, {}};
  if (!bodies[0] || !bodies[1])
    return {Reason::NotPresent, id, false, {}, {}};
  std::array<std::int64_t, 2> final_balance{};
  for (std::size_t side = 0; side != 2; ++side) {
    const auto &part = trade.sides[side];
    if (!part.offer || !offers.rows[part.offer].live)
      return {Reason::OfferGone, id, false, {}, {}};
    const auto &row = offers.rows[part.offer];
    if (row.goods) {
      if (row.goods != goods[side].object || goods[side].owner != trade.players[side] ||
          !(goods[side].flags & Escrow) || !same_parts(row, parts[side]))
        return {Reason::StaleGoods, id, false, {}, {}};
      const auto permission = check(Holder::player(trade.players[side]), Verb::Transfer,
                                    goods[side], players, grants, now);
      if (!permission)
        return {permission.reason, id, false, {}, {}};
      const auto components = check_parts(goods[side], parts[side], true);
      if (!components)
        return {components.reason, id, false, {}, {}};
    }
    const auto available = balance(ledger, trade.players[side]);
    if (available < part.money)
      return {Reason::InsufficientBalance, id, false, {}, {}};
    const auto remainder = available - part.money;
    const auto incoming = trade.sides[1 - side].money;
    if (incoming > std::numeric_limits<std::int64_t>::max() - remainder)
      return {Reason::BalanceOverflow, id, false, {}, {}};
    final_balance[side] = remainder + incoming;
  }
  const auto side = actor == trade.players[0] ? 0 : 1;
  trade.sides[side].confirmed = true;
  TradeResult result{Reason::Allowed, id, false, {}, {}};
  if (!trade.sides[1 - side].confirmed)
    return result;
  // Both players spend their own wallets. Two fixed final balances avoid a partial pair of pays.
  for (std::size_t index = 0; index != 2; ++index)
    ledger.balance[trade.players[index]] = final_balance[index];
  for (std::size_t index = 0; index != 2; ++index) {
    auto &row = offers.rows[trade.sides[index].offer];
    if (row.goods) {
      const auto recipient = trade.players[1 - index];
      auto revoked = transfer_goods(authority, limits, flags, grants, players, goods[index],
                                    parts[index], recipient);
      result.revoked.insert(result.revoked.end(), revoked.begin(), revoked.end());
      std::erase(players.rows[trade.players[index]].inventory, row.goods);
      players.rows[recipient].inventory.push_back(row.goods);
      flags.set(row.goods.slot(), flags.get(row.goods.slot()) & ~Escrow);
      escrow_parts(flags, row.parts, false);
      result.custody.push_back({row.goods, bodies[1 - index], false});
    }
    row.live = false;
  }
  trade.live = false;
  result.completed = true;
  return result;
}

TradeResult cancel_trade(Trades &trades, Offers &offers, PermissionFlags flags, TradeId id,
                         PlayerId actor) {
  if (!id || id >= trades.rows.size() || !trades.rows[id].live)
    return {Reason::OfferGone, id, false, {}, {}};
  auto &trade = trades.rows[id];
  if (actor != trade.players[0] && actor != trade.players[1])
    return {Reason::NotParticipant, id, false, {}, {}};
  TradeResult result{Reason::Allowed, id, false, {}, {}};
  for (auto &side : trade.sides) {
    side.confirmed = false;
    if (!side.offer)
      continue;
    auto &row = offers.rows[side.offer];
    if (!row.live)
      continue;
    if (row.goods) {
      flags.set(row.goods.slot(), flags.get(row.goods.slot()) & ~Escrow);
      escrow_parts(flags, row.parts, false);
      result.custody.push_back(side.prior_custody);
    }
    row.live = false;
  }
  trade.live = false;
  return result;
}

} // namespace amber::perm
