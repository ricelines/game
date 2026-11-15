#pragma once
#include "step.hpp"
#include <algorithm>

namespace amber::step::elaboration_detail {
inline PlayerId player(const step::NativeCall &call) {
  const auto *id = std::get_if<PlayerId>(&call.request.caller);
  if (!id || !*id)
    throw std::invalid_argument("elaboration requires a player caller");
  return *id;
}
inline elaborate::RoleAccess access(bool is_elaborator, const step::NativeCall &call) {
  return {player(call), is_elaborator};
}
inline elaborate::RequestId request_id(const Json &args) {
  return step::identifier(args.at("request"), "request:");
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
inline std::vector<std::pair<bool, bool>> activation_state(const catalog::Scripts &scripts) {
  std::vector<std::pair<bool, bool>> result;
  result.reserve(scripts.rows.size());
  for (const auto &script : scripts.rows)
    result.emplace_back(script.declaration.active, script.declaration.approved);
  return result;
}
inline std::vector<ScriptId> changed_activations(const catalog::Scripts &scripts,
                                                 std::span<const std::pair<bool, bool>> before) {
  std::vector<ScriptId> changed;
  for (std::size_t i = 1; i < scripts.rows.size(); ++i)
    if (before[i] !=
        std::pair{scripts.rows[i].declaration.active, scripts.rows[i].declaration.approved})
      changed.push_back(scripts.rows[i].id);
  return changed;
}
inline step::NativeCompletion result(bool changed, Json reply = Json::object()) {
  step::NativeCompletion out;
  out.changed = changed;
  out.result = changed ? record::Result::Applied : record::Result::NoEffect;
  out.reply = {{"ok", true}, {"effect", changed ? "applied" : "none"}};
  out.reply.update(reply);
  return out;
}
inline void record(step::NativeCompletion &out, const step::NativeCall &call, std::string name,
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
Json request_json(const elaborate::Request &, const elaborate::GapCounts &);
} // namespace amber::step::elaboration_detail
