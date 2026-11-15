#include "run_private.hpp"

#include <algorithm>
#include <stdexcept>

namespace amber::step {
namespace {
using Json = nlohmann::json;
Json initial_catalog(const catalog::Models &models, const catalog::ModelTexts &model_text,
                     const react::Terms &terms, const react::Materials &materials,
                     net::MaterialAppearanceRoles appearance) {
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
          {"models", net::model_metadata(models, model_text, model_ids)},
          {"terms", net::term_metadata(terms, term_ids)},
          {"materials", net::material_metadata(terms, materials, appearance, material_ids)}};
}
void transport_view(net::Server &server, net::ReplicationIndex &replication, IdentityRead ids,
                    AuthorityRead auth, ParentRead parent, const catalog::ModelTexts &model_text,
                    const catalog::InstanceTexts &instance_text, net::SessionId session,
                    Vec3 position) {
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
connection_notice(const net::SessionNotice &notice, net::Sessions &sessions,
                  net::SessionViews &views, observe::ActiveViews &active, observe::Hearing &hearing,
                  net::ReplicationIndex &replication, std::vector<observe::Listener> &listeners,
                  DeliveryTargets &delivery, const catalog::Models &models,
                  const catalog::ModelTexts &model_text, const react::Terms &terms,
                  const react::Materials &materials, net::MaterialAppearanceRoles appearance,
                  IdentityRead identity, TransformRead poses, Step now, double hz, float cell_m,
                  std::uint32_t repl_delay_ms) {
  return std::visit(
      [&](const auto &value) -> std::optional<net::Authenticated> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, net::Authenticated>) {
          sessions.welcome(value.session, {{"step", now},
                                           {"step_hz", hz},
                                           {"cell_m", cell_m},
                                           {"repl_delay_ms", repl_delay_ms},
                                           {"assets", "/assets/"}});
          sessions.send(value.session,
                        initial_catalog(models, model_text, terms, materials, appearance));
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

void answer_query(net::Sessions &sessions, net::Server &server, net::ReplicationIndex &replication,
                  net::SessionViews &views, observe::ActiveViews &active, observe::Hearing &hearing,
                  record::Journal &journal, const catalog::Models &models,
                  const catalog::ParameterBlocks &parameters, const catalog::Assets &assets,
                  const catalog::Scripts &scripts, const catalog::ModelTexts &model_text,
                  const catalog::InstanceTexts &instance_text,
                  const react::InstanceScripts &installations, const react::NativeActions &native,
                  const react::ValueRead &values, const std::vector<TermId> &inspection_terms,
                  const catalog::AuthoredTexts &authored_text, const space::Space &space,
                  TransformRead poses, AuthorityRead authority, ParentRead parents,
                  AttributionRead attribution, const Relation &declared_parts,
                  const Relation &attached_to, const observe::Lights &lights,
                  const observe::ZoneAmbients &zones,
                  const std::vector<observe::MotionSample> &motion, const Intents &intents,
                  const perm::OwnerLimits &owner_limits, const perm::Players &players,
                  const perm::Grants &grants, const perm::Plots &plots, const perm::Ledger &ledger,
                  const react::ScriptStates &states, const react::RuleVersions &rules,
                  const net::RecordFeed &feed, observe::Settings settings, Step now, double hz,
                  const net::Invocation &request) {
  net::Reply reply;
  switch (request.request.kind) {
  case net::ControlKind::Look: {
    reply = net::read_look(models, parameters, assets, scripts, model_text, instance_text,
                           installations, native, values, inspection_terms, authored_text, views,
                           active, hearing, space, poses, declared_parts, attached_to, lights,
                           zones, motion, intents, settings, now, hz, 0, request);
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
    reply = net::read_inspect(models, parameters, assets, scripts, model_text, instance_text,
                              installations, native, values, inspection_terms, authored_text, views,
                              hearing, poses, authority, parents, attribution, values.relations,
                              owner_limits, players, grants, plots, ledger, now, hz,
                              settings.events_per_response, states, rules, request);
    break;
  case net::ControlKind::Actions:
    reply = net::read_actions(models, scripts, installations, native, values, views, hearing, 100,
                              settings.events_per_response, authority, owner_limits, players,
                              grants, now, request);
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
    transport_view(server, replication, values.identity, authority, parents, model_text,
                   instance_text, request.session, views.rows.at(request.session).eye);
}

void send_connection_outputs(std::span<net::SessionOutput> outputs, net::Server &server,
                             net::ReplicationIndex &replication, net::SessionViews &views,
                             IdentityRead identity, AuthorityRead authority, ParentRead parents,
                             const catalog::ModelTexts &model_text,
                             const catalog::InstanceTexts &instance_text) {
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
            transport_view(server, replication, identity, authority, parents, model_text,
                           instance_text, value.session, value.position);
          }
        },
        output);
}

void flush_connections(net::Sessions &sessions, net::Server &server,
                       net::ReplicationIndex &replication, net::SessionViews &views, Inputs &inputs,
                       WorkQueues &queues, IdentityRead identity, AuthorityRead authority,
                       ParentRead parents, const catalog::ModelTexts &model_text,
                       const catalog::InstanceTexts &instance_text) {
  std::vector<net::Invocation> ready;
  std::vector<net::SessionNotice> notices;
  std::vector<net::SessionOutput> outputs;
  sessions.drain(ready, notices, outputs);
  // Completion releases queued mutations; their work enters the next frame.
  for (auto &request : ready)
    queue_connection_mutation(inputs, queues, request);
  send_connection_outputs(outputs, server, replication, views, identity, authority, parents,
                          model_text, instance_text);
}

void refresh_connections(
    const net::Sessions &sessions, net::SessionViews &views, observe::ActiveViews &active,
    std::vector<observe::Listener> &listeners, DeliveryTargets &delivery,
    const catalog::Models &models, const catalog::ParameterBlocks &parameters,
    const catalog::Assets &assets, const catalog::Scripts &scripts,
    const catalog::ModelTexts &model_text, const catalog::InstanceTexts &instance_text,
    const react::InstanceScripts &installations, const react::NativeActions &native,
    const react::ValueRead &values, const std::vector<TermId> &inspection_terms,
    const catalog::AuthoredTexts &authored_text, const space::Space &space, TransformRead poses,
    const Relation &declared_parts, const Relation &attached_to, const observe::Lights &lights,
    const observe::ZoneAmbients &zones, const std::vector<observe::MotionSample> &motion,
    const Intents &intents, observe::Settings settings, Step now, double hz) {
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
      net::refresh_view(models, parameters, assets, scripts, model_text, instance_text,
                        installations, native, values, inspection_terms, authored_text, views,
                        active, space, poses, declared_parts, attached_to, lights, zones, motion,
                        intents, settings, now, hz, 0, session, *principal, settings.things_max);
}
} // namespace amber::step
