#pragma once

#include "catalog/catalog.hpp"
#include "step.hpp"

namespace amber::step::elaboration_detail {
PlayerId player(const step::NativeCall &call);
elaborate::RoleAccess access(bool is_elaborator, const step::NativeCall &call);
elaborate::RequestId request_id(const Json &args);
const elaborate::Request &request(const elaborate::Requests &requests, elaborate::RequestId id);
const catalog::Script &script(const catalog::Scripts &scripts, ScriptId id);
std::vector<std::pair<bool, bool>> activation_state(const catalog::Scripts &scripts);
std::vector<ScriptId> changed_activations(const catalog::Scripts &scripts,
                                          std::span<const std::pair<bool, bool>> before);
step::NativeCompletion result(bool changed, Json reply = Json::object());
void record(step::NativeCompletion &out, const step::NativeCall &call, std::string name,
            const elaborate::Request &request);
Json request_json(const elaborate::Request &, const elaborate::GapCounts &);
} // namespace amber::step::elaboration_detail
