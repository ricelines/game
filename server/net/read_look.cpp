#include "catalog/catalog.hpp"
#include "step/step.hpp"
#include "view_private.hpp"

namespace amber::net {
namespace {
void referenced_settings(const Json &value, const react::Terms &terms,
                         std::unordered_set<TermId> &selected) {
  if (value.is_string()) {
    const auto found = terms.by_name.find(value.get_ref<const std::string &>());
    if (found != terms.by_name.end() && terms.rows[found->second].source == react::Source::Setting)
      selected.insert(found->second);
  } else if (value.is_object()) {
    for (auto field = value.begin(); field != value.end(); ++field) {
      // Explicit term fields may use IDs; ordinary numeric scalar bindings are
      // constants, not term IDs.
      if (field.key() == "term" && field->is_number_unsigned()) {
        const auto id = field->get<std::uint64_t>();
        if (id < terms.rows.size() && terms.rows[id].source == react::Source::Setting)
          selected.insert(static_cast<TermId>(id));
      } else
        referenced_settings(field.value(), terms, selected);
    }
  } else if (value.is_array())
    for (const auto &item : value)
      referenced_settings(item, terms, selected);
}
ObjectDescription seen_description(const amber::catalog::State &catalog_state,
                                   const amber::react::State &react_state,
                                   const react::ValueRead &values, Handle object, Step now,
                                   double hz) {
  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;
  auto &assets = catalog_state.assets;

  auto &inspection_terms = react_state.inspection_terms;

  const auto &asset = view_detail::asset(models, parameters, assets, values, object);
  std::unordered_set<TermId> selected(asset.replicated.begin(), asset.replicated.end());
  referenced_settings(asset.manifest.at("bindings"), values.terms, selected);
  const auto model = values.model_settings.rows.find(values.identity.model[object.slot()]);
  const auto plot = values.plot_settings.rows.find(values.plots[object.slot()]);
  std::vector<TermId> inspected;
  inspected.reserve(inspection_terms.size());
  for (const auto id : inspection_terms) {
    const auto &term = values.terms.rows[id];
    const bool instance = id < values.instance_settings.columns.size() &&
                          values.instance_settings.columns[id].get(object.slot());
    if (term.source != react::Source::Setting || term.combine != react::Combine::None || instance ||
        (model != values.model_settings.rows.end() && model->second.contains(id)) ||
        (plot != values.plot_settings.rows.end() && plot->second.contains(id)) ||
        selected.contains(id))
      inspected.push_back(id);
  }
  return view_detail::describe(catalog_state, values, object, now, hz, inspected);
}
} // namespace
std::vector<TermId> object_action_ids(const catalog::Models &models,
                                      const catalog::Scripts &scripts,
                                      const react::InstanceScripts &instances,
                                      const react::ValueRead &values, Handle object) {
  std::vector<TermId> result;
  for (const auto id : react::installed_scripts(models, instances, values.identity, object)) {
    const auto &script = scripts.rows.at(id);
    if (!script.declaration.active)
      continue;
    for (const auto &action : script.declaration.actions)
      result.push_back(action.action);
  }
  std::sort(result.begin(), result.end(), [&](TermId a, TermId b) {
    return values.terms.rows[a].name < values.terms.rows[b].name;
  });
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}
std::vector<TermId> player_action_ids(const react::NativeActions &native,
                                      const react::ValueRead &values) {
  std::vector<TermId> result;
  for (const auto &[id, decl] : native.rows) {
    if (decl.space.empty() && !std::holds_alternative<react::MechanicAction>(decl.operation))
      result.push_back(id);
  }
  std::sort(result.begin(), result.end(), [&](TermId a, TermId b) {
    return values.terms.rows[a].name < values.terms.rows[b].name;
  });
  return result;
}
std::vector<std::string> object_actions(const catalog::Models &models,
                                        const catalog::Scripts &scripts,
                                        const react::InstanceScripts &instances,
                                        const react::NativeActions &native,
                                        const react::ValueRead &values, Handle object) {
  std::vector<std::string> result;
  for (auto id : object_action_ids(models, scripts, instances, values, object))
    result.push_back(react::action_signature(values.terms, native, id));
  return result;
}
std::vector<std::string> player_actions(const react::NativeActions &native,
                                        const react::ValueRead &values) {
  std::vector<std::string> result;
  for (auto id : player_action_ids(native, values))
    result.push_back(react::action_signature(values.terms, native, id));
  return result;
}
void remove_session(SessionViews &views, observe::Hearing &hearing, net::SessionId session) {
  views.rows.erase(session);
  observe::remove_listener(hearing, session);
}
void append_heard(Json &result, observe::Hearing &hearing, net::SessionId session,
                  std::size_t limit) {
  std::size_t omitted = 0;
  const auto heard = observe::take_heard(hearing, session, limit, omitted);
  if (!result.contains("events"))
    result["events"] = Json::array();
  if (!result.contains("heard"))
    result["heard"] = Json::array();
  for (const auto &event : heard) {
    if (event.sound.speech)
      result["heard"].push_back(
          {{"from", event.sound.speaker ? "p:" + std::to_string(event.sound.speaker)
                                        : to_string(event.sound.source)},
           {"name", event.sound.name},
           {"text", net::event_payload_json(event.sound.payload)},
           {"dist_m", event.distance_m}});
    else
      result["events"].push_back({{"kind", "sound"},
                                  {"source", to_string(event.sound.source)},
                                  {"at", view_detail::vector(event.sound.at)},
                                  {"dist_m", event.distance_m},
                                  {"speaker", event.sound.speaker},
                                  {"data", net::event_payload_json(event.sound.payload)},
                                  {"root", event.sound.origin.root}});
  }
  result["events_omitted"] = result.value("events_omitted", std::size_t{}) + omitted;
}
namespace {
Json describe_intent(const step::Intents &intents, Handle body) {
  const auto found = intents.moving.find(body.bits);
  if (found == intents.moving.end())
    return nullptr;
  const auto &intent = found->second;
  Json goal;
  if (const auto *point = std::get_if<Vec3>(&intent.goal))
    goal = view_detail::vector(*point);
  else
    goal = to_string(std::get<Handle>(intent.goal));
  return {{"id", intent.id}, {"kind", "move_to"}, {"goal", std::move(goal)}};
}
std::vector<std::pair<Handle, std::string>> describe_zones(const catalog::State &catalog,
                                                           IdentityRead identity,
                                                           const observe::ZoneAmbients &zones,
                                                           std::vector<Handle> selected) {
  std::sort(selected.begin(), selected.end(), [&](Handle left, Handle right) {
    const auto *a = zones.rows.get(left.slot());
    const auto *b = zones.rows.get(right.slot());
    const auto pa = a && a->zone == left ? a->priority : 0;
    const auto pb = b && b->zone == right ? b->priority : 0;
    return pa != pb ? pa > pb : left < right;
  });
  std::vector<std::pair<Handle, std::string>> result;
  result.reserve(selected.size());
  for (const auto zone : selected)
    result.emplace_back(zone, object_text(catalog, zone, identity.model[zone.slot()]).name);
  return result;
}
std::vector<PreparedThing> describe_things(const catalog::State &catalog_state,
                                           const react::State &react_state,
                                           const react::ValueRead &values,
                                           std::span<const observe::Seen> seen,
                                           const step::Clock &clock) {
  std::vector<PreparedThing> result;
  result.reserve(seen.size());
  for (const auto &thing : seen) {
    auto description =
        seen_description(catalog_state, react_state, values, thing.object, clock.step, clock.hz);
    if (thing.reachable)
      description.actions =
          object_actions(catalog_state.models, catalog_state.scripts, react_state.installations,
                         react_state.native_declarations, values, thing.object);
    result.push_back({thing, std::move(description)});
  }
  return result;
}
} // namespace
net::Reply read_look(const amber::catalog::State &catalog_state,
                     const amber::react::State &react_state, SessionViews &sessions,
                     amber::space::State &space_state, const amber::ObjectState &objects_state,
                     const amber::step::State &step_state, observe::Settings settings,
                     double world_ambient_lux, const net::Invocation &call) {
  const auto &clock = step_state.clock;
  const auto values = react::value_read(objects_state, catalog_state, react_state);
  const auto transforms = amber::transforms(std::as_const(objects_state.objects));
  const auto body = call.principal.body;
  try {
    if (!values.identity.valid(body))
      throw std::invalid_argument("Player has no live body");
    const auto &asset = view_detail::asset(catalog_state.models, catalog_state.parameters,
                                           catalog_state.assets, values, body);
    const auto authored = observer_binding(asset, values.terms);
    if (!authored)
      throw std::invalid_argument("Body has no authored observer binding");
    const auto observer = net::observer(*authored, body, transforms, values, clock.step, clock.hz);
    Handle held;
    if (const auto *incoming = space_state.physical_relations.attached_to.in.get(body.slot()))
      for (const auto object : *incoming)
        if (values.identity.valid(object) && (values.identity.flags[object.slot()] & Held)) {
          held = object;
          break;
        }
    const auto limit = call.request.fields.value("limit", settings.things_max);
    auto scan = observe::scan(objects_state, observer, held, settings, space_state,
                              world_ambient_lux, limit);
    const SelfDescription self{
        body,
        held,
        transforms.position[body.slot()],
        static_cast<float>(std::atan2(observer.forward.x, observer.forward.y) * 180.0 /
                           std::numbers::pi),
        describe_intent(step_state.intents, body),
        describe_zones(catalog_state, values.identity, space_state.zones, std::move(scan.zones))};
    const auto things = describe_things(
        catalog_state, react_state, values,
        std::span<const observe::Seen>(scan.things).first(std::min(limit, scan.things.size())),
        clock);
    const auto far = describe_things(catalog_state, react_state, values, scan.far, clock);
    const auto body_actions =
        object_actions(catalog_state.models, catalog_state.scripts, react_state.installations,
                       react_state.native_declarations, values, body);
    const auto player_actions = net::player_actions(react_state.native_declarations, values);
    auto result =
        net::observation(clock.step, clock.hz, self, things, far, body_actions, player_actions);
    result["action_spaces"] = view_detail::action_spaces(react_state);
    auto &row = sessions.rows[call.session];
    row.player = call.principal.player;
    row.body = body;
    row.eye = observer.eye;
    row.last_observation.clear();
    row.last_observation.push_back(body);
    for (const auto *rows : {&things, &far})
      for (const auto &thing : *rows)
        row.last_observation.push_back(thing.seen.object);
    for (const auto &[zone, name] : self.zones) {
      (void)name;
      if (std::find(row.last_observation.begin(), row.last_observation.end(), zone) ==
          row.last_observation.end())
        row.last_observation.push_back(zone);
    }
    append_heard(result, space_state.hearing, call.session,
                 call.request.fields.value("events_limit", settings.events_per_response));
    return net::Reply::result(std::move(result));
  } catch (const std::exception &error) {
    return view_detail::error(error.what());
  }
}
} // namespace amber::net
