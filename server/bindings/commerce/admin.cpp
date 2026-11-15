#include "admin.hpp"

#include <algorithm>
#include <limits>

namespace amber::bindings::commerce {
namespace {
PlayerId player(const drive::NativeCall &call, const perm::Players &players) {
  const auto *id = std::get_if<PlayerId>(&call.request.caller);
  if (!id || !*id || *id >= players.rows.size())
    throw std::invalid_argument("this administration action requires a current player caller");
  return *id;
}
PlayerId recipient(const Json &value, const perm::Players &players) {
  const auto id = authority::identifier(value, "p:");
  if (id >= players.rows.size())
    throw std::invalid_argument("unknown player");
  return id;
}
bool architect(const control::Profiles &profiles, PlayerId player) {
  return player < profiles.rows.size() && profiles.rows[player].id == player &&
         std::find(profiles.rows[player].roles.begin(), profiles.rows[player].roles.end(),
                   "architect") != profiles.rows[player].roles.end();
}
perm::Holder holder(const Json &row, const perm::Players &players) {
  if (row.is_string())
    return perm::Holder::player(recipient(row, players));
  const auto parsed = parse_handle(row.at("on").get<std::string>());
  if (!parsed)
    throw std::invalid_argument("script holder needs a full object handle");
  return perm::Holder::script(authority::identifier(row.at("script"), "s:"), *parsed);
}
perm::HolderControl installed_holder(const Json &row, const GrantBinding &binding,
                                     const drive::NativeCall &call) {
  const auto selected = holder(row, binding.players);
  if (selected.kind == perm::HolderKind::Player)
    return {selected, selected.id, true};
  const auto object = selected.installed_on;
  if (!binding.scopes.identity.valid(object) || selected.id >= binding.scripts.rows.size() ||
      binding.scripts.rows[selected.id].id != selected.id)
    throw std::invalid_argument("unknown current script holder");
  auto installed = install::installed_scripts(binding.models, binding.instances,
                                              binding.scopes.identity, object);
  const auto rules =
      install::rule_scripts(binding.rules, binding.values, object, call.step, call.hz);
  installed.insert(installed.end(), rules.begin(), rules.end());
  return {selected, binding.scopes.authority.owner[object.slot()],
          std::find(installed.begin(), installed.end(), selected.id) != installed.end()};
}
drive::NativeCompletion result(bool changed, Json fields = Json::object()) {
  drive::NativeCompletion out;
  out.changed = changed;
  out.result = changed ? record::Result::Applied : record::Result::NoEffect;
  out.reply = {{"ok", true}, {"effect", changed ? "applied" : "none"}};
  out.reply.update(fields);
  return out;
}
drive::NativeCompletion denied(std::string_view reason, Json fields = Json::object()) {
  drive::NativeCompletion out;
  out.result = record::Result::Denied;
  out.reply = {{"ok", false}, {"error", "permission_denied"}, {"problem", reason}};
  out.reply.update(fields);
  return out;
}
void invalidate(const AdminInvalidation &sink) {
  if (sink.changed)
    sink.changed(sink.local);
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
  case perm::ScopeKind::OwnedOnPlot:
    return {{"owned_on_plot",
             {{"owner", "p:" + std::to_string(scope.owner)}, {"plot", scope.subject}}}};
  case perm::ScopeKind::Account:
    return {{"account", "p:" + std::to_string(scope.subject)}};
  case perm::ScopeKind::Avatar:
    return {{"avatar", to_string(Handle{scope.subject})}};
  case perm::ScopeKind::Plot:
    return {{"plot", scope.subject}};
  case perm::ScopeKind::Issuance:
    return {{"issuance", true}};
  }
  throw std::logic_error("unknown stored grant scope");
}
Json rights_json(perm::Rights rights, perm::ScopeKind scope) {
  Json result = Json::array();
  if (scope == perm::ScopeKind::Account || scope == perm::ScopeKind::Issuance) {
    if (rights & 1)
      result.push_back(scope == perm::ScopeKind::Account ? "spend" : "issue");
  } else if (scope == perm::ScopeKind::Avatar) {
    if (rights & 1)
      result.push_back("animate");
    if (rights & 2)
      result.push_back("camera");
  } else if (scope == perm::ScopeKind::Plot) {
    constexpr std::array names{"build", "enter", "run_scripts"};
    for (std::size_t index = 0; index < names.size(); ++index)
      if (rights & (perm::Rights{1} << index))
        result.push_back(names[index]);
    if (rights & perm::bit(perm::Verb::Transfer))
      result.push_back("transfer");
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
  const bool monetary =
      grant.scope.kind == perm::ScopeKind::Account || grant.scope.kind == perm::ScopeKind::Issuance;
  return {
      {"id", "grant:" + std::to_string(grant.id)},
      {"issuer", "p:" + std::to_string(grant.issuer)},
      {"holder", holder_json(grant.holder)},
      {"scope", scope_json(grant.scope)},
      {"rights", rights_json(grant.verbs, grant.scope.kind)},
      {"parent", grant.parent ? Json("grant:" + std::to_string(grant.parent)) : Json(nullptr)},
      {"cap", monetary ? Json(grant.cap) : Json(nullptr)},
      {"remaining", monetary ? Json(perm::allowance(grants, grant.id, step)) : Json(nullptr)},
      {"period_s", grant.period_steps / hz},
      {"expires_step", grant.expires_step ? Json(grant.expires_step) : Json(nullptr)},
      {"expires_in_s", grant.expires_step
                           ? Json((grant.expires_step > step ? grant.expires_step - step : 0) / hz)
                           : Json(nullptr)}};
}
GroupId group(const Json &value, PlayerId who, const perm::Players &players,
              const perm::Groups &groups) {
  if (value.is_null())
    return 0;
  const auto id = authority::identifier(value, "g:");
  if (id >= groups.rows.size() || groups.rows[id].id != id)
    throw std::invalid_argument("unknown group");
  if (!std::binary_search(players.rows[who].groups.begin(), players.rows[who].groups.end(), id))
    throw std::invalid_argument("assigned group requires current membership");
  return id;
}
void preset_rights(std::uint32_t &preset, const Json &arguments, std::string_view name,
                   unsigned shift, perm::ScopeKind scope, perm::Rights allowed) {
  if (!arguments.contains(name))
    return;
  const auto rights = authority::rights(arguments.at(name), scope);
  if (rights & ~allowed)
    throw std::invalid_argument(scope == perm::ScopeKind::Plot
                                    ? "plot presets allow only build, enter and run_scripts"
                                    : "preset cannot widen the inherited next-owner ceiling");
  const auto mask = (scope == perm::ScopeKind::Plot ? perm::Rights{7} : perm::all_rights) << shift;
  preset = (preset & ~mask) | (rights << shift);
}
Json preset_json(std::uint32_t preset, GroupId group, bool plot) {
  const auto scope = plot ? perm::ScopeKind::Plot : perm::ScopeKind::Object;
  const auto mask = plot ? perm::Rights{7} : perm::all_rights;
  Json row = {{"everyone", rights_json(preset & mask, scope)},
              {"group_rights", rights_json((preset >> 8) & mask, scope)},
              {"group", group ? Json("g:" + std::to_string(group)) : Json(nullptr)}};
  if (!plot)
    row["next_owner"] = rights_json((preset >> 16) & mask, scope);
  return row;
}
} // namespace
GrantUpdates advance_grants(GrantMaintenance &binding, Step now, Step reminder_steps, double hz,
                            std::size_t max_entries) {
  const auto due =
      perm::advance_grants(binding.grants, binding.players, now, reminder_steps, max_entries);
  GrantUpdates updates;
  const auto publish = [&](GrantId id, bool expired) {
    const auto &grant = binding.grants.rows[id];
    auto details = grant_json(binding.grants, grant, now, hz);
    details["kind"] = expired ? "grant_expired" : "grant_expiring";
    details["say"] = expired ? "The grant has expired."
                             : "This grant expires soon; renew it if it is still needed.";
    updates.notices.push_back({grant.issuer, details});
    if (grant.parent) {
      const auto owner = binding.grants.rows[grant.parent].issuer;
      if (owner != grant.issuer)
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
  if (!due.expired.empty())
    invalidate(binding.invalidation);
  return updates;
}
drive::NativeCompletion grant(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<GrantBinding *>(local);
  try {
    const auto who = player(call, binding.players);
    const auto control = authority::scope(call.arguments.at("scope"), binding.scopes, who);
    const auto holder = installed_holder(call.arguments.at("holder"), binding, call);
    const auto duration =
        authority::duration_steps(call.arguments.value("expires_s", 0.0), call.hz);
    if (duration > std::numeric_limits<Step>::max() - call.step)
      throw std::invalid_argument("grant expiry exceeds world step range");
    const perm::GrantRequest request{
        who,
        holder.holder,
        control.scope,
        authority::rights(call.arguments.at("rights"), control.scope.kind),
        call.arguments.value("cap", std::int64_t{}),
        authority::duration_steps(call.arguments.value("period_s", 0.0), call.hz),
        duration ? call.step + duration : 0};
    const auto created =
        perm::create_grant(binding.grants, binding.players, request, control, holder, call.step);
    if (!created)
      return denied(perm::reason_text(created.reason),
                    {{"need", call.arguments.at("rights")}, {"scope", call.arguments.at("scope")}});
    invalidate(binding.invalidation);
    return result(true, {{"grant", "grant:" + std::to_string(created.grant)},
                         {"details", grant_json(binding.grants, binding.grants.rows[created.grant],
                                                call.step, call.hz)}});
  } catch (const std::exception &error) {
    return denied(error.what());
  }
}
drive::NativeCompletion revoke(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<RevokeBinding *>(local);
  try {
    const auto who = player(call, binding.players);
    std::vector<GrantId> ids;
    if (call.arguments.contains("grant")) {
      const auto id = authority::identifier(call.arguments.at("grant"), "grant:");
      if (id >= binding.grants.rows.size())
        throw std::invalid_argument("unknown grant");
      if (binding.grants.rows[id].issuer != who && !architect(binding.profiles, who))
        throw std::invalid_argument("only the issuer or architect may revoke this grant");
      ids.push_back(id);
    } else {
      const auto selected = holder(call.arguments.at("holder"), binding.players);
      for (auto id : binding.players.rows[who].grants_given)
        if (binding.grants.rows[id].holder == selected)
          ids.push_back(id);
    }
    const auto removed = perm::revoke_many(binding.grants, binding.players, ids);
    Json revoked = Json::array();
    for (auto id : removed)
      revoked.push_back("grant:" + std::to_string(id));
    if (!removed.empty())
      invalidate(binding.invalidation);
    return result(!removed.empty(), {{"revoked", revoked}});
  } catch (const std::exception &error) {
    return denied(error.what());
  }
}
drive::NativeCompletion list_grants(void *local, const drive::NativeCall &call) {
  const auto &binding = *static_cast<ListGrantsBinding *>(local);
  try {
    const auto who = player(call, binding.players);
    auto rows = [&](const auto &ids) {
      Json result = Json::array();
      for (auto id : ids)
        if (in_force(binding.grants, binding.grants.rows[id], call.step))
          result.push_back(grant_json(binding.grants, binding.grants.rows[id], call.step, call.hz));
      return result;
    };
    return result(false, {{"held", rows(binding.players.rows[who].grants_held)},
                          {"given", rows(binding.players.rows[who].grants_given)}});
  } catch (const std::exception &error) {
    return denied(error.what());
  }
}
drive::NativeCompletion preset(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<PresetBinding *>(local);
  try {
    const auto who = player(call, binding.players);
    const auto object = call.request.target;
    if (!binding.identity.valid(object))
      throw std::invalid_argument("stale goods handle");
    const auto target = perm::target(binding.identity, binding.authority, binding.limits, object);
    if (target.flags & Body)
      throw std::invalid_argument("body is not goods");
    if (target.owner != who)
      throw std::invalid_argument("only the current owner may change goods presets");
    if (target.flags & Escrow)
      throw std::invalid_argument("goods in escrow cannot change presets");
    const auto allowed = (perm::all_rights & ~perm::inherited_rights) | target.owner_limit;
    auto preset = target.preset;
    preset_rights(preset, call.arguments, "everyone", 0, perm::ScopeKind::Object, allowed);
    preset_rights(preset, call.arguments, "group_rights", 8, perm::ScopeKind::Object, allowed);
    preset_rights(preset, call.arguments, "next_owner", 16, perm::ScopeKind::Object, allowed);
    const auto assigned = call.arguments.contains("group") ? group(call.arguments.at("group"), who,
                                                                   binding.players, binding.groups)
                                                           : target.group;
    const auto no_copy = call.arguments.value("no_copy", bool(target.flags & NoCopy));
    if (!no_copy && !(target.owner_limit & perm::bit(perm::Verb::Copy)))
      throw std::invalid_argument("inherited copy restriction cannot be cleared");
    const bool changed = preset != target.preset || assigned != target.group ||
                         no_copy != bool(target.flags & NoCopy);
    binding.columns.preset[object.slot()] = preset;
    binding.columns.group[object.slot()] = assigned;
    binding.flags.set(object.slot(), (binding.flags.get(object.slot()) & ~NoCopy) |
                                         (no_copy ? std::uint32_t{NoCopy} : 0));
    if (changed)
      invalidate(binding.invalidation);
    auto details = preset_json(preset, assigned, false);
    details["no_copy"] = no_copy;
    return result(changed, {{"object", to_string(object)}, {"preset", details}});
  } catch (const std::exception &error) {
    return denied(error.what());
  }
}
drive::NativeCompletion group_create(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<GroupCreateBinding *>(local);
  try {
    const auto who = player(call, binding.players);
    const auto name = call.arguments.at("name").get<std::string>();
    const auto id = static_cast<GroupId>(binding.groups.rows.size());
    binding.groups.rows.push_back({id, who, name});
    auto &groups = binding.players.rows[who].groups;
    groups.insert(std::lower_bound(groups.begin(), groups.end(), id), id);
    invalidate(binding.invalidation);
    return result(true, {{"group", "g:" + std::to_string(id)},
                         {"controller", "p:" + std::to_string(who)},
                         {"name", name}});
  } catch (const std::exception &error) {
    return denied(error.what());
  }
}
drive::NativeCompletion group_member(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<GroupMemberBinding *>(local);
  try {
    const auto who = player(call, binding.players);
    const auto id = authority::identifier(call.arguments.at("group"), "g:");
    const auto member = recipient(call.arguments.at("player"), binding.players);
    const auto present = call.arguments.at("member").get<bool>();
    if (id >= binding.groups.rows.size() || binding.groups.rows[id].id != id)
      throw std::invalid_argument("unknown group");
    if (binding.groups.rows[id].controller != who && (member != who || present))
      throw std::invalid_argument("only the group controller may change another membership");
    auto &memberships = binding.players.rows[member].groups;
    const auto found = std::lower_bound(memberships.begin(), memberships.end(), id);
    const bool before = found != memberships.end() && *found == id;
    if (present && !before)
      memberships.insert(found, id);
    else if (!present && before)
      memberships.erase(found);
    if (before != present)
      invalidate(binding.invalidation);
    return result(before != present, {{"group", "g:" + std::to_string(id)},
                                      {"player", "p:" + std::to_string(member)},
                                      {"member", present}});
  } catch (const std::exception &error) {
    return denied(error.what());
  }
}
drive::NativeCompletion plot_permissions(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<PlotPermissionsBinding *>(local);
  try {
    const auto who = player(call, binding.players);
    const auto id = call.arguments.at("plot").get<PlotId>();
    if (!id || id >= binding.plots.rows.size() || binding.plots.rows[id].id != id)
      throw std::invalid_argument("unknown plot");
    auto &plot = binding.plots.rows[id];
    if (plot.owner != who)
      throw std::invalid_argument("only the current plot owner may change plot permissions");
    auto preset = plot.preset;
    preset_rights(preset, call.arguments, "everyone", 0, perm::ScopeKind::Plot, 7);
    preset_rights(preset, call.arguments, "group_rights", 8, perm::ScopeKind::Plot, 7);
    const auto assigned = call.arguments.contains("group") ? group(call.arguments.at("group"), who,
                                                                   binding.players, binding.groups)
                                                           : plot.group;
    auto banned = plot.banned;
    if (call.arguments.contains("banned")) {
      banned.clear();
      for (const auto &value : call.arguments.at("banned"))
        banned.push_back(recipient(value, binding.players));
      std::sort(banned.begin(), banned.end());
      banned.erase(std::unique(banned.begin(), banned.end()), banned.end());
    }
    const bool changed = preset != plot.preset || assigned != plot.group || banned != plot.banned;
    plot.preset = preset;
    plot.group = assigned;
    plot.banned = std::move(banned);
    if (changed)
      invalidate(binding.invalidation);
    auto details = preset_json(preset, assigned, true);
    details["banned"] = Json::array();
    for (auto player : plot.banned)
      details["banned"].push_back("p:" + std::to_string(player));
    return result(changed, {{"plot", id}, {"permissions", details}});
  } catch (const std::exception &error) {
    return denied(error.what());
  }
}
} // namespace amber::bindings::commerce
