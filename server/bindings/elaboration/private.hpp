#pragma once
#include "bindings/authority/descriptors.hpp"
#include "elaboration.hpp"
#include <algorithm>

namespace amber::bindings::elaboration::detail {
inline PlayerId player(const drive::NativeCall &call) {
  const auto *id = std::get_if<PlayerId>(&call.request.caller);
  if (!id || !*id)
    throw std::invalid_argument("elaboration requires a player caller");
  return *id;
}
inline bool role(const control::Profiles &profiles, PlayerId id, std::string_view wanted) {
  return id < profiles.rows.size() && profiles.rows[id].id == id &&
         std::ranges::find(profiles.rows[id].roles, wanted) != profiles.rows[id].roles.end();
}
inline elaborate::RoleAccess access(const control::Profiles &profiles,
                                    const drive::NativeCall &call) {
  const auto id = player(call);
  return {id, role(profiles, id, "elaborator")};
}
inline elaborate::RequestId request_id(const Json &args) {
  return authority::identifier(args.at("request"), "request:");
}
inline const elaborate::Request &request(const elaborate::Requests &requests,
                                         elaborate::RequestId id) {
  if (!id || id >= requests.rows.size() || requests.rows[id].id != id)
    throw std::invalid_argument("unknown elaboration request");
  return requests.rows[id];
}
inline const catalog::Script &script(const catalog::Scripts &scripts, ScriptId id) {
  if (!id || id >= scripts.rows.size() || scripts.rows[id].id != id ||
      scripts.rows[id].declaration.kind != react::ScriptDeclaration::Kind::Rule)
    throw std::invalid_argument("registered immutable world rule required");
  return scripts.rows[id];
}
inline drive::NativeCompletion result(bool changed, Json reply = Json::object()) {
  drive::NativeCompletion out;
  out.changed = changed;
  out.result = changed ? record::Result::Applied : record::Result::NoEffect;
  out.reply = {{"ok", true}, {"effect", changed ? "applied" : "none"}};
  out.reply.update(reply);
  return out;
}
inline void record(drive::NativeCompletion &out, const drive::NativeCall &call, std::string name,
                   const elaborate::Request &request) {
  record::Event event;
  event.step = call.step;
  event.kind = record::Kind::System;
  event.actor = record::Actor::player(player(call));
  event.name = std::move(name);
  event.result = out.result;
  event.changed = out.changed;
  event.things = {request.target};
  event.detail = {
      {"request", request.id}, {"requester", request.requester}, {"response", out.reply}};
  out.records.push_back(std::move(event));
}
inline void notify(const Notification &sink, const elaborate::CloseOutcome &outcome) {
  if (outcome.requester && sink.send)
    sink.send(sink.local, outcome);
}
Json request_json(const elaborate::Request &, const elaborate::GapCounts &);
} // namespace amber::bindings::elaboration::detail
