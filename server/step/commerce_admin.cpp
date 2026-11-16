#include "step.hpp"

#include <algorithm>
#include <limits>

namespace amber::step {
namespace {
PlayerId player(const step::NativeCall &call, const perm::Players &players) {
  const auto *id = std::get_if<PlayerId>(&call.request.caller);
  if (!id || !*id || *id >= players.rows.size())
    throw std::invalid_argument("this administration action requires a current player caller");
  return *id;
}
PlayerId recipient(const Json &value, const perm::Players &players) {
  const auto id = step::identifier(value, "p:");
  if (id >= players.rows.size())
    throw std::invalid_argument("unknown player");
  return id;
}
perm::Holder holder(const Json &row, const perm::Players &players) {
  if (row.is_string())
    return perm::Holder::player(recipient(row, players));
  const auto parsed = parse_handle(row.at("on").get<std::string>());
  if (!parsed)
    throw std::invalid_argument("script holder needs a full object handle");
  return perm::Holder::script(step::identifier(row.at("script"), "s:"), *parsed);
}
perm::Holder actor(const step::NativeCall &call) {
  if (const auto *player = std::get_if<PlayerId>(&call.request.caller))
    return perm::Holder::player(*player);
  const auto &script = std::get<ScriptHolder>(call.request.caller);
  return perm::Holder::script(script.script, script.object);
}
perm::HolderControl installed_holder(perm::Holder selected, const perm::Players &players,
                                     const amber::ObjectState &objects_state,
                                     const amber::catalog::State &catalog_state,
                                     const amber::react::State &react_state,
                                     const step::NativeCall &call) {
  const auto values = amber::react::value_read(objects_state, catalog_state, react_state);

  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto &models = catalog_state.models;
  auto &scripts = catalog_state.scripts;
  auto &instances = react_state.installations;
  auto &rules = react_state.rules;

  if (selected.kind == perm::HolderKind::Player) {
    if (!selected.id || selected.id >= players.rows.size())
      throw std::invalid_argument("unknown current player holder");
    return {selected, selected.id, true};
  }
  const auto object = selected.installed_on;
  if (!identity.valid(object) || selected.id >= scripts.rows.size() ||
      scripts.rows[selected.id].id != selected.id)
    throw std::invalid_argument("unknown current script holder");
  auto installed = react::installed_scripts(models, instances, identity, object);
  const auto applicable_rules = react::rule_scripts(rules, values, object, call.step, call.hz);
  installed.insert(installed.end(), applicable_rules.begin(), applicable_rules.end());
  return {selected, authority.owner[object.slot()],
          std::find(installed.begin(), installed.end(), selected.id) != installed.end()};
}
step::NativeCompletion result(bool changed, Json fields = Json::object()) {
  step::NativeCompletion out;
  out.changed = changed;
  out.permissions_changed = changed;
  out.result = changed ? record::Result::Applied : record::Result::NoEffect;
  out.reply = {{"ok", true}, {"effect", changed ? "applied" : "none"}};
  out.reply.update(fields);
  return out;
}
step::NativeCompletion denied(std::string_view need, Json on, std::string_view why,
                              std::string_view fix) {
  step::NativeCompletion out;
  out.result = record::Result::Denied;
  out.reply = {{"ok", false},
               {"denied", {{"need", need}, {"on", std::move(on)}, {"why", why}, {"fix", fix}}}};
  return out;
}
step::NativeCompletion bad_input(std::string_view problem) {
  step::NativeCompletion out;
  out.result = record::Result::Denied;
  out.reply = {{"ok", false}, {"error", "bad_input"}, {"problem", problem}};
  return out;
}
std::string_view grant_fix(perm::Reason reason) {
  switch (reason) {
  case perm::Reason::InheritedLimit:
    return "narrow the requested rights to the inherited ceiling";
  case perm::Reason::ExpiryRequired:
  case perm::Reason::Expired:
    return "set a future expires_s within the parent grant's lifetime";
  case perm::Reason::Duplicate:
    return "revoke the existing grant for this holder and scope before issuing another";
  case perm::Reason::UnknownHolder:
    return "choose a current player or a script installed on the specified object";
  case perm::Reason::InvalidDelegation:
    return "delegate once from a root grant, narrowing its scope, rights, cap and expiry";
  default:
    return "obtain a root grant for these rights and scope, or have their controller issue it";
  }
}
Json holder_json(perm::Holder holder) {
  if (holder.kind == perm::HolderKind::Player)
    return "p:" + std::to_string(holder.id);
  return {{"script", "s:" + std::to_string(holder.id)}, {"on", to_string(holder.installed_on)}};
}
Json scope_json(perm::Scope scope) {
  switch (scope.kind) {
  case perm::ScopeKind::Object:
    return {{"object", to_string(Handle{scope.subject})}};
  case perm::ScopeKind::Owned:
    return {{"owned", "p:" + std::to_string(scope.owner)}};
  case perm::ScopeKind::AllLand:
    return {{"all_land", true}};
  case perm::ScopeKind::OwnedOnPlot:
    return {{"owned_on_plot",
             {{"owner", "p:" + std::to_string(scope.owner)}, {"plot", scope.subject}}}};
  case perm::ScopeKind::Account:
    return {{"account", perm::account_name(scope.account())}};
  case perm::ScopeKind::Avatar:
    return {{"avatar", to_string(Handle{scope.subject})}};
  case perm::ScopeKind::Plot:
    return {{"plot", scope.subject}};
  }
  throw std::logic_error("unknown stored grant scope");
}
Json rights_json(perm::Rights rights, perm::ScopeKind scope) {
  Json result = Json::array();
  if (scope == perm::ScopeKind::Account) {
    if (rights & 1)
      result.push_back("spend");
  } else if (scope == perm::ScopeKind::Avatar) {
    if (rights & 1)
      result.push_back("animate");
    if (rights & 2)
      result.push_back("camera");
  } else if (scope == perm::ScopeKind::AllLand) {
    result = rights_json(rights & perm::all_rights, perm::ScopeKind::Object);
    for (const auto &name : rights_json(rights >> 8, perm::ScopeKind::Plot))
      result.push_back("land_" + name.get<std::string>());
  } else if (scope == perm::ScopeKind::Plot) {
    constexpr std::array names{"build", "enter", "run_scripts", "transfer", "return"};
    for (std::size_t index = 0; index < names.size(); ++index)
      if (rights & (perm::Rights{1} << index))
        result.push_back(names[index]);
  } else {
    constexpr std::array names{"use", "take", "move", "edit", "copy", "transfer", "destroy"};
    for (std::size_t index = 0; index < names.size(); ++index)
      if (rights & (perm::Rights{1} << index))
        result.push_back(names[index]);
  }
  return result;
}
bool in_force(const perm::Grants &grants, const perm::Grant &grant, Step step) {
  if (!grant.live || (grant.expires_step && step >= grant.expires_step))
    return false;
  if (!grant.parent)
    return true;
  const auto &parent = grants.rows[grant.parent];
  return parent.live && (!parent.expires_step || step < parent.expires_step);
}
Json grant_json(const perm::Grants &grants, const perm::Grant &grant, Step step, double hz) {
  const bool monetary = grant.scope.kind == perm::ScopeKind::Account;
  return {
      {"id", "g:" + std::to_string(grant.id)},
      {"issuer", "p:" + std::to_string(grant.issuer)},
      {"holder", holder_json(grant.holder)},
      {"scope", scope_json(grant.scope)},
      {"rights", rights_json(grant.verbs, grant.scope.kind)},
      {"parent", grant.parent ? Json("g:" + std::to_string(grant.parent)) : Json(nullptr)},
      {"cap", monetary ? Json(grant.cap) : Json(nullptr)},
      {"remaining", monetary ? Json(perm::allowance(grants, grant.id, step)) : Json(nullptr)},
      {"period_s", grant.period_steps / hz},
      {"expires_step", grant.expires_step ? Json(grant.expires_step) : Json(nullptr)},
      {"expires_in_s", grant.expires_step
                           ? Json((grant.expires_step > step ? grant.expires_step - step : 0) / hz)
                           : Json(nullptr)}};
}
GroupId group(const Json &value, const perm::Groups &groups) {
  if (value.is_null())
    return 0;
  const auto id = step::identifier(value, "group:");
  if (id >= groups.rows.size() || groups.rows[id].id != id)
    throw std::invalid_argument("unknown group");
  return id;
}
bool preset_rights(std::uint32_t &preset, const Json &arguments, std::string_view name,
                   unsigned shift, perm::ScopeKind scope, perm::Rights allowed) {
  if (!arguments.contains(name))
    return true;
  const auto rights = step::rights(arguments.at(name), scope);
  if (rights & ~allowed)
    return false;
  const auto mask = (scope == perm::ScopeKind::Plot ? perm::Rights{7} : perm::all_rights) << shift;
  preset = (preset & ~mask) | (rights << shift);
  return true;
}
Json preset_json(std::uint32_t preset, GroupId group, bool plot) {
  const auto scope = plot ? perm::ScopeKind::Plot : perm::ScopeKind::Object;
  const auto mask = plot ? perm::Rights{7} : perm::all_rights;
  Json row = {{"everyone", rights_json(preset & mask, scope)},
              {"group_rights", rights_json((preset >> 8) & mask, scope)},
              {"group", group ? Json("group:" + std::to_string(group)) : Json(nullptr)}};
  if (!plot)
    row["next_owner"] = rights_json((preset >> 16) & mask, scope);
  return row;
}
} // namespace
GrantUpdates advance_grants(perm::Grants &grants, perm::Players &players, Step now,
                            Step reminder_steps, double hz, std::size_t max_entries) {
  const auto due = perm::advance_grants(grants, players, now, reminder_steps, max_entries);
  GrantUpdates updates;
  const auto publish = [&](GrantId id, bool expired) {
    const auto &grant = grants.rows[id];
    auto details = grant_json(grants, grant, now, hz);
    details["kind"] = expired ? "grant_expired" : "grant_expiring";
    details["say"] = expired ? "The grant has expired."
                             : "This grant expires soon; renew it if it is still needed.";
    if (grant.issuer)
      updates.notices.push_back({grant.issuer, details});
    if (grant.parent) {
      const auto owner = grants.rows[grant.parent].issuer;
      if (owner && owner != grant.issuer)
        updates.notices.push_back({owner, details});
    }
    record::Event event;
    event.step = now;
    event.kind = record::Kind::System;
    event.name = expired ? "grant_expired" : "grant_expiring";
    event.changed = expired;
    event.result = expired ? record::Result::Applied : record::Result::NoEffect;
    event.detail = std::move(details);
    event.detail["grant"] = event.detail.at("id");
    event.detail.erase("id");
    event.detail.erase("kind");
    event.detail["agent"] = false;
    if (grant.scope.kind == perm::ScopeKind::Object || grant.scope.kind == perm::ScopeKind::Avatar)
      event.things.push_back(Handle{grant.scope.subject});
    updates.records.push_back(std::move(event));
  };
  for (auto id : due.reminders)
    publish(id, false);
  for (auto id : due.expired)
    publish(id, true);
  return updates;
}
step::NativeCompletion grant(amber::perm::State &perm_state,
                             const amber::ObjectState &objects_state,
                             const amber::catalog::State &catalog_state,
                             const amber::react::State &react_state, const step::NativeCall &call) {
  auto &grants = perm_state.grants;
  auto &players = perm_state.players;
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto &limits = perm_state.owner_limits;
  auto &plots = perm_state.plots;
  auto &groups = perm_state.groups;

  try {
    const auto issuer =
        installed_holder(actor(call), players, objects_state, catalog_state, react_state, call);
    if (!issuer.installed)
      return denied("installation", holder_json(issuer.holder),
                    "caller script is not currently installed",
                    "install the script on that object before invoking its actions");
    const auto who = issuer.controller;
    const auto control = step::scope(call.arguments.at("scope"), identity, authority, limits,
                                     players, plots, groups);
    const auto recipient = installed_holder(holder(call.arguments.at("holder"), players), players,
                                            objects_state, catalog_state, react_state, call);
    const auto duration = step::duration_steps(call.arguments.value("expires_s", 0.0), call.hz);
    if (duration > std::numeric_limits<Step>::max() - call.step)
      throw std::invalid_argument("grant expiry exceeds world step range");
    const perm::GrantRequest request{
        who,
        recipient.holder,
        control.scope,
        step::rights(call.arguments.at("rights"), control.scope.kind),
        call.arguments.value("cap", std::int64_t{}),
        step::duration_steps(call.arguments.value("period_s", 0.0), call.hz),
        duration ? call.step + duration : 0};
    const auto created =
        perm::create_grant(grants, players, issuer.holder, request, control, recipient, call.step);
    if (!created) {
      if (created.reason == perm::Reason::BadAmount || created.reason == perm::Reason::InvalidScope)
        return bad_input(perm::reason_text(created.reason));
      return denied("grant", call.arguments.at("scope"), perm::reason_text(created.reason),
                    grant_fix(created.reason));
    }
    return result(
        true, {{"grant", "g:" + std::to_string(created.grant)},
               {"details", grant_json(grants, grants.rows[created.grant], call.step, call.hz)}});
  } catch (const std::exception &error) {
    return bad_input(error.what());
  }
}
step::NativeCompletion revoke(amber::perm::State &perm_state,
                              const amber::ObjectState &objects_state, bool issuer_is_architect,
                              const step::NativeCall &call) {
  auto &grants = perm_state.grants;
  auto &players = perm_state.players;
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto &limits = perm_state.owner_limits;
  auto &plots = perm_state.plots;
  auto &groups = perm_state.groups;

  try {
    const auto issuer = actor(call);
    PlayerId who = 0;
    if (issuer.kind == perm::HolderKind::Player)
      who = player(call, players);
    else {
      if (!issuer.id || !identity.valid(issuer.installed_on))
        throw std::invalid_argument("stale script caller");
      who = authority.owner[issuer.installed_on.slot()];
    }
    const auto allowed = [&](const perm::Grant &grant) {
      if (issuer.kind == perm::HolderKind::Player)
        return grant.issuer != 0 && (grant.issuer == who || issuer_is_architect);
      return grant.holder == issuer || (grant.parent && grants.rows[grant.parent].holder == issuer);
    };
    std::vector<GrantId> ids;
    if (call.arguments.contains("grant")) {
      const auto id = step::identifier(call.arguments.at("grant"), "g:");
      if (id >= grants.rows.size())
        throw std::invalid_argument("unknown grant");
      if (!allowed(grants.rows[id]))
        return denied("revoke", "g:" + std::to_string(id),
                      "caller did not issue or hold this grant",
                      "have its issuer revoke it, or relinquish a grant held by this caller");
      ids.push_back(id);
    } else {
      const auto selected = holder(call.arguments.at("holder"), players);
      if (call.arguments.contains("scope")) {
        const auto scope = step::scope(call.arguments.at("scope"), identity, authority, limits,
                                       players, plots, groups)
                               .scope;
        const auto found = grants.by_key.find({selected, scope});
        if (found != grants.by_key.end()) {
          if (!allowed(grants.rows[found->second]))
            return denied("revoke", "g:" + std::to_string(found->second),
                          "caller did not issue or hold this grant",
                          "have its issuer revoke it, or relinquish a grant held by this caller");
          ids.push_back(found->second);
        }
      } else if (issuer.kind == perm::HolderKind::Player && !issuer_is_architect) {
        for (auto id : players.rows[who].grants_given)
          if (grants.rows[id].holder == selected)
            ids.push_back(id);
      } else
        for (const auto &grant : grants.rows)
          if (grant.live && grant.holder == selected && allowed(grant))
            ids.push_back(grant.id);
    }
    const auto removed = perm::revoke_many(grants, players, ids);
    Json revoked = Json::array();
    for (auto id : removed)
      revoked.push_back("g:" + std::to_string(id));
    return result(!removed.empty(), {{"revoked", revoked}});
  } catch (const std::exception &error) {
    return bad_input(error.what());
  }
}
step::NativeCompletion list_grants(const perm::Grants &grants, const perm::Players &players,
                                   const step::NativeCall &call) {
  try {
    const auto issuer = actor(call);
    auto rows = [&](const auto &ids) {
      Json result = Json::array();
      for (auto id : ids)
        if (in_force(grants, grants.rows[id], call.step))
          result.push_back(grant_json(grants, grants.rows[id], call.step, call.hz));
      return result;
    };
    if (issuer.kind == perm::HolderKind::Player) {
      const auto who = player(call, players);
      return result(false, {{"held", rows(players.rows[who].grants_held)},
                            {"given", rows(players.rows[who].grants_given)}});
    }
    std::vector<GrantId> held, given;
    for (const auto &grant : grants.rows)
      if (grant.live) {
        if (grant.holder == issuer)
          held.push_back(grant.id);
        if (grant.parent && grants.rows[grant.parent].holder == issuer)
          given.push_back(grant.id);
      }
    return result(false, {{"held", rows(held)}, {"given", rows(given)}});
  } catch (const std::exception &error) {
    return bad_input(error.what());
  }
}
step::NativeCompletion preset(amber::ObjectState &objects_state,
                              std::vector<std::uint32_t> &presets,
                              const amber::perm::State &perm_state, const step::NativeCall &call) {
  auto &object_groups = objects_state.objects.group;
  auto flags = amber::permission_flags(objects_state.objects);
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto &limits = perm_state.owner_limits;
  auto &players = perm_state.players;
  auto &groups = perm_state.groups;

  if (!std::holds_alternative<PlayerId>(call.request.caller))
    return denied("player_caller", "goods presets", "this action requires a current player caller",
                  "have the player perform this administrative action directly");
  try {
    const auto who = player(call, players);
    const auto object = call.request.target;
    if (!identity.valid(object))
      throw std::invalid_argument("stale goods handle");
    const auto target = perm::target(identity, authority, limits, perm_state.plots, object);
    if (target.flags & Body)
      throw std::invalid_argument("body is not goods");
    if (target.owner != who)
      return denied("owner", to_string(object), "only the current owner may change goods presets",
                    "ask p:" + std::to_string(target.owner) + " to change these presets");
    if (target.flags & Escrow)
      return denied("available_goods", to_string(object), "goods in escrow cannot change presets",
                    "cancel the offer or finish the trade before changing presets");
    const auto allowed = (perm::all_rights & ~perm::inherited_rights) | target.owner_limit;
    auto preset = target.preset;
    if (!preset_rights(preset, call.arguments, "everyone", 0, perm::ScopeKind::Object, allowed) ||
        !preset_rights(preset, call.arguments, "group_rights", 8, perm::ScopeKind::Object,
                       allowed) ||
        !preset_rights(preset, call.arguments, "next_owner", 16, perm::ScopeKind::Object, allowed))
      return denied("inherited_ceiling", to_string(object),
                    "preset cannot widen the inherited next-owner ceiling",
                    "remove rights excluded by the inherited ceiling");
    const auto assigned =
        call.arguments.contains("group") ? group(call.arguments.at("group"), groups) : target.group;
    if (call.arguments.contains("group") && assigned &&
        !std::binary_search(players.rows[who].groups.begin(), players.rows[who].groups.end(),
                            assigned))
      return denied("group_membership", "group:" + std::to_string(assigned),
                    "assigning this group requires current membership",
                    "ask the group controller to add you before assigning this group");
    const auto no_copy = call.arguments.value("no_copy", bool(target.flags & NoCopy));
    if (!no_copy && !(target.owner_limit & perm::bit(perm::Verb::Copy)))
      return denied("copy", to_string(object), "inherited copy restriction cannot be cleared",
                    "keep no_copy enabled for these goods");
    const bool changed = preset != target.preset || assigned != target.group ||
                         no_copy != bool(target.flags & NoCopy);
    presets[object.slot()] = preset;
    object_groups[object.slot()] = assigned;
    flags.set(object.slot(),
              (flags.get(object.slot()) & ~NoCopy) | (no_copy ? std::uint32_t{NoCopy} : 0));
    auto details = preset_json(preset, assigned, false);
    details["no_copy"] = no_copy;
    auto out = result(changed, {{"object", to_string(object)}, {"preset", details}});
    if (changed)
      out.changed_objects.push_back(object);
    return out;
  } catch (const std::exception &error) {
    return bad_input(error.what());
  }
}
step::NativeCompletion group_create(perm::Groups &groups, perm::Players &players,
                                    const step::NativeCall &call) {
  if (!std::holds_alternative<PlayerId>(call.request.caller))
    return denied("player_caller", "group creation", "this action requires a current player caller",
                  "have the player perform this administrative action directly");
  try {
    const auto who = player(call, players);
    const auto name = call.arguments.at("name").get<std::string>();
    const auto id = static_cast<GroupId>(groups.rows.size());
    groups.rows.push_back({id, who, name});
    auto &memberships = players.rows[who].groups;
    memberships.insert(std::lower_bound(memberships.begin(), memberships.end(), id), id);
    return result(true, {{"group", "group:" + std::to_string(id)},
                         {"controller", "p:" + std::to_string(who)},
                         {"name", name}});
  } catch (const std::exception &error) {
    return bad_input(error.what());
  }
}
step::NativeCompletion group_member(const perm::Groups &groups, perm::Players &players,
                                    const step::NativeCall &call) {
  if (!std::holds_alternative<PlayerId>(call.request.caller))
    return denied("player_caller", "group membership",
                  "this action requires a current player caller",
                  "have the player perform this administrative action directly");
  try {
    const auto who = player(call, players);
    const auto id = step::identifier(call.arguments.at("group"), "group:");
    const auto member = recipient(call.arguments.at("player"), players);
    const auto present = call.arguments.at("member").get<bool>();
    if (id >= groups.rows.size() || groups.rows[id].id != id)
      throw std::invalid_argument("unknown group");
    if (groups.rows[id].controller != who && (member != who || present))
      return denied("group_controller", "group:" + std::to_string(id),
                    "only the group controller may change another membership",
                    "ask p:" + std::to_string(groups.rows[id].controller) +
                        " to change membership; you may leave the group yourself");
    auto &memberships = players.rows[member].groups;
    const auto found = std::lower_bound(memberships.begin(), memberships.end(), id);
    const bool before = found != memberships.end() && *found == id;
    if (present && !before)
      memberships.insert(found, id);
    else if (!present && before)
      memberships.erase(found);
    return result(before != present, {{"group", "group:" + std::to_string(id)},
                                      {"player", "p:" + std::to_string(member)},
                                      {"member", present}});
  } catch (const std::exception &error) {
    return bad_input(error.what());
  }
}
step::NativeCompletion plot_permissions(perm::Plots &plots, const perm::Players &players,
                                        const perm::Groups &groups, const step::NativeCall &call) {
  if (!std::holds_alternative<PlayerId>(call.request.caller))
    return denied("player_caller", "plot presets", "this action requires a current player caller",
                  "have the player perform this administrative action directly");
  try {
    const auto who = player(call, players);
    const auto id = call.arguments.at("plot").get<PlotId>();
    if (!id || id >= plots.rows.size() || plots.rows[id].id != id)
      throw std::invalid_argument("unknown plot");
    auto &plot = plots.rows[id];
    if (plot.owner != who)
      return denied("plot_owner", "plot:" + std::to_string(id),
                    "only the current plot owner may change plot permissions",
                    "ask p:" + std::to_string(plot.owner) + " to change plot permissions");
    auto preset = plot.preset;
    if (!preset_rights(preset, call.arguments, "everyone", 0, perm::ScopeKind::Plot, 7) ||
        !preset_rights(preset, call.arguments, "group_rights", 8, perm::ScopeKind::Plot, 7))
      return bad_input("plot presets allow only build, enter and run_scripts");
    const auto assigned =
        call.arguments.contains("group") ? group(call.arguments.at("group"), groups) : plot.group;
    if (call.arguments.contains("group") && assigned &&
        !std::binary_search(players.rows[who].groups.begin(), players.rows[who].groups.end(),
                            assigned))
      return denied("group_membership", "group:" + std::to_string(assigned),
                    "assigning this group requires current membership",
                    "ask the group controller to add you before assigning this group");
    auto banned = plot.banned;
    if (call.arguments.contains("banned")) {
      banned.clear();
      for (const auto &value : call.arguments.at("banned"))
        banned.push_back(recipient(value, players));
      std::sort(banned.begin(), banned.end());
      banned.erase(std::unique(banned.begin(), banned.end()), banned.end());
    }
    const bool changed = preset != plot.preset || assigned != plot.group || banned != plot.banned;
    plot.preset = preset;
    plot.group = assigned;
    plot.banned = std::move(banned);
    auto details = preset_json(preset, assigned, true);
    details["banned"] = Json::array();
    for (auto player : plot.banned)
      details["banned"].push_back("p:" + std::to_string(player));
    return result(changed, {{"plot", id}, {"permissions", details}});
  } catch (const std::exception &error) {
    return bad_input(error.what());
  }
}
void advance_permission_deadlines(amber::perm::State &perm_state, TravelAccess &travel_access,
                                  net::Sessions &sessions, record::Journal &journal, Step now,
                                  double hz, double reminder_seconds, std::size_t limit) {
  auto &grants = perm_state.grants;
  auto &players = perm_state.players;

  auto grant_updates = advance_grants(
      grants, players, now, static_cast<Step>(std::llround(reminder_seconds * hz)), hz, limit);
  invalidate_travel(travel_access, grants, now);
  for (auto &notice : grant_updates.notices)
    sessions.event_for_player(notice.recipient, std::move(notice.event));
  for (auto &event : grant_updates.records)
    journal.append(std::move(event));
}
} // namespace amber::step
