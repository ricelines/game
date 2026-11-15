#pragma once

#include "bindings/authority/descriptors.hpp"
#include "bindings/metadata/metadata.hpp"
#include "editing.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>

namespace amber::bindings::editing::detail {
inline PlayerId player(const drive::NativeCall &call) {
  if (const auto *id = std::get_if<PlayerId>(&call.request.caller); id && *id)
    return *id;
  throw std::invalid_argument("this administration action requires a player caller");
}
inline std::uint32_t identifier(const Json &value, std::string_view prefix) {
  return authority::identifier(value, prefix);
}
inline Handle handle(const Json &value, IdentityRead identity) {
  return authority::live_handle(value, identity);
}
inline void edit(const EditAccess &access, const drive::NativeCall &call) {
  const auto who = player(call);
  if (!access.identity.valid(call.request.target))
    throw std::invalid_argument("stale edited object");
  const auto target =
      perm::target(access.identity, access.authority, access.limits, call.request.target);
  const auto allowed = perm::check(perm::Holder::player(who), perm::Verb::Edit, target,
                                   access.players, access.grants, call.step);
  if (!allowed)
    throw std::invalid_argument("edit denied: " + std::string(perm::reason_text(allowed.reason)));
}
inline ModelId model(const catalog::Models &models, const drive::NativeCall &call) {
  const auto id = identifier(call.arguments.at("model"), "m:");
  if (id >= models.rows.size() || models.rows[id].id != id)
    throw std::invalid_argument("unknown model");
  if (models.rows[id].author != player(call))
    throw std::invalid_argument("only the model author may edit its defaults");
  return id;
}
inline TermId term(const react::Terms &terms, const Json &value) {
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
inline Value scalar(const Json &value, const react::Term &term) {
  if (term.kind == react::Kind::Flag) {
    if (!value.is_boolean())
      throw std::invalid_argument("flag value must be boolean");
    return Value(value.get<bool>());
  }
  if (term.kind != react::Kind::Quantity || !value.is_number() ||
      !std::isfinite(value.get<double>()))
    throw std::invalid_argument("quantity value must be finite numeric");
  return value.is_number_integer() ? Value(value.get<std::int64_t>()) : Value(value.get<double>());
}
inline drive::NativeCompletion result(bool changed, Json extra = Json::object()) {
  drive::NativeCompletion out;
  out.changed = changed;
  out.result = changed ? record::Result::Applied : record::Result::NoEffect;
  out.reply = {{"ok", true}, {"effect", changed ? "applied" : "none"}};
  out.reply.update(extra);
  return out;
}
inline drive::NativeCompletion failed(const std::exception &error) {
  drive::NativeCompletion out;
  out.result = record::Result::Denied;
  out.reply = {{"ok", false}, {"error", "edit_denied"}, {"problem", error.what()}};
  return out;
}
inline std::vector<Handle> instances(IdentityRead identity, ModelId model) {
  std::vector<Handle> result;
  for (std::uint32_t slot = 1; slot < identity.model.size(); ++slot)
    if (identity.model[slot] == model)
      result.push_back(identity.at(slot));
  return result;
}
inline void watches(react::DeferredEffects &effects, install::WatchChanges changed) {
  for (auto &work : changed.dirty)
    effects.ordered.push_back(std::move(work));
  for (auto &timer : changed.timers)
    effects.ordered.push_back(std::move(timer));
}
inline bool role(const control::Profiles &profiles, PlayerId player, std::string_view role) {
  return player < profiles.rows.size() && profiles.rows[player].id == player &&
         std::find(profiles.rows[player].roles.begin(), profiles.rows[player].roles.end(), role) !=
             profiles.rows[player].roles.end();
}
} // namespace amber::bindings::editing::detail

namespace amber::bindings::editing {
std::string validate_install(const Json &);
std::string validate_uninstall(const Json &);
std::string validate_model_script(const Json &);
std::string validate_edit_text(const Json &);
std::string validate_model_text(const Json &);
std::string validate_edit_detail(const Json &);
std::string validate_set_setting(const Json &);
std::string validate_model_setting(const Json &);
std::string validate_patch_state(const Json &);
std::string validate_relate(const Json &);
std::string validate_define_term(const Json &);
std::string validate_define_material(const Json &);
drive::NativeCompletion install_script(void *, const drive::NativeCall &);
drive::NativeCompletion uninstall_script(void *, const drive::NativeCall &);
drive::NativeCompletion model_script(void *, const drive::NativeCall &);
drive::NativeCompletion edit_text(void *, const drive::NativeCall &);
drive::NativeCompletion model_text(void *, const drive::NativeCall &);
drive::NativeCompletion edit_detail(void *, const drive::NativeCall &);
drive::NativeCompletion set_setting(void *, const drive::NativeCall &);
drive::NativeCompletion model_setting(void *, const drive::NativeCall &);
drive::NativeCompletion patch_state(void *, const drive::NativeCall &);
drive::NativeCompletion relate(void *, const drive::NativeCall &);
drive::NativeCompletion define_term(void *, const drive::NativeCall &);
drive::NativeCompletion define_material(void *, const drive::NativeCall &);
} // namespace amber::bindings::editing
