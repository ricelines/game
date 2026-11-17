#include "catalog/catalog.hpp"
#include "editing_private.hpp"

#include <limits>

#include <algorithm>

namespace amber::step::editing {
namespace {
const catalog::Script &script(const catalog::Scripts &scripts, const Json &value) {
  const auto id = detail::identifier(value, "s:");
  if (id >= scripts.rows.size() || scripts.rows[id].id != id ||
      scripts.rows[id].declaration.kind == react::ScriptDeclaration::Kind::Rule)
    throw std::invalid_argument("installation needs an uploaded ordinary script");
  return scripts.rows[id];
}
std::vector<ScriptId> extras(const react::InstanceScripts &instances, Handle object) {
  if (const auto *rows = instances.get(object.slot()))
    return *rows;
  return {};
}
bool revoke_holder(perm::Grants &grants, perm::Players &players, ScriptKey key) {
  const auto held =
      perm::held_grants(grants, players, perm::Holder::script(key.script, key.object));
  return !perm::revoke_many(grants, players, held).empty();
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
bool synchronize(amber::react::State &react_state, const react::ValueRead &values,
                 const catalog::Scripts &scripts, amber::perm::State &perm_state,
                 TermId start_event, Handle object, std::span<const ScriptId> before,
                 std::span<const ScriptId> after, const amber::step::Clock &clock,
                 const amber::Origin &origin, react::DeferredEffects &effects) {
  const auto now = clock.step;
  const auto hz = clock.hz;

  auto &subscriptions = react_state.subscriptions;
  auto &states = react_state.states;
  auto &shared = react_state.shared;
  auto &gives = react_state.gives_to;

  auto &triggers = react_state.triggers;
  auto &grants = perm_state.grants;
  auto &players = perm_state.players;

  bool permissions_changed = false;
  auto remaining = std::vector<ScriptId>(after.begin(), after.end());
  if (std::any_of(before.begin(), before.end(), [&](ScriptId old) {
        return std::find(after.begin(), after.end(), old) == after.end();
      }))
    for (const auto &[trigger, row] : triggers) {
      (void)row;
      for (const auto &handler : react::dispatch(triggers, values, object, trigger, now, hz))
        if (std::find(remaining.begin(), remaining.end(), handler.script) == remaining.end())
          remaining.push_back(handler.script);
    }
  for (auto old : before)
    if (std::find(after.begin(), after.end(), old) == after.end()) {
      react::remove_watches(subscriptions, {object, old});
      states.erase({object, old});
      permissions_changed |= revoke_holder(grants, players, {object, old});
      withdraw_removed(shared, gives, values.terms, scripts, object, old, remaining, effects);
    }
  for (auto added : after)
    if (std::find(before.begin(), before.end(), added) == before.end()) {
      const auto &declaration = scripts.rows[added].declaration;
      detail::watches(effects,
                      react::sync_watches(react_state, values, {object, added}, declaration, now,
                                          hz, origin, react::SyncMode::Change));
      if (start_event && declaration.active &&
          std::find(declaration.handlers.begin(), declaration.handlers.end(), start_event) !=
              declaration.handlers.end())
        effects.ordered.emplace_back(react::EventWork{object, {}, start_event, {}, origin, added});
    }
  return permissions_changed;
}
} // namespace
namespace detail {
std::vector<SlotRequest> slot_choices(const Json &rows, const catalog::Script &source,
                                      const amber::ObjectState &objects_state,
                                      const amber::perm::State &perm_state,
                                      const step::NativeCall &call) {
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto &limits = perm_state.owner_limits;
  auto &players = perm_state.players;
  auto &plots = perm_state.plots;
  auto &groups = perm_state.groups;

  std::vector<SlotRequest> result;
  std::set<std::string> chosen;
  for (const auto &row : rows) {
    const auto name = row.at("slot").get<std::string>();
    const auto &slots = source.declaration.slots;
    if (std::find(slots.begin(), slots.end(), name) == slots.end() || !chosen.insert(name).second)
      throw std::invalid_argument("unknown or repeated script slot");
    if (row.at("rights").size() != 1 || row.at("rights")[0] != name)
      throw std::invalid_argument("a slot choice must grant exactly its declared right");
    const auto actor = detail::holder(call);
    const auto issuer = actor.kind == perm::HolderKind::Player
                            ? actor.id
                            : authority.owner[actor.installed_on.slot()];
    const auto control =
        step::scope(row.at("scope"), identity, authority, limits, players, plots, groups);
    const auto expires = step::duration_steps(row.at("expires_s").get<double>(), call.hz);
    if (!expires || expires > std::numeric_limits<Step>::max() - call.step)
      throw std::invalid_argument("script grant needs a future expiry");
    perm::GrantRequest request{issuer,
                               perm::Holder::script(source.id, call.request.target),
                               control.scope,
                               step::rights(row.at("rights"), control.scope.kind),
                               row.value("cap", std::int64_t{}),
                               step::duration_steps(row.value("period_s", 0.0), call.hz),
                               call.step + expires};
    const auto shared = std::find_if(result.begin(), result.end(), [&](const auto &choice) {
      return choice.request.scope == request.scope;
    });
    if (shared == result.end()) {
      result.push_back({{name}, request, control});
    } else {
      if (shared->request.cap != request.cap ||
          shared->request.period_steps != request.period_steps ||
          shared->request.expires_step != request.expires_step)
        throw std::invalid_argument(
            "slots on the same scope must choose the same cap, period and expiry");
      shared->request.verbs |= request.verbs;
      shared->names.push_back(name);
    }
  }
  return result;
}
NativeCompletion fill_slots(const catalog::Script &source, std::span<const SlotRequest> requested,
                            AuthorityRead authority, perm::Players &players, perm::Grants &grants,
                            const NativeCall &call) {
  auto out = result(false, {{"grants", Json::array()}, {"unfilled_slots", Json::array()}});
  const perm::HolderControl recipient{perm::Holder::script(source.id, call.request.target),
                                      authority.owner[call.request.target.slot()], true};
  std::set<std::string> filled;
  for (const auto &choice : requested) {
    const auto granted = perm::create_grant(grants, players, holder(call), choice.request,
                                            choice.control, recipient, call.step);
    for (const auto &name : choice.names)
      if (granted) {
        out.reply["grants"].push_back(
            {{"slot", name}, {"grant", "g:" + std::to_string(granted.grant)}});
        filled.insert(name);
      } else
        out.reply["unfilled_slots"].push_back(
            {{"slot", name}, {"reason", perm::reason_text(granted.reason)}});
    if (granted) {
      out.changed = true;
      out.permissions_changed = true;
    }
  }
  for (const auto &name : source.declaration.slots)
    if (!filled.contains(name) &&
        std::none_of(requested.begin(), requested.end(), [&](const auto &row) {
          return std::find(row.names.begin(), row.names.end(), name) != row.names.end();
        }))
      out.reply["unfilled_slots"].push_back({{"slot", name}, {"reason", "not chosen"}});
  out.result = out.changed ? record::Result::Applied : record::Result::NoEffect;
  out.reply["effect"] = out.changed ? "applied" : "none";
  return out;
}
} // namespace detail
namespace {
Json preview(const catalog::Script &source, Handle object, IdentityRead identity,
             AuthorityRead authority, const step::NativeCall &call) {
  Json slots = Json::array();
  for (const auto &name : source.declaration.slots) {
    Json selected_scope = {{"object", to_string(object)}};
    if (name == "spend") {
      const auto actor = detail::holder(call);
      selected_scope = {{"account", actor.kind == perm::HolderKind::Player
                                        ? "p:" + std::to_string(actor.id)
                                        : to_string(actor.installed_on)}};
    } else if (name == "animate" || name == "camera") {
      const auto actor = detail::holder(call);
      const auto body = actor.kind == perm::HolderKind::Player ? call.body : actor.installed_on;
      selected_scope = body && identity.valid(body) && (identity.flags[body.slot()] & Body)
                           ? Json{{"avatar", to_string(body)}}
                           : Json(nullptr);
    } else if (name == "build" || name == "enter" || name == "run_scripts" || name == "return") {
      const auto plot = authority.plot[object.slot()];
      selected_scope = plot ? Json{{"plot", plot}} : Json(nullptr);
    }
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
} // namespace
step::NativeCompletion install_script(const step::NativeCall &call,
                                      amber::react::State &react_state,
                                      const amber::catalog::State &catalog_state,
                                      const amber::ObjectState &objects_state,
                                      amber::perm::State &perm_state, TermId start_event) {
  const auto values = amber::react::value_read(objects_state, catalog_state, react_state);

  auto &instances = react_state.installations;
  auto &triggers = react_state.triggers;

  auto &models = catalog_state.models;
  auto &scripts = catalog_state.scripts;
  auto authority = amber::authority(std::as_const(objects_state.objects));

  auto &players = perm_state.players;
  auto &grants = perm_state.grants;
  auto &plots = perm_state.plots;

  try {
    const auto object = call.request.target;
    const auto &source = script(scripts, call.arguments.at("script"));
    if (!call.arguments.contains("choices"))
      return detail::result(false,
                            {{"slots", preview(source, object, values.identity, authority, call)},
                             {"requires_commit", true}});
    const auto requested =
        detail::slot_choices(call.arguments.at("choices"), source, objects_state, perm_state, call);
    const auto plot = authority.plot[object.slot()];
    if (plot) {
      if (plot >= plots.rows.size())
        throw std::invalid_argument("unknown current plot");
      const auto allowed = perm::check_land(detail::holder(call), perm::LandVerb::RunScripts,
                                            plots.rows[plot], players, grants, call.step);
      if (!allowed)
        throw detail::PolicyDenied("run_scripts", "plot:" + std::to_string(plot),
                                   std::string(perm::reason_text(allowed.reason)),
                                   "obtain RunScripts from p:" +
                                       std::to_string(plots.rows[plot].owner));
    }
    const auto before = react::installed_scripts(models, instances, values.identity, object);
    auto out =
        detail::result(false, {{"grants", Json::array()}, {"unfilled_slots", Json::array()}});
    if (std::find(before.begin(), before.end(), source.id) == before.end()) {
      auto ids = extras(instances, object);
      ids.push_back(source.id);
      react::set_instances(instances, object, ids, scripts);
      react::project_instance(triggers, instances, scripts, values.terms, object);
      const auto after = react::installed_scripts(models, instances, values.identity, object);
      out.permissions_changed |= synchronize(
          react_state, values, scripts, perm_state, start_event, object, before, after,
          amber::step::Clock{.step = call.step, .hz = call.hz}, call.origin, out.effects);
      out.changed = true;
    }
    auto granted = detail::fill_slots(source, requested, authority, players, grants, call);
    out.reply.update(granted.reply);
    out.changed |= granted.changed;
    out.permissions_changed |= granted.permissions_changed;
    out.result = out.changed ? record::Result::Applied : record::Result::NoEffect;
    out.reply["effect"] = out.changed ? "applied" : "none";
    if (out.changed)
      out.changed_objects.push_back(object);
    return out;
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
step::NativeCompletion uninstall_script(const step::NativeCall &call,
                                        amber::react::State &react_state,
                                        const react::ValueRead &values,
                                        const amber::catalog::State &catalog_state,
                                        amber::perm::State &perm_state, TermId start_event) {
  auto &instances = react_state.installations;
  auto &triggers = react_state.triggers;

  auto &models = catalog_state.models;
  auto &scripts = catalog_state.scripts;

  try {
    const auto object = call.request.target;
    const auto &source = script(scripts, call.arguments.at("script"));
    auto ids = extras(instances, object);
    const auto found = std::find(ids.begin(), ids.end(), source.id);
    if (found == ids.end())
      return detail::result(false, {{"reason", "not an explicit instance script"}});
    const auto before = react::installed_scripts(models, instances, values.identity, object);
    ids.erase(found);
    react::set_instances(instances, object, ids, scripts);
    react::project_instance(triggers, instances, scripts, values.terms, object);
    const auto after = react::installed_scripts(models, instances, values.identity, object);
    auto out = detail::result(true);
    out.permissions_changed |=
        synchronize(react_state, values, scripts, perm_state, start_event, object, before, after,
                    amber::step::Clock{.step = call.step, .hz = call.hz}, call.origin, out.effects);
    out.changed_objects.push_back(object);
    return out;
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
step::NativeCompletion model_script(const step::NativeCall &call, amber::react::State &react_state,
                                    const react::ValueRead &values,
                                    amber::catalog::State &catalog_state,
                                    amber::perm::State &perm_state, TermId start_event) {
  auto &instances = react_state.installations;
  auto &triggers = react_state.triggers;

  auto &models = catalog_state.models;
  auto &scripts = catalog_state.scripts;

  try {
    const auto model = detail::model(models, perm_state.players, call);
    const auto replacement = call.arguments.at("script").is_null()
                                 ? ScriptId{}
                                 : script(scripts, call.arguments.at("script")).id;
    if (models.rows[model].script == replacement)
      return detail::result(false);
    const auto objects = detail::instances(values.identity, model);
    std::vector<std::vector<ScriptId>> before;
    before.reserve(objects.size());
    for (auto object : objects)
      before.push_back(react::installed_scripts(models, instances, values.identity, object));
    models.rows[model].script = replacement;
    react::project_model(triggers, models, scripts, values.terms, model);
    auto out = detail::result(true);
    for (std::size_t index = 0; index < objects.size(); ++index) {
      const auto after =
          react::installed_scripts(models, instances, values.identity, objects[index]);
      out.permissions_changed |= synchronize(
          react_state, values, scripts, perm_state, start_event, objects[index], before[index],
          after, amber::step::Clock{.step = call.step, .hz = call.hz}, call.origin, out.effects);
    }
    out.changed_objects = objects;
    out.changed_models.push_back(model);
    return out;
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
void rebind_scripts(const amber::catalog::State &catalog_state, amber::react::State &react_state,
                    const react::ValueRead &values, amber::perm::State &perm_state,
                    TermId start_event, const amber::step::Clock &clock,
                    const amber::Origin &origin, NativeCompletion &out) {
  const auto now = clock.step;
  const auto hz = clock.hz;

  auto &models = catalog_state.models;
  auto &scripts = catalog_state.scripts;
  auto &instances = react_state.installations;

  auto &triggers = react_state.triggers;

  const auto identity = values.identity;
  const auto unique = [](auto &rows) {
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
  };
  unique(out.changed_models);
  unique(out.changed_scripts);
  std::vector<Handle> rebound;
  for (const auto &binding : out.rebound_objects)
    rebound.push_back(binding.object);
  // Definition edits are rare; inspect their actual model and instance rows.
  for (const auto &model : models.rows)
    if (model.id &&
        (std::binary_search(out.changed_models.begin(), out.changed_models.end(), model.id) ||
         (model.script && std::binary_search(out.changed_scripts.begin(), out.changed_scripts.end(),
                                             model.script)))) {
      react::project_model(triggers, models, scripts, values.terms, model.id);
      for (std::uint32_t slot = 1; slot < identity.model.size(); ++slot)
        if (identity.model[slot] == model.id)
          rebound.push_back(identity.at(slot));
    }
  unique(rebound);
  std::erase_if(rebound, [&](Handle object) { return !identity.valid(object); });
  unique(out.created_objects);
  out.changed_objects.insert(out.changed_objects.end(), rebound.begin(), rebound.end());
  out.changed_objects.insert(out.changed_objects.end(), out.created_objects.begin(),
                             out.created_objects.end());
  unique(out.changed_objects);
  std::erase_if(out.changed_objects, [&](Handle object) { return !identity.valid(object); });
  if (!rebound.empty()) {
    for (const auto &binding : out.rebound_objects) {
      std::vector<ScriptId> before;
      if (const auto old = models.rows[binding.before.model].script)
        before.push_back(old);
      if (const auto *extra = instances.get(binding.object.slot()))
        for (auto script : *extra)
          if (std::find(before.begin(), before.end(), script) == before.end())
            before.push_back(script);
      const auto after = react::installed_scripts(models, instances, identity, binding.object);
      out.permissions_changed |=
          synchronize(react_state, values, scripts, perm_state, start_event, binding.object, before,
                      after, clock, origin, out.effects);
    }
    react::rebind_reactions(catalog_state, react_state, values, rebound, now, hz, origin,
                            out.effects);
  }
  if (!out.created_objects.empty())
    react::install_reactions(catalog_state, react_state, values, out.created_objects, now, hz,
                             origin, out.effects);
}
} // namespace amber::step::editing
