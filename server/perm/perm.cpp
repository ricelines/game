#include "perm.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace amber::perm {
std::vector<PlotHolder> plot_holders(const Plot &plot, const Grants &grants, Step now) {
  std::vector<PlotHolder> result;
  const auto append = [&](GrantId id) {
    const auto &grant = grants.rows[id];
    if (grant.expires_step && now >= grant.expires_step)
      return;
    Rights verbs = 0;
    if (grant.scope == Scope::plot(plot.id))
      verbs = grant.verbs;
    else if (plot.kind == PlotKind::Lot && !grant.issuer && grant.scope.kind == ScopeKind::AllLand)
      verbs = grant.verbs >> 8;
    if (!(verbs & ((Rights{1} << unsigned(LandVerb::Transfer)) |
                   (Rights{1} << unsigned(LandVerb::Return)))) ||
        (grant.holder.kind == HolderKind::Player && grant.holder.id == plot.owner))
      return;
    const auto found = std::find_if(result.begin(), result.end(), [&](const PlotHolder &row) {
      return row.holder == grant.holder;
    });
    if (found == result.end())
      result.push_back({grant.holder, verbs});
    else
      found->verbs |= verbs;
  };
  std::vector<GrantId> candidates = grants.all_land;
  if (const auto found = grants.by_plot.find(plot.id); found != grants.by_plot.end())
    candidates.insert(candidates.end(), found->second.begin(), found->second.end());
  std::sort(candidates.begin(), candidates.end());
  for (auto id : candidates)
    append(id);
  return result;
}
nlohmann::json plot_record(const Plots &plots, const Grants &grants, PlotId id,
                           float port_spacing_m, Step now, const Offers &offers, PlayerId caller) {
  using Json = nlohmann::json;
  const auto &plot = plots.rows.at(id);
  const auto &b = plot.bounds;
  const char *fronts = "none";
  for (const auto &way : plots.rows) {
    if (!way.id || way.kind == PlotKind::Lot)
      continue;
    const auto &w = way.bounds;
    const bool vertical =
        (b[0] == w[2] || b[2] == w[0]) && std::max(b[1], w[1]) < std::min(b[3], w[3]);
    const bool horizontal =
        (b[1] == w[3] || b[3] == w[1]) && std::max(b[0], w[0]) < std::min(b[2], w[2]);
    if (horizontal && way.kind == PlotKind::Street) {
      fronts = "street";
      break;
    }
    if (way.kind == PlotKind::Alley) {
      if (horizontal) {
        fronts = "back way";
      } else if (vertical && std::string_view(fronts) == "none") {
        fronts = "alley";
      }
    }
  }
  const auto port = std::int64_t(std::round((b[0] + b[2]) / (2 * port_spacing_m)));
  Json rights = Json::array();
  for (const auto &[held, verbs] : plot_holders(plot, grants, now)) {
    Json holder = held.kind == HolderKind::Player ? Json("p:" + std::to_string(held.id))
                                                  : Json{{"script", "s:" + std::to_string(held.id)},
                                                         {"on", to_string(held.installed_on)}};
    Json names = Json::array();
    constexpr const char *land_verbs[]{"build", "enter", "run_scripts", "transfer", "return"};
    for (unsigned bit = 0; bit < 5; ++bit)
      if (verbs & (1u << bit))
        names.push_back(land_verbs[bit]);
    rights.push_back({{"holder", std::move(holder)}, {"verbs", std::move(names)}});
  }
  constexpr const char *kinds[]{"street", "alley", "lot"};
  Json result = {{"plot", id},
                 {"kind", kinds[static_cast<unsigned>(plot.kind)]},
                 {"owner", "p:" + std::to_string(plot.owner)},
                 {"bounds", b},
                 {"open", bool(plot.preset & (Rights{1} << unsigned(LandVerb::Enter)))},
                 {"area_m2", (b[2] - b[0]) * (b[3] - b[1])},
                 {"fronts", fronts},
                 {"port", port},
                 {"port_x", port * port_spacing_m},
                 {"rights", std::move(rights)}};
  for (const auto &offer : offers.rows)
    if (offer.live && offer.plot == id && (!offer.expires_step || offer.expires_step > now) &&
        (!offer.buyer || offer.buyer == caller)) {
      result["for_sale"] = {
          {"offer", "offer:" + std::to_string(offer.id)},
          {"price", offer.price},
          {"buyer", offer.buyer ? Json("p:" + std::to_string(offer.buyer)) : Json(nullptr)}};
      break;
    }
  return result;
}
std::string_view required_right(std::string_view right, Caller caller, const perm::Target &target) {
  const auto *script = std::get_if<ScriptHolder>(&caller);
  return right == "move" && (target.flags & Body) && script && script->object != target.object
             ? "animate"
             : right;
}
Gate check_right(std::string_view right, Caller caller, const perm::Target &target,
                 const perm::Players &players, const perm::Grants &grants, Step now) {
  if (right.empty())
    return {true, {}};
  static constexpr std::array<std::string_view, 7> rights{"use",  "take",     "move",   "edit",
                                                          "copy", "transfer", "destroy"};
  const auto found = std::find(rights.begin(), rights.end(), right);
  if (found == rights.end())
    return {false, "unknown declared right"};
  const auto holder = std::visit(
      [](const auto &actor) -> perm::Holder {
        using T = std::decay_t<decltype(actor)>;
        if constexpr (std::is_same_v<T, PlayerId>)
          return perm::Holder::player(actor);
        else
          return perm::Holder::script(actor.script, actor.object);
      },
      caller);
  const auto decision =
      required_right(right, caller, target) == "animate"
          ? perm::check_scope(holder, perm::Scope::avatar(target.object), 1, grants, now)
          : perm::check(holder, static_cast<perm::Verb>(found - rights.begin()), target, players,
                        grants, now);
  return {bool(decision),
          decision ? std::string{} : std::string(perm::reason_text(decision.reason))};
}
const Plot *plot_at(const Plots &plots, Vec3 position) {
  for (const auto &plot : plots.rows)
    if (plot.id && position.x >= plot.bounds[0] && position.y >= plot.bounds[1] &&
        position.x < plot.bounds[2] && position.y < plot.bounds[3])
      return &plot;
  return nullptr;
}
std::vector<Handle> update_plots(Objects &objects, const Plots &plots,
                                 std::span<const Handle> moved, bool in_world) {
  const auto identity = amber::identity(objects);
  std::vector<Handle> changed;
  for (auto object : moved) {
    if (!identity.valid(object))
      continue;
    PlotId next = 0;
    if (in_world) {
      const auto point = objects.position[object.slot()];
      const auto current = objects.plot[object.slot()];
      if (current && current < plots.rows.size()) {
        const auto &plot = plots.rows[current];
        if (plot.id == current && point.x >= plot.bounds[0] && point.y >= plot.bounds[1] &&
            point.x < plot.bounds[2] && point.y < plot.bounds[3])
          continue;
      }
      if (const auto *plot = plot_at(plots, point))
        next = plot->id;
    }
    if (objects.plot[object.slot()] != next) {
      set_plot(objects, object.slot(), next);
      changed.push_back(object);
    }
  }
  return changed;
}
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
                                    "not present",
                                    "price changed"};
  static_assert(names.size() == static_cast<std::size_t>(Reason::PriceChanged) + 1);
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

template <class Map, class Key> void prune_grants(Map &map, const Key &key, const Grants &grants) {
  const auto found = map.find(key);
  if (found == map.end())
    return;
  std::erase_if(found->second, [&](GrantId id) { return !grants.rows[id].live; });
  if (found->second.empty())
    map.erase(found);
}
std::vector<GrantId> erase_grants(Grants &grants, Players &players, std::span<const GrantId> ids) {
  std::vector<GrantId> removed;
  std::set<PlayerId> people;
  std::map<std::uint64_t, std::set<ScriptId>> scripts;
  std::set<GrantId> parents;
  std::set<PlotId> plots;
  std::set<std::uint64_t> objects;
  bool all_land = false;
  for (auto id : ids) {
    auto &grant = grants.rows[id];
    if (!grant.live)
      continue;
    grants.by_key.erase({grant.holder, grant.scope});
    grants.expiring.erase({grant.expires_step, id});
    grants.reminders.erase({grant.expires_step, id});
    people.insert(grant.issuer);
    if (grant.holder.kind == HolderKind::Player)
      people.insert(grant.holder.id);
    else
      scripts[grant.holder.installed_on.bits].insert(grant.holder.id);
    if (grant.parent)
      parents.insert(grant.parent);
    switch (grant.scope.kind) {
    case ScopeKind::Plot:
    case ScopeKind::OwnedOnPlot:
      plots.insert(PlotId(grant.scope.subject));
      break;
    case ScopeKind::Object:
    case ScopeKind::Avatar:
      objects.insert(grant.scope.subject);
      break;
    case ScopeKind::Account:
      if (grant.scope.account_kind == AccountKind::Object)
        objects.insert(grant.scope.subject);
      break;
    case ScopeKind::AllLand:
      all_land = true;
      break;
    case ScopeKind::Owned:
      break;
    }
    grant.live = false;
    removed.push_back(id);
  }
  const auto dead = [&](GrantId id) { return !grants.rows[id].live; };
  for (auto person : people) {
    std::erase_if(players.rows[person].grants_given, dead);
    std::erase_if(players.rows[person].grants_held, dead);
  }
  for (const auto &[object, holders] : scripts) {
    auto found = grants.script_held.find(object);
    for (auto script : holders)
      prune_grants(found->second, script, grants);
    if (found->second.empty())
      grants.script_held.erase(found);
  }
  for (auto parent : parents)
    prune_grants(grants.children, parent, grants);
  for (auto plot : plots)
    prune_grants(grants.by_plot, plot, grants);
  for (auto object : objects)
    prune_grants(grants.by_object, object, grants);
  if (all_land)
    std::erase_if(grants.all_land, dead);
  return removed;
}

std::vector<GrantId> remove_grants(Grants &grants, Players &players,
                                   std::span<const GrantId> roots) {
  std::vector<GrantId> pending(roots.begin(), roots.end()), ids;
  std::unordered_set<GrantId> visited;
  for (std::size_t at = 0; at < pending.size(); ++at) {
    const auto id = pending[at];
    if (!id || id >= grants.rows.size() || !grants.rows[id].live || !visited.insert(id).second)
      continue;
    ids.push_back(id);
    if (const auto found = grants.children.find(id); found != grants.children.end())
      pending.insert(pending.end(), found->second.begin(), found->second.end());
  }
  std::sort(ids.begin(), ids.end());
  return erase_grants(grants, players, ids);
}

bool covered(const Grant &parent, const ScopeControl &control) {
  return parent.scope == control.scope ||
         (parent.scope.kind == ScopeKind::Owned && parent.scope.owner == control.controller &&
          (control.scope.kind == ScopeKind::Object ||
           control.scope.kind == ScopeKind::OwnedOnPlot)) ||
         (parent.scope.kind == ScopeKind::AllLand && control.on_lot &&
          (control.scope.kind == ScopeKind::Object || control.scope.kind == ScopeKind::Plot)) ||
         (parent.scope.kind == ScopeKind::OwnedOnPlot && control.scope.kind == ScopeKind::Object &&
          parent.scope.owner == control.controller && parent.scope.subject == control.plot);
}

bool monetary(Scope scope) {
  return scope.kind == ScopeKind::Account;
}
} // namespace

std::size_t GrantKeyHash::operator()(const GrantKey &key) const {
  return mix(key.holder.id | (std::uint64_t{static_cast<unsigned>(key.holder.kind)} << 32)) ^
         mix(key.holder.installed_on.bits) ^ mix(key.scope.subject) ^
         mix(key.scope.owner | (std::uint64_t{static_cast<unsigned>(key.scope.kind)} << 32)) ^
         mix(static_cast<unsigned>(key.scope.account_kind));
}

std::string account_name(Account account) {
  switch (account.kind) {
  case AccountKind::Player:
    return "p:" + std::to_string(account.subject);
  case AccountKind::Object:
    return to_string(Handle{account.subject});
  case AccountKind::Group:
    return "group:" + std::to_string(account.subject);
  }
  throw std::invalid_argument("unknown account kind");
}

Target target(IdentityRead identity, AuthorityRead authority, const OwnerLimits &limits,
              const Plots &plots, Handle h) {
  assert(identity.valid(h));
  const auto slot = h.slot();
  const auto *limit = limits.get(slot);
  return {h,
          authority.owner[slot],
          authority.group[slot],
          authority.preset[slot],
          authority.plot[slot],
          identity.flags[slot],
          limit ? *limit : inherited_rights,
          authority.plot[slot] && authority.plot[slot] < plots.rows.size() &&
              plots.rows[authority.plot[slot]].kind == PlotKind::Lot};
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
  const auto owned = usable(find(grants, actor, Scope::owned(target.owner)), required, now);
  if (owned)
    return owned;
  const auto plot =
      usable(find(grants, actor, Scope::owned_on_plot(target.owner, target.plot)), required, now);
  if (plot)
    return plot;
  const auto land =
      target.on_lot ? usable(find(grants, actor, Scope::all_land()), required, now) : Decision{};
  if (land)
    return land;
  if (direct.reason == Reason::Expired)
    return direct;
  if (owned.reason == Reason::Expired)
    return owned;
  return plot.reason == Reason::Expired ? plot : land;
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
  const bool preset_verb = verb <= LandVerb::RunScripts;
  if (preset_verb && (plot.preset & required))
    return {Reason::Allowed};
  if (preset_verb && actor.kind == HolderKind::Player && plot.group &&
      (plot.preset & (required << 8))) {
    const auto &groups = players.rows[actor.id].groups;
    if (std::binary_search(groups.begin(), groups.end(), plot.group))
      return {Reason::Allowed};
  }
  const auto direct = check_scope(actor, Scope::plot(plot.id), required, grants, now);
  if (direct || plot.kind != PlotKind::Lot)
    return direct;
  const auto land = check_scope(actor, Scope::all_land(), required << 8, grants, now);
  return land || direct.reason != Reason::Expired ? land : direct;
}

Decision check_account(Holder actor, AccountTarget target, std::int64_t amount,
                       const Grants &grants, Step now) {
  if (amount < 0)
    return {Reason::BadAmount};
  if (((!target.account.subject || !target.controller) && target.account != Account::player(0)) ||
      !actor.id)
    return {Reason::UnknownHolder};
  if (actor.kind == HolderKind::Player && actor.id == target.controller)
    return {Reason::Allowed};
  return budget(grants, find(grants, actor, Scope::account(target.account)), amount, now);
}

std::int64_t balance(const Ledger &ledger, Account account) {
  const auto row = ledger.balance.find(account);
  return row == ledger.balance.end() ? 0 : row->second;
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

namespace {
Reason validate_grant(const Grants &grants, const Players &players, const GrantRequest &request,
                      const HolderControl &holder, bool needs_expiry, Step now) {
  if (!request.holder.id ||
      (request.holder.kind == HolderKind::Player && request.holder.id >= players.rows.size()))
    return Reason::UnknownHolder;
  if (!request.verbs)
    return Reason::InvalidScope;
  const Rights scope_mask = request.scope.kind == ScopeKind::Avatar ? Rights{3}
                            : request.scope.kind == ScopeKind::Plot ? Rights{31}
                            : request.scope.kind == ScopeKind::AllLand
                                ? (all_rights | (Rights{31} << 8))
                            : monetary(request.scope) ? Rights{1}
                                                      : all_rights;
  if (request.verbs & ~scope_mask)
    return Reason::InvalidScope;
  if (grants.by_key.contains({request.holder, request.scope}))
    return Reason::Duplicate;
  if (request.holder.kind == HolderKind::Script &&
      (holder.holder != request.holder || !holder.installed || !request.holder.installed_on))
    return Reason::InvalidScope;
  if (needs_expiry && request.expires_step == 0)
    return Reason::ExpiryRequired;
  if ((request.expires_step && request.expires_step <= now) || request.cap < 0 ||
      (monetary(request.scope) && request.period_steps == 0))
    return Reason::InvalidScope;
  return Reason::Allowed;
}
void index_grant(Grants &grants, Players &players, const Grant &grant, bool needs_expiry) {
  players.rows[grant.issuer].grants_given.push_back(grant.id);
  if (grant.holder.kind == HolderKind::Player)
    players.rows[grant.holder.id].grants_held.push_back(grant.id);
  else
    grants.script_held[grant.holder.installed_on.bits][grant.holder.id].push_back(grant.id);
  if (grant.parent)
    grants.children[grant.parent].push_back(grant.id);
  switch (grant.scope.kind) {
  case ScopeKind::Plot:
  case ScopeKind::OwnedOnPlot:
    grants.by_plot[PlotId(grant.scope.subject)].push_back(grant.id);
    break;
  case ScopeKind::Object:
  case ScopeKind::Avatar:
    grants.by_object[grant.scope.subject].push_back(grant.id);
    break;
  case ScopeKind::Account:
    if (grant.scope.account_kind == AccountKind::Object)
      grants.by_object[grant.scope.subject].push_back(grant.id);
    break;
  case ScopeKind::AllLand:
    grants.all_land.push_back(grant.id);
    break;
  case ScopeKind::Owned:
    break;
  }
  grants.by_key.emplace(GrantKey{grant.holder, grant.scope}, grant.id);
  if (grant.expires_step) {
    grants.expiring.emplace(grant.expires_step, grant.id);
    if (needs_expiry)
      grants.reminders.emplace(grant.expires_step, grant.id);
  }
}
GrantResult add_grant(Grants &grants, Players &players, const GrantRequest &request,
                      GrantId parent_id, bool needs_expiry, Step now) {
  const auto id = static_cast<GrantId>(grants.rows.size());
  grants.rows.push_back({id, request.issuer, parent_id, request.holder, request.scope,
                         request.verbs, request.cap, 0, request.period_steps, now,
                         request.expires_step, true});
  index_grant(grants, players, grants.rows.back(), needs_expiry);
  return {Reason::Allowed, id};
}

} // namespace
GrantResult create_grant(Grants &grants, Players &players, Holder issuer,
                         const GrantRequest &request, const ScopeControl &control,
                         const HolderControl &holder, Step now) {
  if (request.issuer == 0 || request.issuer >= players.rows.size() || request.holder.id == 0 ||
      !issuer.id || (issuer.kind == HolderKind::Player && issuer.id != request.issuer) ||
      (issuer.kind == HolderKind::Script && !issuer.installed_on) ||
      (request.holder.kind == HolderKind::Player && request.holder.id >= players.rows.size()))
    return {Reason::UnknownHolder};
  if (request.scope.kind == ScopeKind::AllLand ||
      (request.scope.kind == ScopeKind::Owned && request.scope.owner != request.issuer) ||
      control.scope != request.scope || request.verbs == 0 ||
      (request.scope.kind == ScopeKind::Account &&
       (((request.scope.subject == 0 || !control.controller) &&
         request.scope.account() != Account::player(0)) ||
        (request.scope.account_kind == AccountKind::Player &&
         (request.scope.subject >= players.rows.size() ||
          control.controller != request.scope.subject)))))
    return {Reason::InvalidScope};
  const bool needs_expiry = monetary(request.scope) || request.holder.kind == HolderKind::Script ||
                            issuer.kind == HolderKind::Script;
  if (const auto why = validate_grant(grants, players, request, holder, needs_expiry, now);
      why != Reason::Allowed)
    return {why};
  if ((request.verbs & control.available) != request.verbs)
    return {Reason::InheritedLimit};

  GrantId parent_id = 0;
  const bool controls = issuer.kind == HolderKind::Player && request.issuer == control.controller;
  if (!controls) {
    auto eligible = [&](const Grant *candidate) {
      const auto required = candidate && candidate->scope.kind == ScopeKind::AllLand &&
                                    request.scope.kind == ScopeKind::Plot
                                ? request.verbs << 8
                                : request.verbs;
      return candidate && candidate->live && !expired(*candidate, now) && !candidate->parent &&
             covered(*candidate, control) && (required & candidate->verbs) == required &&
             (issuer.kind == HolderKind::Script ||
              (request.holder.kind == HolderKind::Script && holder.controller == request.issuer)) &&
             (!monetary(request.scope) || request.cap <= candidate->cap) &&
             (!candidate->expires_step ||
              (request.expires_step && request.expires_step <= candidate->expires_step));
    };
    const Grant *parent = find(grants, issuer, request.scope);
    if (!eligible(parent) && request.scope.kind == ScopeKind::Object)
      parent = find(grants, issuer, Scope::owned_on_plot(control.controller, control.plot));
    if (!eligible(parent) &&
        (request.scope.kind == ScopeKind::Object || request.scope.kind == ScopeKind::OwnedOnPlot))
      parent = find(grants, issuer, Scope::owned(control.controller));
    if (!eligible(parent))
      parent = find(grants, issuer, Scope::all_land());
    if (!eligible(parent))
      return {Reason::InvalidDelegation};
    parent_id = parent->id;
  }
  return add_grant(grants, players, request, parent_id, needs_expiry, now);
}

GrantResult create_root_grant(Grants &grants, Players &players, const GrantRequest &request,
                              const HolderControl &holder, Step now) {
  if (request.issuer != 0)
    return {Reason::UnknownHolder};
  const bool needs_expiry = monetary(request.scope) || request.holder.kind == HolderKind::Script;
  if (const auto why = validate_grant(grants, players, request, holder, needs_expiry, now);
      why != Reason::Allowed)
    return {why};
  return add_grant(grants, players, request, 0, needs_expiry, now);
}

std::vector<GrantId> revoke(Grants &grants, Players &players, GrantId id) {
  if (id == 0 || id >= grants.rows.size() || !grants.rows[id].live)
    return {};
  return revoke_many(grants, players, std::span(&id, 1));
}

std::vector<GrantId> revoke_many(Grants &grants, Players &players, std::span<const GrantId> ids) {
  return remove_grants(grants, players, ids);
}

std::vector<GrantId> revoke_object(Grants &grants, Players &players, Handle object) {
  return revoke_objects(grants, players, std::span(&object, 1));
}

std::vector<GrantId> revoke_objects(Grants &grants, Players &players,
                                    std::span<const Handle> objects) {
  std::vector<GrantId> ids;
  for (auto object : objects) {
    if (const auto found = grants.by_object.find(object.bits); found != grants.by_object.end())
      ids.insert(ids.end(), found->second.begin(), found->second.end());
    if (const auto found = grants.script_held.find(object.bits); found != grants.script_held.end())
      for (const auto &[script, held] : found->second)
        ids.insert(ids.end(), held.begin(), held.end());
  }
  return remove_grants(grants, players, ids);
}

GrantDeadlines advance_grants(Grants &grants, Players &players, Step now, Step reminder_steps,
                              std::size_t max_entries) {
  GrantDeadlines result;
  std::size_t consumed = 0;
  if (!grants.expiring.empty() && grants.expiring.begin()->first <= now && max_entries) {
    std::vector<GrantId> ids;
    while (consumed < max_entries && !grants.expiring.empty() &&
           grants.expiring.begin()->first <= now) {
      const auto id = grants.expiring.begin()->second;
      grants.expiring.erase(grants.expiring.begin());
      ids.push_back(id);
      ++consumed;
    }
    std::sort(ids.begin(), ids.end());
    result.expired = erase_grants(grants, players, ids);
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

std::span<const GrantId> held_grants(const Grants &grants, const Players &players, Holder holder) {
  if (holder.kind == HolderKind::Player)
    return players.rows[holder.id].grants_held;
  const auto object = grants.script_held.find(holder.installed_on.bits);
  if (object == grants.script_held.end())
    return {};
  const auto script = object->second.find(holder.id);
  return script == object->second.end() ? std::span<const GrantId>{} : script->second;
}
void rebuild_grant_lookup(Grants &grants, Players &players) {
  grants.by_key.clear();
  grants.script_held.clear();
  grants.children.clear();
  grants.by_plot.clear();
  grants.by_object.clear();
  grants.all_land.clear();
  grants.expiring.clear();
  grants.reminders.clear();
  for (auto &player : players.rows) {
    player.grants_held.clear();
    player.grants_given.clear();
  }
  for (const auto &grant : grants.rows)
    if (grant.live)
      index_grant(grants, players, grant,
                  monetary(grant.scope) || grant.holder.kind == HolderKind::Script || grant.parent);
}

TransferResult pay(Ledger &ledger, Grants &grants, Holder actor, AccountTarget payer,
                   Account recipient, std::int64_t amount, Step now) {
  const bool root_payer = payer.account == Account::player(0);
  const bool root_recipient = recipient == Account::player(0);
  if ((!payer.account.subject && !root_payer) || (!recipient.subject && !root_recipient))
    return {Reason::UnknownHolder};
  const auto permission = check_account(actor, payer, amount, grants, now);
  if (!permission)
    return {permission.reason};
  if (!root_payer && balance(ledger, payer.account) < amount)
    return {Reason::InsufficientBalance};
  if (payer.account == recipient || amount == 0)
    return {Reason::Allowed, 0};
  if (!root_recipient &&
      amount > std::numeric_limits<std::int64_t>::max() - balance(ledger, recipient))
    return {Reason::BalanceOverflow};
  spend(grants, permission.grant, amount, now);
  if (!root_payer)
    ledger.balance[payer.account] -= amount;
  if (!root_recipient)
    ledger.balance[recipient] += amount;
  return {Reason::Allowed, amount};
}

Reason can_refund_object_accounts(const Ledger &ledger, AuthorityRead authority,
                                  std::span<const Handle> objects) {
  std::map<PlayerId, std::int64_t> refunds;
  for (auto object : objects) {
    const auto amount = balance(ledger, Account::object(object));
    if (!amount)
      continue;
    const auto owner = authority.owner[object.slot()];
    auto &total = refunds[owner];
    if (amount > std::numeric_limits<std::int64_t>::max() - total)
      return Reason::BalanceOverflow;
    total += amount;
  }
  for (const auto &[owner, amount] : refunds)
    if (amount > std::numeric_limits<std::int64_t>::max() - balance(ledger, Account::player(owner)))
      return Reason::BalanceOverflow;
  return Reason::Allowed;
}

std::vector<AccountRefund> refund_object_accounts(Ledger &ledger, AuthorityRead authority,
                                                  std::span<const Handle> objects) {
  assert(can_refund_object_accounts(ledger, authority, objects) == Reason::Allowed);
  std::vector<AccountRefund> refunds;
  for (auto object : objects) {
    const auto account = Account::object(object);
    const auto row = ledger.balance.find(account);
    if (row == ledger.balance.end())
      continue;
    const auto owner = authority.owner[object.slot()];
    const auto amount = row->second;
    ledger.balance[Account::player(owner)] += amount;
    ledger.balance.erase(row);
    if (amount)
      refunds.push_back({object, owner, amount});
  }
  return refunds;
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

void close_offer(Offers &offers, Offer &row) {
  offers.expiring.erase({row.expires_step, row.id});
  row.live = false;
}
void close_plot_offers(Offers &offers, PlotId id) {
  for (auto &row : offers.rows)
    if (row.live && row.plot == id)
      close_offer(offers, row);
}

std::vector<GrantId> change_plot_owner(State &state, PlotId id, PlayerId owner) {
  auto &plot = state.plots.rows.at(id);
  std::vector<GrantId> ids;
  if (plot.owner)
    if (const auto found = state.grants.by_plot.find(id); found != state.grants.by_plot.end())
      for (auto grant_id : found->second)
        if (state.grants.rows[grant_id].issuer == plot.owner)
          ids.push_back(grant_id);
  auto removed = remove_grants(state.grants, state.players, ids);
  close_plot_offers(state.offers, id);
  plot.owner = owner;
  return removed;
}

PlotId split_plot(State &state, PlotId id, int axis, float cut, Step now) {
  auto &plot = state.plots.rows[id];
  auto upper = plot;
  upper.id = PlotId(state.plots.rows.size());
  const auto next = upper.id;
  plot.bounds[axis + 2] = cut;
  upper.bounds[axis] = cut;
  state.plots.rows.push_back(std::move(upper));
  std::vector<GrantId> originals;
  if (const auto found = state.grants.by_plot.find(id); found != state.grants.by_plot.end())
    originals = found->second;
  std::sort(originals.begin(), originals.end());
  std::map<GrantId, GrantId> copies;
  for (auto i : originals) {
    auto grant = state.grants.rows[i];
    if (grant.expires_step && grant.expires_step <= now)
      continue;
    grant.id = GrantId(state.grants.rows.size());
    copies.emplace(GrantId(i), grant.id);
    grant.scope.subject = next;
    if (copies.contains(grant.parent))
      grant.parent = copies.at(grant.parent);
    index_grant(state.grants, state.players, grant,
                monetary(grant.scope) || grant.holder.kind == HolderKind::Script || grant.parent);
    state.grants.rows.push_back(grant);
  }

  close_plot_offers(state.offers, id);
  return next;
}
std::vector<GrantId> join_plots(State &state, PlotId a, PlotId b) {
  auto &left = state.plots.rows[a];
  auto &right = state.plots.rows[b];
  const auto l = left.bounds, r = right.bounds;
  left.bounds = {std::min(l[0], r[0]), std::min(l[1], r[1]), std::max(l[2], r[2]),
                 std::max(l[3], r[3])};
  right.id = 0;
  const auto found = state.grants.by_plot.find(b);
  const auto removed = found == state.grants.by_plot.end()
                           ? std::vector<GrantId>{}
                           : revoke_many(state.grants, state.players, found->second);
  close_plot_offers(state.offers, a);
  close_plot_offers(state.offers, b);
  return removed;
}

OfferResult offer(amber::perm::State &perm_state, PermissionFlags flags, Holder seller,
                  const Target &goods, std::int64_t price, PlayerId buyer, Step expiry, bool placed,
                  Step now, std::span<const Target> parts) {
  auto &offers = perm_state.offers;
  auto &players = perm_state.players;
  auto &grants = perm_state.grants;

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
  offers.rows.push_back({id, goods.owner, buyer, goods.object, price, expiry, 0, true,
                         part_handles(parts), 0, seller});
  if (expiry)
    offers.expiring.emplace(expiry, id);
  flags.set(goods.object.slot(), flags.get(goods.object.slot()) | Escrow);
  escrow_parts(flags, offers.rows.back().parts, true);
  return {Reason::Allowed, id, {{goods.object, {}, true}}};
}

OfferResult offer_plot(State &state, Holder actor, PlotId id, std::int64_t price, PlayerId buyer,
                       Step expiry, Step now) {
  if (!id || id >= state.plots.rows.size() || state.plots.rows[id].id != id ||
      state.plots.rows[id].kind != PlotKind::Lot)
    return {Reason::InvalidScope, 0, {}};
  const auto &plot = state.plots.rows[id];
  const auto permission =
      check_land(actor, LandVerb::Transfer, plot, state.players, state.grants, now);
  if (!permission)
    return {permission.reason, 0, {}};
  if (price < 0)
    return {Reason::BadAmount, 0, {}};
  if (expiry && expiry <= now)
    return {Reason::Expired, 0, {}};
  if (buyer >= state.players.rows.size())
    return {Reason::UnknownHolder, 0, {}};
  for (const auto &row : state.offers.rows)
    if (row.live && row.plot == id && (!row.expires_step || row.expires_step > now))
      return {Reason::Duplicate, 0, {}};
  const auto offer = OfferId(state.offers.rows.size());
  state.offers.rows.push_back(
      {offer, plot.owner, buyer, {}, price, expiry, 0, true, {}, id, actor});
  if (expiry)
    state.offers.expiring.emplace(expiry, offer);
  return {Reason::Allowed, offer, {}};
}
Reason set_offer_price(Offers &offers, Holder actor, OfferId id, std::int64_t price, Step now) {
  if (!id || id >= offers.rows.size() || !offers.rows[id].live || offers.rows[id].trade)
    return Reason::OfferGone;
  auto &row = offers.rows[id];
  if (row.expires_step && now >= row.expires_step)
    return Reason::Expired;
  if (actor != row.maker && actor != Holder::player(row.seller))
    return Reason::NotController;
  if (price < 0)
    return Reason::BadAmount;
  row.price = price;
  return Reason::Allowed;
}
SwapResult accept_plot(State &state, const Buyer &buyer, OfferId id, std::int64_t max_price,
                       Step now) {
  if (!id || id >= state.offers.rows.size() || !state.offers.rows[id].live ||
      state.offers.rows[id].trade)
    return {Reason::OfferGone, {}, {}};
  auto &row = state.offers.rows[id];
  if (row.expires_step && now >= row.expires_step)
    return {Reason::Expired, {}, {}};
  if (!buyer.beneficiary || buyer.beneficiary >= state.players.rows.size())
    return {Reason::UnknownHolder, {}, {}};
  if (row.buyer && row.buyer != buyer.beneficiary)
    return {Reason::WrongBuyer, {}, {}};
  if (row.price > max_price)
    return {Reason::PriceChanged, {}, {}};
  if (!row.plot || row.plot >= state.plots.rows.size())
    return {Reason::InvalidScope, {}, {}};
  const auto &plot = state.plots.rows[row.plot];
  if (plot.id != row.plot || plot.owner != row.seller)
    return {Reason::StaleGoods, {}, {}};
  const auto permission =
      check_land(row.maker, LandVerb::Transfer, plot, state.players, state.grants, now);
  if (!permission)
    return {permission.reason, {}, {}};
  if (row.price) {
    const auto payment = pay(state.ledger, state.grants, buyer.actor, buyer.payer,
                             Account::player(row.seller), row.price, now);
    if (!payment)
      return {payment.reason, {}, {}};
  }
  auto revoked = change_plot_owner(state, row.plot, buyer.beneficiary);
  return {Reason::Allowed, {}, std::move(revoked)};
}

OfferResult cancel_offer(amber::perm::State &perm_state, PermissionFlags flags, Holder actor,
                         const Target &goods, OfferId id, Step now, std::span<const Target> parts) {
  auto &offers = perm_state.offers;
  auto &players = perm_state.players;
  auto &grants = perm_state.grants;

  if (id == 0 || id >= offers.rows.size() || !offers.rows[id].live || offers.rows[id].trade)
    return {Reason::OfferGone, 0, {}};
  auto &row = offers.rows[id];
  if (row.goods != goods.object || row.seller != goods.owner || !(goods.flags & Escrow) ||
      !same_parts(row, parts))
    return {Reason::StaleGoods, 0, {}};
  const auto permission = check(actor, Verb::Transfer, goods, players, grants, now);
  if (!permission)
    return {permission.reason, 0, {}};
  close_offer(offers, row);
  flags.set(goods.object.slot(), flags.get(goods.object.slot()) & ~Escrow);
  escrow_parts(flags, row.parts, false);
  return {Reason::Allowed, id, {}};
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
    close_offer(offers, row);
    if (identity.valid(row.goods))
      flags.set(row.goods.slot(), flags.get(row.goods.slot()) & ~Escrow);
    for (auto part : row.parts)
      if (identity.valid(part))
        flags.set(part.slot(), flags.get(part.slot()) & ~Escrow);
    expired.push_back(id);
  }
  return expired;
}

SwapResult accept(amber::perm::State &perm_state, amber::ObjectState &objects_state,
                  const Buyer &buyer, const Target &goods, OfferId id, std::int64_t max_price,
                  std::size_t mailbox_max, Step now, std::span<const Target> parts) {
  auto &offers = perm_state.offers;
  auto &ledger = perm_state.ledger;
  auto &grants = perm_state.grants;
  auto &players = perm_state.players;
  auto authority = amber::authority(objects_state.objects);
  auto flags = amber::permission_flags(objects_state.objects);
  auto &limits = perm_state.owner_limits;

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
  if (row.price > max_price)
    return {Reason::PriceChanged, {}, {}};
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
    const auto payment =
        pay(ledger, grants, buyer.actor, buyer.payer, Account::player(row.seller), row.price, now);
    if (!payment)
      return {payment.reason, {}, {}};
  }
  auto revoked =
      transfer_goods(authority, limits, flags, grants, players, goods, parts, buyer.beneficiary);
  std::erase(players.rows[row.seller].inventory, goods.object);
  (buyer.present ? recipient.inventory : recipient.mailbox).push_back(goods.object);
  flags.set(goods.object.slot(), flags.get(goods.object.slot()) & ~Escrow);
  escrow_parts(flags, row.parts, false);
  close_offer(offers, row);
  return {Reason::Allowed,
          {{goods.object, buyer.present ? buyer.custodian : Handle{}, false}},
          std::move(revoked)};
}

SwapResult give(amber::perm::State &perm_state, amber::ObjectState &objects_state, Holder sender,
                const Target &goods, PlayerId recipient, Handle body, bool present,
                std::size_t mailbox_max, Step now, std::span<const Target> parts) {
  auto &players = perm_state.players;
  auto &grants = perm_state.grants;
  auto authority = amber::authority(objects_state.objects);
  auto flags = amber::permission_flags(objects_state.objects);
  auto &limits = perm_state.owner_limits;

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

SwapResult return_goods(State &state, const ReturnRequest &request, Step now) {
  const auto &goods = request.goods;
  if (!request.forced) {
    if (goods.flags & Body)
      return {Reason::NotGoods, {}, {}};
    if (!request.plot || request.plot >= state.plots.rows.size() ||
        state.plots.rows[request.plot].id != request.plot || goods.plot != request.plot)
      return {Reason::InvalidScope, {}, {}};
    if (request.actor == Holder::player(goods.owner))
      return {Reason::NotController, {}, {}};
    if (usable(find(state.grants, Holder::player(goods.owner), Scope::plot(request.plot)), 1, now))
      return {Reason::MissingRight, {}, {}};
    const auto allowed = check_land(request.actor, LandVerb::Return, state.plots.rows[request.plot],
                                    state.players, state.grants, now);
    if (!allowed)
      return {allowed.reason, {}, {}};
  }
  std::vector<OfferId> closed;
  for (auto &offer : state.offers.rows)
    if (offer.live && offer.goods == goods.object) {
      if (offer.trade) {
        auto &trade = state.trades.rows[offer.trade];
        // Other trade goods retain custody; the adapter clears their escrow flags.
        for (auto &side : trade.sides)
          if (side.offer && state.offers.rows[side.offer].live) {
            close_offer(state.offers, state.offers.rows[side.offer]);
            closed.push_back(side.offer);
          }
        trade.live = false;
      } else {
        close_offer(state.offers, offer);
        closed.push_back(offer.id);
      }
    }
  auto &owner = state.players.rows.at(goods.owner);
  std::erase(owner.inventory, goods.object);
  std::erase(owner.mailbox, goods.object);
  (request.present ? owner.inventory : owner.mailbox).push_back(goods.object);
  return {Reason::Allowed,
          {{goods.object, request.present ? request.owner_body : Handle{}, false, true}},
          {},
          std::move(closed)};
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

TradeResult change_trade(amber::perm::State &perm_state, PermissionFlags flags, TradeId id,
                         PlayerId actor, const Target *goods, std::int64_t money, bool in_world,
                         Step now, std::span<const Target> parts) {
  auto &trades = perm_state.trades;
  auto &offers = perm_state.offers;
  auto &players = perm_state.players;
  auto &grants = perm_state.grants;

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
  }
  TradeResult result{Reason::Allowed, id, false, {}, {}};
  if (previous.offer) {
    close_offer(offers, offers.rows[previous.offer]);
    if (old_goods && (!goods || old_goods != goods->object)) {
      flags.set(old_goods.slot(), flags.get(old_goods.slot()) & ~Escrow);
      escrow_parts(flags, offers.rows[previous.offer].parts, false);
    }
  }
  const auto offer_id = static_cast<OfferId>(offers.rows.size());
  // Direct-trade money is the amount this side gives, independent of a goods sale price.
  offers.rows.push_back({offer_id, actor, trade.players[1 - side], goods ? goods->object : Handle{},
                         0, 0, id, true, part_handles(parts), 0, Holder::player(actor)});
  previous = {offer_id, money, false};
  trade.sides[1 - side].confirmed = false;
  if (goods) {
    flags.set(goods->object.slot(), flags.get(goods->object.slot()) | Escrow);
    escrow_parts(flags, offers.rows.back().parts, true);
    if (goods->object != old_goods)
      result.custody.push_back({goods->object, {}, in_world});
  }
  return result;
}

std::vector<Handle> invalidate_goods_offers(Offers &offers, Trades &trades, IdentityRead identity,
                                            PermissionFlags flags, Handle object) {
  std::vector<Handle> changed;
  for (auto &row : offers.rows) {
    if (!row.live || (row.goods != object &&
                      std::find(row.parts.begin(), row.parts.end(), object) == row.parts.end()))
      continue;
    if (row.trade) {
      for (const auto &side : trades.rows[row.trade].sides)
        if (side.offer && offers.rows[side.offer].live && offers.rows[side.offer].goods)
          changed.push_back(offers.rows[side.offer].goods);
      cancel_trade(trades, offers, flags, row.trade, row.seller);
    } else {
      if (identity.valid(row.goods))
        flags.set(row.goods.slot(), flags.get(row.goods.slot()) & ~Escrow);
      for (auto part : row.parts)
        if (identity.valid(part))
          flags.set(part.slot(), flags.get(part.slot()) & ~Escrow);
      changed.push_back(row.goods);
      close_offer(offers, row);
    }
  }
  return changed;
}

TradeResult confirm_trade(amber::perm::State &perm_state, amber::ObjectState &objects_state,
                          TradeId id, PlayerId actor, const std::array<Target, 2> &goods,
                          const std::array<Handle, 2> &bodies, Step now,
                          const std::array<std::span<const Target>, 2> &parts) {
  auto &trades = perm_state.trades;
  auto &offers = perm_state.offers;
  auto &ledger = perm_state.ledger;
  auto &grants = perm_state.grants;
  auto &players = perm_state.players;
  auto authority = amber::authority(objects_state.objects);
  auto flags = amber::permission_flags(objects_state.objects);
  auto &limits = perm_state.owner_limits;

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
    const auto available = balance(ledger, Account::player(trade.players[side]));
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
    ledger.balance[Account::player(trade.players[index])] = final_balance[index];
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
    close_offer(offers, row);
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
    }
    close_offer(offers, row);
  }
  trade.live = false;
  return result;
}

} // namespace amber::perm
