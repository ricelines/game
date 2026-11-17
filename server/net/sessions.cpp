#include "net.hpp"

#include <type_traits>

namespace amber::net {
bool join_allowed(const JoinRequest &) {
  return true;
}
Sessions::Sessions(const Profiles &profiles) : profiles_(profiles) {}
void Sessions::drain(std::vector<Invocation> &ready, std::vector<SessionNotice> &notices,
                     std::vector<SessionOutput> &outputs) {
  for (auto &input : ready_) {
    const auto found = sessions_.find(input.session);
    if (found != sessions_.end() && found->second.waiting.contains(input.sequence))
      ready.push_back(std::move(input));
  }
  ready_.clear();
  for (auto &notice : notices_)
    notices.push_back(std::move(notice));
  notices_.clear();
  for (auto &output : outputs_)
    outputs.push_back(std::move(output));
  outputs_.clear();
}
void Sessions::send(net::SessionId session, Json message) {
  if (sessions_.contains(session))
    outputs_.emplace_back(Send{session, std::move(message)});
}
void Sessions::error(net::SessionId session, std::uint64_t id, std::string problem) {
  send(session, {{"t", "result"}, {"id", id}, {"ok", false}, {"errors", {std::move(problem)}}});
}
void Sessions::forget(net::SessionId id) {
  const auto found = sessions_.find(id);
  if (found == sessions_.end())
    return;
  if (found->second.principal) {
    auto controlling = controlling_.find(found->second.principal->player);
    if (controlling != controlling_.end() && controlling->second == id)
      controlling_.erase(controlling);
  }
  notices_.emplace_back(Closed{id, found->second.principal});
  sessions_.erase(found);
}
void Sessions::reject(net::SessionId id, std::string reason) {
  send(id, {{"t", "bye"}, {"reason", reason}});
  outputs_.emplace_back(Close{id, 1008, std::move(reason)});
  forget(id);
}
std::optional<Admission> Sessions::accept(const net::Input &input) {
  return std::visit(
      [&](const auto &message) -> std::optional<Admission> {
        using T = std::decay_t<decltype(message)>;
        if constexpr (std::is_same_v<T, net::Connected>) {
          forget(message.session);
          sessions_.try_emplace(message.session);
        } else if constexpr (std::is_same_v<T, net::Disconnected>) {
          forget(message.session);
        } else if constexpr (std::is_same_v<T, net::Control>) {
          const auto found = sessions_.find(message.session);
          if (found == sessions_.end())
            return {};
          auto parsed = parse(message.text);
          if (!parsed.error.empty()) {
            if (!found->second.principal)
              reject(message.session, parsed.error);
            else
              error(message.session, parsed.id, parsed.error);
          } else if (found->second.principal && !std::holds_alternative<Request>(parsed.message)) {
            reject(message.session, "already_authenticated");
          } else if (auto *value = std::get_if<Hello>(&parsed.message)) {
            hello(message.session, std::move(*value));
          } else if (auto *value = std::get_if<JoinRequest>(&parsed.message)) {
            return Admission{message.session, std::move(*value)};
          } else if (auto *value = std::get_if<OperatorRequest>(&parsed.message)) {
            return Admission{message.session, std::move(*value)};
          } else {
            request(message.session, std::get<Request>(std::move(parsed.message)));
          }
        }
        return {};
      },
      input);
}
void Sessions::hello(net::SessionId id, Hello hello) {
  const auto *profile = authenticate(profiles_, hello.key);
  if (!profile) {
    reject(id, "bad_key");
    return;
  }
  if (!(profile->connection_modes & modes(hello.mode))) {
    reject(id, "mode_not_allowed");
    return;
  }
  notices_.emplace_back(establish(id, *profile, hello.mode, hello.binary));
}
Authenticated Sessions::establish(net::SessionId id, const Profile &profile, Mode mode,
                                  bool binary) {
  Principal principal{profile.id, profile.body, profile.name, mode, binary};
  if (mode == Mode::Play) {
    const auto old = controlling_.find(profile.id);
    if (old != controlling_.end()) {
      const auto displaced = old->second;
      send(displaced, {{"t", "bye"}, {"reason", "replaced"}});
      outputs_.emplace_back(Close{displaced, 1000, "replaced"});
      forget(displaced);
    }
    controlling_[profile.id] = id;
  }
  sessions_.at(id).principal = principal;
  return {id, std::move(principal)};
}
void Sessions::welcome(net::SessionId id, Json welcome) {
  const auto selected = principal(id);
  if (!selected)
    return;
  const auto &principal = *selected;
  welcome["t"] = "welcome";
  welcome["version"] = 1;
  welcome["session"] = id;
  welcome["player"] = "p:" + std::to_string(principal.player);
  welcome["body"] = principal.body ? Json(to_string(principal.body)) : Json(nullptr);
  welcome["mode"] = mode_name(principal.mode);
  welcome["binary"] = principal.binary;
  send(id, std::move(welcome));
}
void Sessions::request(net::SessionId id, Request request) {
  auto &session = sessions_.at(id);
  if (!session.principal) {
    reject(id, "hello_required");
    return;
  }
  if (session.ids.contains(request.id)) {
    error(id, request.id, "request id is already outstanding");
    return;
  }
  if (request.kind == ControlKind::View) {
    const auto &fields = request.fields;
    if (fields.size() != 1 || !fields.contains("pos") || !fields["pos"].is_array() ||
        fields["pos"].size() != 3) {
      error(id, request.id, "view requires pos with three finite numbers");
      return;
    }
    for (const auto &value : fields["pos"])
      if (!value.is_number() || !std::isfinite(value.get<float>())) {
        error(id, request.id, "view requires pos with three finite numbers");
        return;
      }
    if (!session.principal->binary) {
      error(id, request.id, "binary replication was not requested in hello");
      return;
    }
    ++session.next_sequence;
    const auto &p = fields["pos"];
    outputs_.emplace_back(View{id, {p[0].get<float>(), p[1].get<float>(), p[2].get<float>()}});
    if (request.id)
      send(id, {{"t", "result"}, {"id", request.id}, {"ok", true}});
    return;
  }
  const bool mutation =
      request.kind == ControlKind::Do || request.kind == ControlKind::Cancel ||
      request.kind == ControlKind::RegisterModel || request.kind == ControlKind::RegisterVariant ||
      request.kind == ControlKind::RegisterScript || request.kind == ControlKind::AuthoringFailed;
  if ((mutation && session.principal->mode != Mode::Play) ||
      (request.kind == ControlKind::Debug && session.principal->mode != Mode::Debug)) {
    error(id, request.id, "operation is not allowed in this connection mode");
    return;
  }
  std::string problem;
  switch (request.kind) {
  case ControlKind::Look:
    problem = validate_look(request.fields);
    break;
  case ControlKind::Inspect:
    problem = validate_inspect(request.fields);
    break;
  case ControlKind::Actions:
    problem = validate_actions(request.fields);
    break;
  case ControlKind::Do:
    problem = validate_do(request.fields);
    break;
  case ControlKind::Cancel:
    problem = validate_cancel(request.fields);
    break;
  case ControlKind::RegisterModel:
    problem = validate_register_model(request.fields);
    break;
  case ControlKind::RegisterVariant:
    problem = validate_register_variant(request.fields);
    break;
  case ControlKind::RegisterScript:
    problem = validate_register_script(request.fields);
    break;
  case ControlKind::AuthoringFailed:
    problem = validate_authoring_failed(request.fields);
    break;
  case ControlKind::Events:
    problem = validate_events(request.fields);
    break;
  case ControlKind::Debug:
    problem = validate_debug(request.fields);
    break;
  case ControlKind::View:
    break;
  }
  if (!problem.empty()) {
    error(id, request.id, std::move(problem));
    return;
  }
  const auto sequence = session.next_sequence++;
  session.ids.emplace(request.id, sequence);
  Pending pending{{id, sequence, *session.principal, std::move(request)}, mutation};
  if (!mutation)
    start(id, std::move(pending));
  else {
    session.mutations.push_back(std::move(pending));
    advance(id);
  }
}
void Sessions::advance(net::SessionId id) {
  auto &session = sessions_.at(id);
  if (!session.active_mutation && !session.mutations.empty()) {
    auto pending = std::move(session.mutations.front());
    session.mutations.pop_front();
    session.active_mutation = pending.invocation.sequence;
    start(id, std::move(pending));
  }
}
void Sessions::start(net::SessionId id, Pending pending) {
  const auto sequence = pending.invocation.sequence;
  ready_.push_back(pending.invocation);
  sessions_.at(id).waiting.emplace(sequence, std::move(pending));
}
void Sessions::complete(net::SessionId id, std::uint64_t sequence, Json result) {
  const auto found = sessions_.find(id);
  if (found == sessions_.end())
    return;
  auto &session = found->second;
  const auto waiting = session.waiting.find(sequence);
  if (waiting == session.waiting.end())
    return;
  const auto request_id = waiting->second.invocation.request.id;
  const auto mutation = waiting->second.mutation;
  const auto kind = waiting->second.invocation.request.kind;
  if (!result.is_object())
    result = {{"ok", false}, {"errors", {"operation result must be an object"}}};
  switch (kind) {
  case ControlKind::Look:
    result["t"] = "obs";
    break;
  case ControlKind::Inspect:
    result["t"] = "inspection";
    break;
  case ControlKind::Actions:
    result["t"] = "actions";
    break;
  case ControlKind::Events:
    result["t"] = "events";
    break;
  case ControlKind::Debug:
    result["t"] = "debug";
    break;
  default:
    result["t"] = "result";
    break;
  }
  result["id"] = request_id;
  session.waiting.erase(waiting);
  session.ids.erase(request_id);
  if (mutation)
    session.active_mutation.reset();
  send(id, std::move(result));
  advance(id);
}
void Sessions::event(net::SessionId id, Json record) {
  const auto found = sessions_.find(id);
  if (found == sessions_.end() || !found->second.principal ||
      found->second.principal->mode != Mode::Play)
    return;
  record["t"] = "event";
  send(id, std::move(record));
}
std::vector<net::SessionId> Sessions::sessions_for(PlayerId player) const {
  std::vector<net::SessionId> selected;
  for (const auto &[id, session] : sessions_)
    if (session.principal && session.principal->player == player)
      selected.push_back(id);
  return selected;
}
void Sessions::event_for_player(PlayerId player, const Json &record) {
  for (auto id : sessions_for(player))
    event(id, record);
}
void Sessions::broadcast(Json message, Modes recipients) {
  for (const auto &[id, session] : sessions_)
    if (session.principal && (modes(session.principal->mode) & recipients))
      send(id, message);
}
std::optional<Principal> Sessions::principal(net::SessionId id) const {
  const auto found = sessions_.find(id);
  return found == sessions_.end() ? std::nullopt : found->second.principal;
}
} // namespace amber::net
