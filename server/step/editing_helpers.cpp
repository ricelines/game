#include "catalog/catalog.hpp"
#include "editing_private.hpp"

#include <algorithm>
#include <charconv>
#include <set>

namespace amber::step::editing::detail {
PolicyDenied::PolicyDenied(std::string_view need, Json on, std::string why, std::string fix)
    : std::runtime_error(why),
      details{
          {"need", need}, {"on", std::move(on)}, {"why", std::move(why)}, {"fix", std::move(fix)}} {
}
perm::Holder holder(const step::NativeCall &call) {
  if (const auto *id = std::get_if<PlayerId>(&call.request.caller))
    return perm::Holder::player(*id);
  const auto &script = std::get<ScriptHolder>(call.request.caller);
  return perm::Holder::script(script.script, script.object);
}
PlayerId player(const step::NativeCall &call) {
  if (const auto *id = std::get_if<PlayerId>(&call.request.caller); id && *id)
    return *id;
  throw PolicyDenied("player_caller",
                     call.request.target ? Json(to_string(call.request.target))
                                         : Json("administration"),
                     "this action requires the actual player caller",
                     "have the authorized player perform this action directly");
}
std::uint32_t identifier(const Json &value, std::string_view prefix) {
  return step::identifier(value, prefix);
}
Handle handle(const Json &value, IdentityRead identity) {
  return step::live_handle(value, identity);
}
ModelId model(const catalog::Models &models, const perm::Players &players,
              const step::NativeCall &call) {
  const auto id = identifier(call.arguments.at("model"), "m:");
  if (id >= models.rows.size() || models.rows[id].id != id)
    throw std::invalid_argument("unknown model");
  const auto *author = std::get_if<PlayerId>(&call.request.caller);
  const auto group = models.rows[id].group;
  if (!author || !*author ||
      (models.rows[id].author != *author &&
       (!group || !std::binary_search(players.rows[*author].groups.begin(),
                                      players.rows[*author].groups.end(), group))))
    throw PolicyDenied("model_author", "m:" + std::to_string(id),
                       "only the model author or a model group member may edit its defaults",
                       "ask p:" + std::to_string(models.rows[id].author) +
                           " to edit these defaults");
  return id;
}
TermId term(const react::Terms &terms, const Json &value) {
  TermId id = 0;
  if (value.is_string()) {
    const auto found = terms.by_name.find(value.get<std::string>());
    if (found != terms.by_name.end())
      id = found->second;
  } else if (value.is_number_unsigned() || value.is_number_integer())
    id = value.get<TermId>();
  if (!id || id >= terms.rows.size())
    throw std::invalid_argument("unknown registered term");
  return id;
}
Value scalar(const Json &value, const react::Term &term) {
  if (term.kind == react::Kind::Flag) {
    if (!value.is_boolean())
      throw std::invalid_argument("flag value must be boolean");
    return Value(value.get<bool>());
  }
  if (term.kind != react::Kind::Quantity || !value.is_number())
    throw std::invalid_argument("quantity value must be numeric");
  return value.is_number_integer() ? Value(value.get<std::int64_t>()) : Value(value.get<double>());
}
step::NativeCompletion result(bool changed, Json extra) {
  step::NativeCompletion out;
  out.changed = changed;
  out.result = changed ? record::Result::Applied : record::Result::NoEffect;
  out.reply = {{"ok", true}, {"effect", changed ? "applied" : "none"}};
  out.reply.update(extra);
  return out;
}
step::NativeCompletion failed(const std::exception &error) {
  step::NativeCompletion out;
  out.result = record::Result::Denied;
  if (const auto *denied = dynamic_cast<const PolicyDenied *>(&error))
    out.reply = {{"ok", false}, {"denied", denied->details}};
  else
    out.reply = {{"ok", false}, {"error", "bad_input"}, {"problem", error.what()}};
  return out;
}
std::vector<Handle> instances(IdentityRead identity, ModelId model) {
  std::vector<Handle> result;
  for (std::uint32_t slot = 1; slot < identity.model.size(); ++slot)
    if (identity.model[slot] == model)
      result.push_back(identity.at(slot));
  return result;
}
void watches(react::DeferredEffects &effects, react::WatchChanges changed) {
  for (auto &work : changed.dirty)
    effects.ordered.push_back(std::move(work));
  for (auto &timer : changed.timers)
    effects.ordered.push_back(std::move(timer));
}
bool role(std::span<const std::string> roles, std::string_view name) {
  return std::find(roles.begin(), roles.end(), name) != roles.end();
}
} // namespace amber::step::editing::detail
