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
  return view_detail::describe(catalog_state, react_state, values, object, now, hz, inspected);
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
    if (!std::holds_alternative<react::MechanicAction>(decl.operation))
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
void view_changed(SessionViews &views, net::SessionId session, Vec3 eye) {
  views.rows[session].eye = eye;
}
void invalidate(SessionViews &views, std::span<const Handle> changed) {
  for (auto &[session, row] : views.rows) {
    (void)session;
    if (!row.pending.ready)
      continue;
    for (const auto object : changed)
      if (row.body == object || row.acquired.contains(object.bits)) {
        row.pending.ready = false;
        break;
      }
  }
}
void remove_session(SessionViews &views, observe::ActiveViews &active, observe::Hearing &hearing,
                    net::SessionId session) {
  views.rows.erase(session);
  observe::remove_view(active, session);
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
void refresh_view(const amber::catalog::State &catalog_state,
                  const amber::react::State &react_state, SessionViews &sessions,
                  amber::space::State &space_state, const amber::ObjectState &objects_state,
                  const amber::step::State &step_state, observe::Settings settings,
                  double world_ambient_lux, const Authenticated &recipient, std::size_t limit) {
  const auto &intents = step_state.intents;
  const auto &clock = step_state.clock;
  const auto session = recipient.session;
  const auto &principal = recipient.principal;

  const auto values =
      amber::react::value_read(objects_state, catalog_state, react_state, space_state);

  const auto now = clock.step;
  const auto hz = clock.hz;

  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;
  auto &assets = catalog_state.assets;
  auto &scripts = catalog_state.scripts;

  auto &instances = react_state.installations;
  auto &native = react_state.native_declarations;
  auto &inspection_terms = react_state.inspection_terms;

  auto &active = space_state.active_views;
  auto transforms = amber::transforms(std::as_const(objects_state.objects));

  auto &attached_to = space_state.physical_relations.attached_to;

  auto &zones = space_state.zones;

  const auto identity = values.identity;
  if (!identity.valid(principal.body))
    throw std::invalid_argument("Player has no live body");
  const auto &asset = view_detail::asset(models, parameters, assets, values, principal.body);
  const auto authored = observer_binding(asset, values.terms);
  if (!authored)
    throw std::invalid_argument("Body has no authored observer binding");
  const auto observer = net::observer(*authored, principal.body, transforms, values, now, hz);
  Handle held;
  if (const auto *incoming = attached_to.in.get(principal.body.slot()))
    for (const auto object : *incoming)
      if (identity.valid(object) && (identity.flags[object.slot()] & Held)) {
        held = object;
        break;
      }
  const auto scan =
      observe::scan(objects_state, observer, held, settings, space_state, world_ambient_lux, limit);
  auto &row = sessions.rows[session];
  row.player = principal.player;
  row.body = principal.body;
  row.eye = observer.eye;
  PendingView next;
  next.ready = true;
  next.step = now;
  next.limit = limit;
  next.self.body = principal.body;
  next.self.holding = held;
  next.self.position = transforms.position[principal.body.slot()];
  next.self.facing_degrees =
      std::atan2(observer.forward.x, observer.forward.y) * 180.0 / std::numbers::pi;
  next.self.intent = nullptr;
  if (const auto intent = intents.moving.find(principal.body.bits);
      intent != intents.moving.end()) {
    Json goal;
    if (const auto *point = std::get_if<Vec3>(&intent->second.goal))
      goal = view_detail::vector(*point);
    else
      goal = to_string(std::get<Handle>(intent->second.goal));
    next.self.intent = {{"id", intent->second.id}, {"kind", "move_to"}, {"goal", goal}};
  }
  auto ordered_zones = scan.zones;
  std::sort(ordered_zones.begin(), ordered_zones.end(), [&](Handle left, Handle right) {
    const auto *a = zones.rows.get(left.slot());
    const auto *b = zones.rows.get(right.slot());
    const auto pa = a && a->zone == left ? a->priority : 0;
    const auto pb = b && b->zone == right ? b->priority : 0;
    return pa != pb ? pa > pb : left < right;
  });
  for (const auto zone : ordered_zones) {
    auto description =
        view_detail::describe(catalog_state, react_state, values, zone, now, hz, inspection_terms);
    next.self.zones.emplace_back(zone, description.says.name);
  }
  auto prepare = [&](const auto &seen, auto &output) {
    for (const auto &thing : seen)
      output.push_back(
          {thing, seen_description(catalog_state, react_state, values, thing.object, now, hz)});
  };
  prepare(scan.things, next.things);
  prepare(scan.far, next.far);
  next.body_actions = object_actions(models, scripts, instances, native, values, principal.body);
  next.player_actions = player_actions(native, values);
  row.pending = std::move(next);
  const auto tracked = active.rows.find(session);
  if (tracked == active.rows.end() || !(tracked->second.eye == observer.eye))
    observe::track_view(active, session, observer.eye, settings.sight_m);
  active.dirty.erase(session);
}
net::Reply read_look(const amber::catalog::State &catalog_state,
                     const amber::react::State &react_state, SessionViews &sessions,
                     amber::space::State &space_state, const amber::ObjectState &objects_state,
                     const amber::step::State &step_state, observe::Settings settings,
                     double world_ambient_lux, const net::Invocation &call) {

  const auto &clock = step_state.clock;

  const auto now = clock.step;
  const auto hz = clock.hz;

  auto &active = space_state.active_views;
  auto &hearing = space_state.hearing;

  try {
    const auto limit = call.request.fields.value("limit", settings.things_max);
    auto &row = sessions.rows[call.session];
    if (!row.pending.ready || row.pending.step != now || row.pending.limit < limit ||
        active.dirty.contains(call.session))
      refresh_view(catalog_state, react_state, sessions, space_state, objects_state, step_state,
                   settings, world_ambient_lux, net::Authenticated{call.session, call.principal},
                   limit);
    auto &pending = row.pending;
    const auto things = std::span<const net::PreparedThing>(pending.things)
                            .first(std::min(limit, pending.things.size()));
    const auto event_limit =
        call.request.fields.value("events_limit", settings.events_per_response);
    auto result = net::observation(pending.step, hz, pending.self, things, pending.far,
                                   pending.body_actions, pending.player_actions);
    row.last_observation.clear();
    row.last_observation.push_back(call.principal.body);
    row.acquired.insert(call.principal.body.bits);
    for (const auto rows : {things, std::span<const net::PreparedThing>(pending.far)})
      for (const auto &thing : rows) {
        row.last_observation.push_back(thing.seen.object);
        row.acquired.insert(thing.seen.object.bits);
      }
    for (const auto &[zone, name] : pending.self.zones) {
      (void)name;
      if (std::find(row.last_observation.begin(), row.last_observation.end(), zone) ==
          row.last_observation.end())
        row.last_observation.push_back(zone);
      row.acquired.insert(zone.bits);
    }
    append_heard(result, hearing, call.session, event_limit);
    return net::Reply::result(std::move(result));
  } catch (const std::exception &error) {
    return view_detail::error(error.what());
  }
}
} // namespace amber::net
