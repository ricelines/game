#include "common.hpp"

#include <limits>

namespace amber::bindings::editing {
namespace {
const catalog::Script &script(const catalog::Scripts &scripts, const Json &value) {
  const auto id = detail::identifier(value, "s:");
  if (id >= scripts.rows.size() || scripts.rows[id].id != id ||
      scripts.rows[id].declaration.kind == react::ScriptDeclaration::Kind::Rule)
    throw std::invalid_argument("installation needs an uploaded ordinary script");
  return scripts.rows[id];
}
std::vector<ScriptId> extras(const install::InstanceScripts &instances, Handle object) {
  if (const auto *rows = instances.get(object.slot()))
    return *rows;
  return {};
}
void revoke_holder(perm::Grants &grants, perm::Players &players, ScriptKey key) {
  std::vector<GrantId> ids;
  const auto holder = perm::Holder::script(key.script, key.object);
  for (const auto &grant : grants.rows)
    if (grant.live && grant.holder == holder)
      ids.push_back(grant.id);
  if (!ids.empty())
    perm::revoke_many(grants, players, ids);
}
void withdraw_removed(react::SharedColumns &shared, react::GivesTo &gives,
                      const react::Terms &terms, const catalog::Scripts &scripts, Handle object,
                      ScriptId removed, std::span<const ScriptId> remaining,
                      react::DeferredEffects &effects) {
  const auto *reverse = gives.get(object.slot());
  if (!reverse)
    return;
  const auto outgoing = *reverse;
  for (const auto &reference : outgoing) {
    const auto &declared = scripts.rows[removed].declaration.contributes;
    if (std::find(declared.begin(), declared.end(), reference.term) == declared.end())
      continue;
    const auto retained = std::any_of(remaining.begin(), remaining.end(), [&](ScriptId id) {
      const auto &contributes = scripts.rows[id].declaration.contributes;
      return std::find(contributes.begin(), contributes.end(), reference.term) != contributes.end();
    });
    if (!retained)
      react::set_contribution(shared, gives, terms.rows[reference.term], object, reference.subject,
                              0, effects);
  }
}
void synchronize(ScriptEditing &binding, Handle object, std::span<const ScriptId> before,
                 std::span<const ScriptId> after, const drive::NativeCall &call,
                 react::DeferredEffects &effects) {
  for (auto old : before)
    if (std::find(after.begin(), after.end(), old) == after.end()) {
      install::remove_watches(binding.subscriptions, binding.links, {object, old});
      binding.states.erase({object, old});
      revoke_holder(binding.grants, binding.players, {object, old});
      withdraw_removed(binding.shared, binding.gives, binding.values.terms, binding.scripts, object,
                       old, after, effects);
    }
  for (auto added : after)
    if (std::find(before.begin(), before.end(), added) == before.end()) {
      const auto &declaration = binding.scripts.rows[added].declaration;
      detail::watches(effects, install::sync_watches(
                                   binding.subscriptions, binding.links, binding.values,
                                   binding.allocations, {object, added}, declaration, call.step,
                                   call.hz, call.origin, install::SyncMode::Change));
      if (binding.start_event && declaration.active &&
          std::find(declaration.handlers.begin(), declaration.handlers.end(),
                    binding.start_event) != declaration.handlers.end())
        effects.ordered.emplace_back(
            step::EventWork{object, {}, binding.start_event, {}, call.origin, added});
    }
}
struct SlotRequest {
  std::string name;
  perm::GrantRequest request;
  perm::ScopeControl control;
};
std::vector<SlotRequest> choices(const Json &rows, const catalog::Script &source,
                                 const ScriptEditing &binding, const drive::NativeCall &call) {
  const authority::ScopeRead scopes{binding.values.identity, binding.authority, binding.limits,
                                    binding.players,         binding.plots,     binding.profiles};
  std::vector<SlotRequest> result;
  std::set<std::string> chosen;
  for (const auto &row : rows) {
    const auto name = row.at("slot").get<std::string>();
    const auto &slots = source.declaration.slots;
    if (std::find(slots.begin(), slots.end(), name) == slots.end() || !chosen.insert(name).second)
      throw std::invalid_argument("unknown or repeated script slot");
    if (row.at("rights").size() != 1 || row.at("rights")[0] != name)
      throw std::invalid_argument("a slot choice must grant exactly its declared right");
    const auto issuer = detail::player(call);
    const auto control = authority::scope(row.at("scope"), scopes, issuer);
    const auto expires = authority::duration_steps(row.at("expires_s").get<double>(), call.hz);
    if (!expires || expires > std::numeric_limits<Step>::max() - call.step)
      throw std::invalid_argument("script grant needs a future expiry");
    result.push_back(
        {name,
         {issuer, perm::Holder::script(source.id, call.request.target), control.scope,
          authority::rights(row.at("rights"), control.scope.kind), row.value("cap", std::int64_t{}),
          authority::duration_steps(row.value("period_s", 0.0), call.hz), call.step + expires},
         control});
  }
  return result;
}
Json preview(const catalog::Script &source, Handle object, const ScriptEditing &binding,
             const drive::NativeCall &call) {
  Json slots = Json::array();
  for (const auto &name : source.declaration.slots) {
    Json selected_scope = {{"object", to_string(object)}};
    if (name == "spend")
      selected_scope = {{"account", "p:" + std::to_string(detail::player(call))}};
    else if (name == "issue")
      selected_scope = {{"issuance", true}};
    else if (name == "animate" || name == "camera")
      selected_scope = {{"avatar", call.body ? Json(to_string(call.body)) : Json(nullptr)}};
    else if (name == "build" || name == "enter" || name == "run_scripts")
      selected_scope = {{"plot", binding.authority.plot[object.slot()]}};
    slots.push_back({{"slot", name},
                     {"suggested",
                      {{"scope", selected_scope},
                       {"rights", Json::array({name})},
                       {"cap", 0},
                       {"period_s", 3600},
                       {"expires_s", 3600}}}});
  }
  return slots;
}
EditAccess access(const ScriptEditing &binding) {
  return {binding.values.identity, binding.authority, binding.limits, binding.players,
          binding.grants};
}
} // namespace
drive::NativeCompletion install_script(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<ScriptEditing *>(local);
  try {
    detail::edit(access(binding), call);
    const auto object = call.request.target;
    const auto &source = script(binding.scripts, call.arguments.at("script"));
    if (!call.arguments.contains("choices"))
      return detail::result(
          false, {{"slots", preview(source, object, binding, call)}, {"requires_commit", true}});
    const auto requested = choices(call.arguments.at("choices"), source, binding, call);
    const auto plot = binding.authority.plot[object.slot()];
    if (plot &&
        (plot >= binding.plots.rows.size() ||
         !perm::check_land(perm::Holder::player(detail::player(call)), perm::LandVerb::RunScripts,
                           binding.plots.rows[plot], binding.players, binding.grants, call.step)))
      throw std::invalid_argument("run_scripts denied on current plot");
    const auto before = install::installed_scripts(binding.models, binding.instances,
                                                   binding.values.identity, object);
    auto out =
        detail::result(false, {{"grants", Json::array()}, {"unfilled_slots", Json::array()}});
    if (std::find(before.begin(), before.end(), source.id) == before.end()) {
      auto ids = extras(binding.instances, object);
      ids.push_back(source.id);
      install::set_instances(binding.instances, object, ids, binding.scripts);
      install::project_instance(binding.triggers, binding.instances, binding.scripts,
                                binding.values.terms, object);
      const auto after = install::installed_scripts(binding.models, binding.instances,
                                                    binding.values.identity, object);
      synchronize(binding, object, before, after, call, out.effects);
      out.changed = true;
    }
    const perm::HolderControl holder{perm::Holder::script(source.id, object),
                                     binding.authority.owner[object.slot()], true};
    std::set<std::string> filled;
    for (const auto &choice : requested) {
      const auto granted = perm::create_grant(binding.grants, binding.players, choice.request,
                                              choice.control, holder, call.step);
      if (granted) {
        out.reply["grants"].push_back({{"slot", choice.name}, {"grant", granted.grant}});
        filled.insert(choice.name);
        out.changed = true;
      } else
        out.reply["unfilled_slots"].push_back(
            {{"slot", choice.name}, {"reason", perm::reason_text(granted.reason)}});
    }
    for (const auto &name : source.declaration.slots)
      if (!filled.contains(name) && std::none_of(requested.begin(), requested.end(),
                                                 [&](const auto &row) { return row.name == name; }))
        out.reply["unfilled_slots"].push_back({{"slot", name}, {"reason", "not chosen"}});
    out.result = out.changed ? record::Result::Applied : record::Result::NoEffect;
    out.reply["effect"] = out.changed ? "applied" : "none";
    if (out.changed)
      metadata_changed(binding.changes, std::span{&object, 1});
    return out;
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
drive::NativeCompletion uninstall_script(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<ScriptEditing *>(local);
  try {
    detail::edit(access(binding), call);
    const auto object = call.request.target;
    const auto &source = script(binding.scripts, call.arguments.at("script"));
    auto ids = extras(binding.instances, object);
    const auto found = std::find(ids.begin(), ids.end(), source.id);
    if (found == ids.end())
      return detail::result(false, {{"reason", "not an explicit instance script"}});
    const auto before = install::installed_scripts(binding.models, binding.instances,
                                                   binding.values.identity, object);
    ids.erase(found);
    install::set_instances(binding.instances, object, ids, binding.scripts);
    install::project_instance(binding.triggers, binding.instances, binding.scripts,
                              binding.values.terms, object);
    const auto after = install::installed_scripts(binding.models, binding.instances,
                                                  binding.values.identity, object);
    auto out = detail::result(true);
    synchronize(binding, object, before, after, call, out.effects);
    metadata_changed(binding.changes, std::span{&object, 1});
    return out;
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
drive::NativeCompletion model_script(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<ScriptEditing *>(local);
  try {
    const auto model = detail::model(binding.models, call);
    const auto replacement = call.arguments.at("script").is_null()
                                 ? ScriptId{}
                                 : script(binding.scripts, call.arguments.at("script")).id;
    if (binding.models.rows[model].script == replacement)
      return detail::result(false);
    const auto objects = detail::instances(binding.values.identity, model);
    std::vector<std::vector<ScriptId>> before;
    before.reserve(objects.size());
    for (auto object : objects)
      before.push_back(install::installed_scripts(binding.models, binding.instances,
                                                  binding.values.identity, object));
    binding.models.rows[model].script = replacement;
    install::project_model(binding.triggers, binding.models, binding.scripts, binding.values.terms,
                           model);
    auto out = detail::result(true);
    for (std::size_t index = 0; index < objects.size(); ++index) {
      const auto after = install::installed_scripts(binding.models, binding.instances,
                                                    binding.values.identity, objects[index]);
      synchronize(binding, objects[index], before[index], after, call, out.effects);
    }
    metadata_changed(binding.changes, objects, std::span{&model, 1});
    return out;
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
} // namespace amber::bindings::editing
