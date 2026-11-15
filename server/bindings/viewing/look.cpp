#include "common.hpp"

namespace amber::bindings::viewing {
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
observe::ObjectDescription seen_description(const CatalogRead &catalog, Handle object, Step now,
                                            double hz) {
  const auto &values = catalog.values;
  const auto &asset = detail::asset(catalog, object);
  std::unordered_set<TermId> selected(asset.replicated.begin(), asset.replicated.end());
  referenced_settings(asset.manifest.at("bindings"), values.terms, selected);
  const auto model = values.model_settings.rows.find(values.identity.model[object.slot()]);
  const auto plot = values.plot_settings.rows.find(values.plots[object.slot()]);
  std::vector<TermId> inspected;
  inspected.reserve(catalog.inspection_terms.size());
  for (const auto id : catalog.inspection_terms) {
    const auto &term = values.terms.rows[id];
    const bool instance = id < values.instance_settings.columns.size() &&
                          values.instance_settings.columns[id].get(object.slot());
    if (term.source != react::Source::Setting || term.combine != react::Combine::None || instance ||
        (model != values.model_settings.rows.end() && model->second.contains(id)) ||
        (plot != values.plot_settings.rows.end() && plot->second.contains(id)) ||
        selected.contains(id))
      inspected.push_back(id);
  }
  return detail::describe(catalog, object, now, hz, inspected);
}
} // namespace
std::vector<TermId> object_action_ids(const CatalogRead &catalog, Handle object) {
  std::vector<TermId> result;
  for (const auto id : install::installed_scripts(catalog.models, catalog.instances,
                                                  catalog.values.identity, object)) {
    const auto &script = catalog.scripts.rows.at(id);
    if (!script.declaration.active)
      continue;
    for (const auto &action : script.declaration.actions)
      result.push_back(action.action);
  }
  std::sort(result.begin(), result.end(), [&](TermId a, TermId b) {
    return catalog.values.terms.rows[a].name < catalog.values.terms.rows[b].name;
  });
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}
std::vector<TermId> player_action_ids(const CatalogRead &catalog) {
  std::vector<TermId> result;
  for (const auto &[id, decl] : catalog.native.rows) {
    (void)decl;
    result.push_back(id);
  }
  std::sort(result.begin(), result.end(), [&](TermId a, TermId b) {
    return catalog.values.terms.rows[a].name < catalog.values.terms.rows[b].name;
  });
  return result;
}
std::vector<std::string> object_actions(const CatalogRead &catalog, Handle object) {
  std::vector<std::string> result;
  for (auto id : object_action_ids(catalog, object))
    result.push_back(command::action_signature(catalog.values.terms, catalog.native, id));
  return result;
}
std::vector<std::string> player_actions(const CatalogRead &catalog) {
  std::vector<std::string> result;
  for (auto id : player_action_ids(catalog))
    result.push_back(command::action_signature(catalog.values.terms, catalog.native, id));
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
  for (const auto &event : heard)
    result["events"].push_back({{"kind", "sound"},
                                {"source", to_string(event.sound.source)},
                                {"at", detail::vector(event.sound.at)},
                                {"dist_m", event.distance_m},
                                {"speaker", event.sound.speaker},
                                {"data", observe::event_payload_json(event.sound.payload)},
                                {"root", event.sound.origin.root}});
  result["events_omitted"] = result.value("events_omitted", std::size_t{}) + omitted;
}
void refresh_view(LookBinding &binding, net::SessionId session, const control::Principal &principal,
                  std::size_t limit) {
  const auto &catalog = binding.catalog;
  const auto identity = catalog.values.identity;
  if (!identity.valid(principal.body))
    throw std::invalid_argument("Player has no live body");
  const auto &asset = detail::asset(catalog, principal.body);
  const auto authored = observer_binding(asset, catalog.values.terms);
  if (!authored)
    throw std::invalid_argument("Body has no authored observer binding");
  const auto observer = bindings::observer(*authored, principal.body, binding.transforms,
                                           catalog.values, binding.now, binding.hz);
  Handle held;
  if (const auto *incoming = binding.attached_to.in.get(principal.body.slot()))
    for (const auto object : *incoming)
      if (identity.valid(object) && (identity.flags[object.slot()] & Held)) {
        held = object;
        break;
      }
  const auto scan = observe::scan(
      binding.space, identity, binding.transforms, binding.declared_parts, observer, held,
      binding.settings, binding.lights, binding.zones, catalog.values, binding.motion, binding.now,
      binding.hz, binding.world_ambient_lux, limit, detail::deadline(binding.query_ms));
  auto &row = binding.sessions.rows[session];
  row.player = principal.player;
  row.body = principal.body;
  row.eye = observer.eye;
  PendingView next;
  next.ready = true;
  next.complete = scan.complete;
  next.step = binding.now;
  next.limit = limit;
  next.self.body = principal.body;
  next.self.holding = held;
  next.self.position = binding.transforms.position[principal.body.slot()];
  next.self.facing_degrees =
      std::atan2(observer.forward.x, observer.forward.y) * 180.0 / std::numbers::pi;
  next.self.intent = nullptr;
  if (const auto intent = binding.intents.moving.find(principal.body.bits);
      intent != binding.intents.moving.end()) {
    Json goal;
    if (const auto *point = std::get_if<Vec3>(&intent->second.goal))
      goal = detail::vector(*point);
    else
      goal = to_string(std::get<Handle>(intent->second.goal));
    next.self.intent = {{"id", intent->second.id}, {"kind", "move_to"}, {"goal", goal}};
  }
  for (const auto zone : scan.zones) {
    auto description = detail::describe(catalog, zone, binding.now, binding.hz);
    next.self.zones.emplace_back(zone, description.says.name);
  }
  auto prepare = [&](const auto &seen, auto &output) {
    for (const auto &thing : seen)
      output.push_back({thing, seen_description(catalog, thing.object, binding.now, binding.hz)});
  };
  prepare(scan.things, next.things);
  prepare(scan.far, next.far);
  next.body_actions = object_actions(catalog, principal.body);
  next.player_actions = player_actions(catalog);
  row.pending = std::move(next);
  const auto tracked = binding.active.rows.find(session);
  if (tracked == binding.active.rows.end() || !(tracked->second.eye == observer.eye))
    observe::track_view(binding.active, session, observer.eye, binding.settings.sight_m);
  binding.active.dirty.erase(session);
  if (binding.interest.changed)
    binding.interest.changed(binding.interest.local, session, observer.eye);
}
control::Reply look_handler(void *local, const control::Invocation &call) {
  auto &binding = *static_cast<LookBinding *>(local);
  try {
    const auto limit = call.request.fields.value("limit", binding.settings.things_max);
    auto &row = binding.sessions.rows[call.session];
    if (!row.pending.ready || row.pending.step != binding.now || row.pending.limit < limit ||
        binding.active.dirty.contains(call.session))
      refresh_view(binding, call.session, call.principal, limit);
    auto &pending = row.pending;
    const auto things = std::span<const observe::PreparedThing>(pending.things)
                            .first(std::min(limit, pending.things.size()));
    const auto event_limit =
        call.request.fields.value("events_limit", binding.settings.events_per_response);
    if (call.request.fields.value("full", false))
      observe::forget(row.sent);
    auto result =
        observe::observation(row.sent, pending.step, binding.hz, pending.self, things, pending.far,
                             {}, pending.body_actions, pending.player_actions);
    result["complete"] = pending.complete;
    row.last_observation.clear();
    row.last_observation.push_back(call.principal.body);
    row.acquired.insert(call.principal.body.bits);
    for (const auto rows : {things, std::span<const observe::PreparedThing>(pending.far)})
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
    append_heard(result, binding.hearing, call.session, event_limit);
    if (binding.observations.record)
      binding.observations.record(binding.observations.local, call.principal.player,
                                  call.principal.body, row.last_observation, result);
    return control::Reply::result(std::move(result));
  } catch (const std::exception &error) {
    return detail::error(error.what());
  }
}
} // namespace amber::bindings::viewing
