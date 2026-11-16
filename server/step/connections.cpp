#include "catalog/catalog.hpp"
#include "run_private.hpp"
#include "settings.hpp"

#include <algorithm>
#include <stdexcept>

namespace amber::step {
namespace {
using Json = nlohmann::json;
Json initial_catalog(const catalog::Models &models, const catalog::ModelTexts &model_text,
                     const react::Terms &terms, const react::Materials &materials,
                     net::MaterialAppearanceRoles appearance, double world_ambient_lux) {
  std::vector<ModelId> model_ids;
  std::vector<TermId> term_ids, material_ids;
  for (const auto &model : models.rows)
    if (model.id)
      model_ids.push_back(model.id);
  for (const auto &term : terms.rows)
    if (term.id) {
      term_ids.push_back(term.id);
      if (term.kind == react::Kind::Material)
        material_ids.push_back(term.id);
    }
  return {{"t", "catalog"},
          {"world", {{"ambient_lux", world_ambient_lux}}},
          {"models", net::model_metadata(models, model_text, model_ids)},
          {"terms", net::term_metadata(terms, term_ids)},
          {"materials", net::material_metadata(terms, materials, appearance, material_ids)}};
}
void transport_view(net::Server &server, net::ReplicationIndex &replication,
                    const amber::ObjectState &objects_state,
                    const amber::catalog::State &catalog_state, net::SessionId session,
                    Vec3 position) {
  auto ids = amber::identity(std::as_const(objects_state.objects));
  auto auth = amber::authority(std::as_const(objects_state.objects));
  auto parent = amber::parents(std::as_const(objects_state.objects));
  auto &model_text = catalog_state.model_text;
  auto &instance_text = catalog_state.instance_text;

  const auto found = replication.interests.find(session);
  const auto previous =
      found == replication.interests.end() ? std::set<net::Cell>{} : found->second;
  net::view_interest(replication, session, position);
  server.view(session, position);
  std::vector<Handle> visible;
  const auto collect = [&](net::Cell cell) {
    if (const auto row = replication.cells.find(cell); row != replication.cells.end())
      for (auto bits : row->second)
        visible.push_back({bits});
  };
  for (const auto cell : replication.interests.at(session))
    if (!previous.contains(cell))
      collect(cell);
  if (previous.empty() && !replication.interests.at(session).contains(net::global_cell))
    collect(net::global_cell);
  if (!visible.empty())
    server.send(session, Json{{"t", "metadata"},
                              {"objects", net::object_metadata(ids, auth, parent, model_text,
                                                               instance_text, visible)}}
                             .dump());
}
} // namespace

std::optional<net::Authenticated>
connection_notice(const net::SessionNotice &notice, amber::net::State &net_state,
                  amber::space::State &space_state, DeliveryTargets &delivery,
                  const amber::catalog::State &catalog_state,
                  const amber::react::State &react_state, net::MaterialAppearanceRoles appearance,
                  const amber::ObjectState &objects_state, const amber::step::Clock &clock,
                  const amber::Settings &settings) {
  const auto cell_m = settings.network.cell_m;
  const auto repl_delay_ms = settings.repl_delay_ms;

  const auto now = clock.step;
  const auto hz = clock.hz;

  auto &sessions = net_state.sessions;
  auto &views = net_state.session_views;
  auto &active = space_state.active_views;
  auto &hearing = space_state.hearing;
  auto &replication = net_state.replication;
  auto &listeners = space_state.listeners;
  auto &models = catalog_state.models;
  auto &model_text = catalog_state.model_text;
  auto &terms = react_state.terms;
  auto &materials = react_state.materials;
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto poses = amber::transforms(std::as_const(objects_state.objects));

  return std::visit(
      [&](const auto &value) -> std::optional<net::Authenticated> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, net::Authenticated>) {
          sessions.welcome(value.session, {{"step", now},
                                           {"step_hz", hz},
                                           {"cell_m", cell_m},
                                           {"repl_delay_ms", repl_delay_ms},
                                           {"assets", "/assets/"}});
          sessions.send(value.session, {{"t", "terrain"},
                                        {"tile_m", settings.space.terrain_tile_m},
                                        {"tiles", space_state.terrain}});
          sessions.send(value.session, initial_catalog(models, model_text, terms, materials,
                                                       appearance, outdoor_lux(react_state)));
          auto &row = views.rows[value.session];
          row.player = value.principal.player;
          row.body = value.principal.body;
          if (identity.valid(row.body)) {
            row.eye = poses.position[row.body.slot()];
            listeners.push_back({value.session, row.body, row.eye});
            if (value.principal.mode == net::Mode::Play) {
              delivery.present_bodies[row.player] = row.body;
              return value;
            }
          }
        } else {
          if (value.principal && value.principal->mode == net::Mode::Play) {
            bool present = false;
            for (auto other : sessions.sessions_for(value.principal->player))
              if (const auto principal = sessions.principal(other);
                  principal && principal->mode == net::Mode::Play)
                present = true;
            if (!present)
              delivery.present_bodies.erase(value.principal->player);
          }
          std::erase_if(listeners,
                        [&](const auto &listener) { return listener.session == value.session; });
          net::remove_view_interest(replication, value.session);
          net::remove_session(views, active, hearing, value.session);
        }
        return {};
      },
      notice);
}

bool queue_connection_mutation(Inputs &inputs, WorkQueues &queues, net::Invocation &request) {
  switch (request.request.kind) {
  case net::ControlKind::Do:
  case net::ControlKind::Cancel:
  case net::ControlKind::RegisterModel:
  case net::ControlKind::RegisterVariant:
  case net::ControlKind::RegisterScript:
  case net::ControlKind::AuthoringFailed:
    break;
  default:
    return false;
  }
  Input input;
  input.kind = request.request.kind;
  input.caller = request.principal.player;
  input.body = request.principal.body;
  input.session = request.session;
  input.sequence = request.sequence;
  input.request = request.request.id;
  input.fields = std::move(request.request.fields);
  auto &queued = queues.turns.emplace_back();
  queued.work.emplace<InputWork>(enqueue_input(inputs, std::move(input)));
  return true;
}

void answer_query(amber::net::State &net_state, amber::space::State &space_state,
                  record::Journal &journal, const amber::catalog::State &catalog_state,
                  const amber::react::State &react_state, const amber::ObjectState &objects_state,
                  const amber::step::State &step_state, const amber::perm::State &perm_state,
                  const amber::Settings &loaded_settings, const net::Invocation &request) {
  auto &server = net_state.server;

  const auto &clock = step_state.clock;
  const auto &settings = loaded_settings.perception;

  const auto values = amber::react::value_read(objects_state, catalog_state, react_state);

  auto &space = space_state.space;
  const auto now = clock.step;
  const auto hz = clock.hz;

  auto &sessions = net_state.sessions;
  auto &replication = net_state.replication;
  auto &views = net_state.session_views;

  auto &hearing = space_state.hearing;
  auto &models = catalog_state.models;

  auto &scripts = catalog_state.scripts;

  auto attribution = amber::attribution(std::as_const(objects_state.objects));

  auto &feed = net_state.feed;

  net::Reply reply;
  switch (request.request.kind) {
  case net::ControlKind::Look: {
    reply = net::read_look(catalog_state, react_state, views, space_state, objects_state,
                           step_state, settings, outdoor_lux(react_state), request);
    if (reply.fields.contains("step")) {
      record::Event event;
      event.step = now;
      event.kind = record::Kind::Observation;
      event.actor = record::Actor::player(request.principal.player);
      event.name = "look";
      event.things = views.rows.at(request.session).last_observation;
      event.detail = {{"observation", reply.fields}};
      event.credits =
          record::credits(values.identity, attribution, models, scripts, event.things, 0, hz);
      journal.append(std::move(event));
    }
    break;
  }
  case net::ControlKind::Inspect:
    reply = net::read_inspect(catalog_state, react_state, views, hearing, objects_state, perm_state,
                              clock, loaded_settings, request);
    break;
  case net::ControlKind::Actions:
    reply = net::read_actions(catalog_state, react_state, objects_state, views, hearing,
                              loaded_settings, perm_state, now, request);
    break;
  case net::ControlKind::Events:
    reply = net::read_events(feed, views, settings.sight_m, settings.events_per_response, request);
    break;
  case net::ControlKind::Debug:
    reply = net::read_debug(space, values.identity, now, settings.sight_m, {}, request);
    break;
  default:
    throw std::logic_error("mutation reached query dispatch");
  }
  sessions.complete(request.session, request.sequence, reply.fields);
  if (request.request.kind == net::ControlKind::Look && request.principal.binary)
    transport_view(server, replication, objects_state, catalog_state, request.session,
                   views.rows.at(request.session).eye);
}

void send_connection_outputs(std::span<net::SessionOutput> outputs, net::Server &server,
                             amber::net::State &net_state, const amber::ObjectState &objects_state,
                             const amber::catalog::State &catalog_state) {
  auto &replication = net_state.replication;
  auto &views = net_state.session_views;

  for (auto &output : outputs)
    std::visit(
        [&](auto &value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, net::Send>)
            server.send(value.session, value.message.dump());
          else if constexpr (std::is_same_v<T, net::Close>)
            server.close(value.session, value.code, std::move(value.reason));
          else {
            net::view_changed(views, value.session, value.position);
            transport_view(server, replication, objects_state, catalog_state, value.session,
                           value.position);
          }
        },
        output);
}

void flush_connections(amber::net::State &net_state, net::Server &server,
                       amber::step::State &step_state, const amber::ObjectState &objects_state,
                       const amber::catalog::State &catalog_state) {
  auto &sessions = net_state.sessions;

  auto &inputs = step_state.inputs;
  auto &queues = step_state.queues;

  std::vector<net::Invocation> ready;
  std::vector<net::SessionNotice> notices;
  std::vector<net::SessionOutput> outputs;
  sessions.drain(ready, notices, outputs);
  // Completion releases queued mutations; their work enters the next frame.
  for (auto &request : ready)
    queue_connection_mutation(inputs, queues, request);
  send_connection_outputs(outputs, server, net_state, objects_state, catalog_state);
}

void refresh_connections(amber::net::State &net_state, amber::space::State &space_state,
                         amber::step::State &step_state, const amber::catalog::State &catalog_state,
                         const amber::react::State &react_state,
                         const amber::ObjectState &objects_state, observe::Settings settings,
                         const amber::step::Clock &clock) {
  const auto values = amber::react::value_read(objects_state, catalog_state, react_state);

  const auto now = clock.step;
  const auto hz = clock.hz;

  auto &sessions = net_state.sessions;
  auto &views = net_state.session_views;
  auto &active = space_state.active_views;
  auto &listeners = space_state.listeners;
  auto &delivery = step_state.delivery;
  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;
  auto &assets = catalog_state.assets;

  auto poses = amber::transforms(std::as_const(objects_state.objects));

  listeners.clear();
  delivery.present_bodies.clear();
  const auto identity = values.identity;
  for (auto &[session, row] : views.rows)
    if (identity.valid(row.body)) {
      const auto effective = catalog::resolve_asset(
          models, parameters, identity.model[row.body.slot()], values.params[row.body.slot()]);
      if (const auto binding = net::observer_binding(assets.rows.at(effective.hash), values.terms))
        row.eye = net::observer(*binding, row.body, poses, values, now, hz).eye;
      listeners.push_back({session, row.body, row.eye});
      if (const auto principal = sessions.principal(session);
          principal && principal->mode == net::Mode::Play)
        delivery.present_bodies[row.player] = row.body;
    }
  for (auto session : observe::take_dirty_views(active))
    if (const auto principal = sessions.principal(session))
      net::refresh_view(catalog_state, react_state, views, space_state, objects_state, step_state,
                        settings, outdoor_lux(react_state), net::Authenticated{session, *principal},
                        settings.things_max);
}
} // namespace amber::step
