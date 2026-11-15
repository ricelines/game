#include "control.hpp"

#include <type_traits>

namespace amber::control {
Sessions::Sessions(const Profiles &profiles, Transport transport, Welcome welcome)
    : profiles_(profiles), transport_(transport), welcome_(welcome) {
  if (!transport_.send || !transport_.close || !transport_.view)
    throw std::runtime_error("control transport requires send, close and view callbacks");
}
void Sessions::register_operation(Operation operation) {
  if (operation.name.empty() || operation.name == "hello" || operation.name == "view" ||
      operation.reply_type.empty() || !operation.handler || !operation.validate ||
      !operation.allowed_modes || (operation.allowed_modes & ~all_modes))
    throw std::runtime_error("invalid control operation registration");
  if (!operations_.emplace(operation.name, std::move(operation)).second)
    throw std::runtime_error("duplicate control operation registration");
}
void Sessions::send(net::SessionId session, Json message) {
  if (sessions_.contains(session))
    transport_.send(transport_.context, session, message.dump());
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
  sessions_.erase(found);
}
void Sessions::reject(net::SessionId id, std::string reason) {
  send(id, {{"t", "bye"}, {"reason", reason}});
  transport_.close(transport_.context, id, 1008, std::move(reason));
  forget(id);
}
void Sessions::accept(const net::Input &input) {
  std::visit(
      [&](const auto &message) {
        using T = std::decay_t<decltype(message)>;
        if constexpr (std::is_same_v<T, net::Connected>) {
          forget(message.session);
          sessions_.try_emplace(message.session);
        } else if constexpr (std::is_same_v<T, net::Disconnected>) {
          forget(message.session);
        } else if constexpr (std::is_same_v<T, net::Control>) {
          const auto found = sessions_.find(message.session);
          if (found == sessions_.end())
            return;
          auto parsed = parse(message.text);
          if (!parsed.error.empty()) {
            if (!found->second.principal)
              reject(message.session, parsed.error);
            else
              error(message.session, parsed.id, parsed.error);
          } else if (auto *value = std::get_if<Hello>(&parsed.message)) {
            hello(message.session, std::move(*value));
          } else {
            request(message.session, std::get<Request>(std::move(parsed.message)));
          }
        }
      },
      input);
}
void Sessions::hello(net::SessionId id, Hello hello) {
  auto &session = sessions_.at(id);
  if (session.principal) {
    reject(id, "already_authenticated");
    return;
  }
  const auto *profile = authenticate(profiles_, hello.key);
  if (!profile) {
    reject(id, "bad_key");
    return;
  }
  if (!(profile->connection_modes & modes(hello.mode))) {
    reject(id, "mode_not_allowed");
    return;
  }
  Principal principal{profile->id,    profile->body, profile->name,
                      profile->roles, hello.mode,    hello.binary};
  if (hello.mode == Mode::Play) {
    const auto old = controlling_.find(profile->id);
    if (old != controlling_.end()) {
      const auto displaced = old->second;
      send(displaced, {{"t", "bye"}, {"reason", "replaced"}});
      transport_.close(transport_.context, displaced, 1000, "replaced");
      forget(displaced);
    }
    controlling_[profile->id] = id;
  }
  session.principal = principal;
  auto welcome = welcome_.fields ? welcome_.fields(welcome_.context, principal) : Json::object();
  if (!welcome.is_object())
    throw std::runtime_error("welcome fields must be an object");
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
  if (request.operation == "view") {
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
    transport_.view(transport_.context, id,
                    {p[0].get<float>(), p[1].get<float>(), p[2].get<float>()});
    if (request.id)
      send(id, {{"t", "result"}, {"id", request.id}, {"ok", true}});
    return;
  }
  const auto found = operations_.find(request.operation);
  if (found == operations_.end()) {
    error(id, request.id, "unknown operation: " + request.operation);
    return;
  }
  const auto &operation = found->second;
  if (!(operation.allowed_modes & modes(session.principal->mode))) {
    error(id, request.id, "operation is not allowed in this connection mode");
    return;
  }
  if (const auto error_text = operation.validate(request.fields); !error_text.empty()) {
    error(id, request.id, error_text);
    return;
  }
  const auto sequence = session.next_sequence++;
  session.ids.emplace(request.id, sequence);
  Pending pending{
      {id, sequence, *session.principal, std::move(request)}, operation.reply_type, operation.lane};
  if (operation.lane == Lane::Query)
    start(id, std::move(pending));
  else {
    session.mutations.push_back(std::move(pending));
    advance(id);
  }
}
void Sessions::advance(net::SessionId id) {
  auto &session = sessions_.at(id);
  if (session.dispatching)
    return;
  session.dispatching = true;
  while (!session.active_mutation && !session.mutations.empty()) {
    auto pending = std::move(session.mutations.front());
    session.mutations.pop_front();
    session.active_mutation = pending.invocation.sequence;
    start(id, std::move(pending));
  }
  session.dispatching = false;
}
void Sessions::start(net::SessionId id, Pending pending) {
  const auto sequence = pending.invocation.sequence;
  const auto operation = operations_.at(pending.invocation.request.operation);
  sessions_.at(id).waiting.emplace(sequence, pending);
  try {
    auto reply = operation.handler(operation.context, pending.invocation);
    if (!reply.deferred)
      complete(id, sequence, std::move(reply.fields));
  } catch (const std::exception &error) {
    complete(id, sequence, {{"ok", false}, {"errors", {error.what()}}});
  }
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
  const auto lane = waiting->second.lane;
  if (!result.is_object())
    result = {{"ok", false}, {"errors", {"operation result must be an object"}}};
  result["t"] = waiting->second.reply_type;
  result["id"] = request_id;
  session.waiting.erase(waiting);
  session.ids.erase(request_id);
  if (lane == Lane::Mutation)
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
} // namespace amber::control
