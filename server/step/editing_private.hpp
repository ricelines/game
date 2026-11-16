#pragma once

#include "catalog/catalog.hpp"

#include "step.hpp"

#include <stdexcept>

namespace amber::step::editing::detail {
struct SlotRequest {
  std::vector<std::string> names;
  perm::GrantRequest request;
  perm::ScopeControl control;
};
std::vector<SlotRequest> slot_choices(const Json &, const catalog::Script &,
                                      const amber::ObjectState &objects_state,
                                      const amber::perm::State &perm_state, const NativeCall &);
NativeCompletion fill_slots(const catalog::Script &, std::span<const SlotRequest>, AuthorityRead,
                            perm::Players &, perm::Grants &, const NativeCall &);
struct PolicyDenied : std::runtime_error {
  Json details;
  PolicyDenied(std::string_view need, Json on, std::string why, std::string fix);
};
perm::Holder holder(const step::NativeCall &call);
PlayerId player(const step::NativeCall &call);
std::uint32_t identifier(const Json &value, std::string_view prefix);
Handle handle(const Json &value, IdentityRead identity);
ModelId model(const catalog::Models &models, const perm::Players &players,
              const step::NativeCall &call);
TermId term(const react::Terms &terms, const Json &value);
Value scalar(const Json &value, const react::Term &term);
step::NativeCompletion result(bool changed, Json extra = Json::object());
step::NativeCompletion failed(const std::exception &error);
std::vector<Handle> instances(IdentityRead identity, ModelId model);
void watches(react::DeferredEffects &effects, react::WatchChanges changed);
bool role(std::span<const std::string> roles, std::string_view name);
} // namespace amber::step::editing::detail
