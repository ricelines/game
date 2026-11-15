#include "command.hpp"

#include <algorithm>
#include <cmath>

namespace amber::command {
TermId declare_native(NativeActions &native, react::Terms &terms, std::string name,
                      std::string description, std::string right, bool reach,
                      nlohmann::json inputs_schema) {
  react::Term expected;
  expected.name = std::move(name);
  expected.kind = react::Kind::Action;
  expected.description = std::move(description);
  TermId id;
  if (const auto existing = terms.by_name.find(expected.name); existing != terms.by_name.end()) {
    id = existing->second;
    auto actual = catalog::term_json(terms.rows.at(id));
    auto desired = catalog::term_json(expected);
    for (auto key : {"id", "compared", "distinction"}) {
      actual.erase(key);
      desired.erase(key);
    }
    if (actual != desired)
      throw std::invalid_argument("reserved native action metadata mismatch: " + expected.name);
  } else
    id = catalog::append_term(
        terms, {expected},
        {0, "Reserved native administration operation with the described input schema."});
  native.rows[id] = {id, std::move(right), reach, std::move(inputs_schema)};
  return id;
}
namespace {
struct Declaration {
  const react::ActionDeclaration *action = nullptr;
  ScriptId script = 0;
};
Declaration declared(const catalog::Models &models, const catalog::Scripts &scripts,
                     const InstalledScripts &installed, IdentityRead identity, Handle object,
                     TermId action) {
  auto find = [&](ScriptId id) -> Declaration {
    if (!id || id >= scripts.rows.size())
      return {};
    const auto &declaration = scripts.rows[id].declaration;
    if (!declaration.active || declaration.kind == react::ScriptDeclaration::Kind::Rule)
      return {};
    for (const auto &entry : declaration.actions)
      if (entry.action == action)
        return {&entry, id};
    return {};
  };
  if (const auto *instances = installed.get(object.slot()))
    for (auto it = instances->rbegin(); it != instances->rend(); ++it)
      if (auto found = find(*it); found.action)
        return found;
  return find(models.rows[identity.model[object.slot()]].script);
}
Resolution selected(Declaration declaration, Route route, Handle receiver, Handle target) {
  Resolution result;
  result.route = route;
  result.receiver = receiver;
  result.permission_target = declaration.action->right_on == "self" ? receiver : target;
  result.right = declaration.action->right;
  result.reach = declaration.action->reach;
  result.mechanics = declaration.action->mechanics;
  result.declaration_script = declaration.script;
  if (!result.permission_target && (!result.right.empty() || result.reach))
    result.failure = Failure::MissingTarget;
  return result;
}
bool matches(const react::Terms &terms, const react::Input &input, const EventAtom &atom) {
  const auto *value = std::get_if<Value>(&atom);
  switch (input.type) {
  case react::InputType::Number:
    return value && (value->kind == Value::Kind::Number || value->kind == Value::Kind::Integer) &&
           std::isfinite(value->numeric());
  case react::InputType::Integer:
    return value && (value->kind == Value::Kind::Integer ||
                     (value->kind == Value::Kind::Number && std::isfinite(value->number) &&
                      std::trunc(value->number) == value->number && value->number >= -0x1p63 &&
                      value->number < 0x1p63));
  case react::InputType::Boolean:
    return value && value->kind == Value::Kind::Boolean;
  case react::InputType::Handle:
    return value && value->kind == Value::Kind::Object;
  case react::InputType::Vector: {
    const auto *vector = std::get_if<Vec3>(&atom);
    return vector && std::isfinite(vector->x) && std::isfinite(vector->y) &&
           std::isfinite(vector->z);
  }
  case react::InputType::String:
    return std::holds_alternative<std::string>(atom);
  case react::InputType::Term:
    if (const auto *ref = std::get_if<TermRef>(&atom))
      return ref->id && ref->id < terms.rows.size();
    if (const auto *name = std::get_if<std::string>(&atom))
      return terms.by_name.contains(*name);
    return value && (value->kind == Value::Kind::Integer || value->kind == Value::Kind::Number) &&
           std::isfinite(value->numeric()) && std::trunc(value->numeric()) == value->numeric() &&
           value->numeric() > 0 && value->numeric() < terms.rows.size();
  }
  return false;
}
} // namespace
Resolution resolve(const NativeActions &native, const react::Terms &terms,
                   const catalog::Models &models, const catalog::Scripts &scripts,
                   const InstalledScripts &installed, IdentityRead identity,
                   const ActionRequest &request, Handle body) {
  Resolution result;
  if (!request.action || request.action >= terms.rows.size() ||
      terms.rows[request.action].kind != react::Kind::Action) {
    result.failure = Failure::UnknownAction;
    return result;
  }
  if (request.target && !identity.valid(request.target)) {
    result.failure = Failure::StaleTarget;
    return result;
  }
  if (const auto found = native.rows.find(request.action); found != native.rows.end()) {
    result.route = Route::Native;
    result.receiver = request.target;
    result.permission_target = request.target;
    result.right = found->second.right;
    result.reach = found->second.reach;
    if (!request.target && (!result.right.empty() || result.reach))
      result.failure = Failure::MissingTarget;
    return result;
  }
  // An installed script never obtains a player's direct-body invocation envelope.
  if (std::holds_alternative<PlayerId>(request.caller) && identity.valid(body)) {
    if (const auto found = declared(models, scripts, installed, identity, body, request.action);
        found.action)
      return selected(found, Route::Body, body, request.target);
  }
  if (request.target) {
    if (const auto found =
            declared(models, scripts, installed, identity, request.target, request.action);
        found.action)
      return selected(found, Route::Selected, request.target, request.target);
    result.receiver = request.target;
  } else if (std::holds_alternative<PlayerId>(request.caller) && identity.valid(body)) {
    result.receiver = body;
  } else {
    result.failure = Failure::MissingTarget;
    return result;
  }
  result.permission_target = result.receiver;
  result.right = "use";
  return result;
}
bool collides(const NativeActions &native, const react::ScriptDeclaration &script) {
  return std::any_of(script.actions.begin(), script.actions.end(),
                     [&](const auto &action) { return native.rows.contains(action.action); });
}
std::vector<ArgumentError> validate_arguments(const react::Terms &terms, const react::Term &term,
                                              const EventPayload &args) {
  std::vector<ArgumentError> errors;
  const auto *fields = std::get_if<std::vector<EventField>>(&args);
  if (!fields) {
    const auto *value = std::get_if<Value>(&args);
    if (!value || value->present()) {
      errors.push_back({"args", "expected an input record"});
      return errors;
    }
  }
  std::vector<std::string_view> seen;
  if (fields)
    for (const auto &field : *fields) {
      if (std::find(seen.begin(), seen.end(), field.name) != seen.end()) {
        errors.push_back({field.name, "input is repeated"});
        continue;
      }
      seen.push_back(field.name);
      const auto input = std::find_if(term.inputs.begin(), term.inputs.end(),
                                      [&](const auto &input) { return input.name == field.name; });
      if (input == term.inputs.end())
        errors.push_back({field.name, "unknown input"});
      else {
        const auto *value = std::get_if<Value>(&field.value);
        if (!(input->optional && value && !value->present()) &&
            !matches(terms, *input, field.value))
          errors.push_back({field.name, "input has the wrong type or numeric range"});
      }
    }
  for (const auto &input : term.inputs)
    if (!input.optional && std::find(seen.begin(), seen.end(), input.name) == seen.end())
      errors.push_back({input.name, "required input is missing"});
  return errors;
}
} // namespace amber::command
