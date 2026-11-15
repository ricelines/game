#pragma once

#include "catalog/catalog.hpp"

namespace amber::command {
struct NativeDeclaration {
  TermId action = 0;
  std::string right;
  bool reach = false;
  nlohmann::json inputs_schema = nlohmann::json::object();
};
struct NativeActions {
  std::unordered_map<TermId, NativeDeclaration> rows;
};
TermId declare_native(NativeActions &, react::Terms &, std::string name, std::string description,
                      std::string right, bool reach, nlohmann::json inputs_schema);
using InstalledScripts = SparseColumn<std::vector<ScriptId>>;
enum class Route { Native, Body, Selected, Fallback };
enum class Failure { None, UnknownAction, StaleTarget, MissingTarget };
struct Resolution {
  Failure failure = Failure::None;
  Route route = Route::Fallback;
  Handle receiver, permission_target;
  std::string right;
  bool reach = false;
  std::vector<std::string> mechanics;
  ScriptId declaration_script = 0;
  explicit operator bool() const { return failure == Failure::None; }
};
Resolution resolve(const NativeActions &, const react::Terms &, const catalog::Models &,
                   const catalog::Scripts &, const InstalledScripts &, IdentityRead,
                   const ActionRequest &, Handle authenticated_body);
bool collides(const NativeActions &, const react::ScriptDeclaration &);
struct ArgumentError {
  std::string input, problem;
};
std::vector<ArgumentError> validate_arguments(const react::Terms &, const react::Term &,
                                              const EventPayload &);
std::string format_unit(const std::array<std::int8_t, 8> &dimensions);
std::string action_signature(const react::Terms &, const NativeActions &, TermId action);
} // namespace amber::command
