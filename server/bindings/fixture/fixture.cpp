#include "fixture.hpp"

#include <fstream>

namespace amber::bindings::fixture {
namespace {
using Json = nlohmann::json;
Handle subject(const Case &test, std::string_view name, IdentityRead identity) {
  const auto found = test.subjects.find(std::string(name));
  if (found == test.subjects.end() || !identity.valid(found->second))
    throw std::invalid_argument("fixture subject is missing or stale: " + std::string(name));
  return found->second;
}
TermId term(const react::Terms &terms, const Json &name) {
  const auto text = name.get<std::string>();
  const auto found = terms.by_name.find(text);
  if (found == terms.by_name.end())
    throw std::invalid_argument("fixture term is not registered: " + text);
  return found->second;
}
Value value(const Case &test, const Json &input, IdentityRead identity) {
  if (input.is_boolean())
    return Value(input.get<bool>());
  if (input.is_number_integer())
    return Value(input.get<std::int64_t>());
  if (input.is_number())
    return Value(input.get<double>());
  return Value(subject(test, input.at("handle").get<std::string>(), identity));
}
Json json_value(Value value) {
  switch (value.kind) {
  case Value::Kind::None:
    return nullptr;
  case Value::Kind::Number:
    return value.number;
  case Value::Kind::Integer:
    return value.integer;
  case Value::Kind::Boolean:
    return value.boolean;
  case Value::Kind::Object:
    return to_string(value.object);
  }
  std::terminate();
}
} // namespace
Case read_case(const std::filesystem::path &path) {
  std::ifstream file(path);
  if (!file)
    throw std::runtime_error("cannot read fixture case");
  const auto json = Json::parse(file);
  if (json.at("version") != 1)
    throw std::invalid_argument("unsupported fixture case version");
  Case result;
  result.test = json.at("test");
  elaborate::validate_tests(Json::array({result.test}));
  for (const auto &[name, bits] : json.at("subjects").items())
    result.subjects.emplace(name, Handle{bits.get<std::uint64_t>()});
  return result;
}
void apply_given(Setup &binding, const Case &test, Step now, double hz,
                 react::DeferredEffects &effects) {
  if (!test.test.contains("given"))
    return;
  const auto apply = [&](const Json &entry) {
    const auto object =
        subject(test, entry.at("subject").get<std::string>(), binding.values.identity);
    if (entry.contains("state"))
      for (const auto &[name, input] : entry.at("state").items()) {
        const auto id = term(binding.values.terms, name);
        react::set_state(binding.properties, binding.processes, binding.values.terms.rows[id],
                         object, value(test, input, binding.values.identity), now, hz, effects);
      }
    if (entry.contains("contribute"))
      for (const auto &[name, amount] : entry.at("contribute").items()) {
        const auto id = term(binding.values.terms, name);
        react::set_contribution(binding.shared, binding.gives, binding.values.terms.rows[id],
                                object, object, amount.get<double>(), effects);
      }
  };
  const auto &given = test.test.at("given");
  if (given.is_array())
    for (const auto &entry : given)
      apply(entry);
  else
    apply(given);
}
ActionOutcome perform_when(ActionTurns &turns, const Case &test) {
  const auto &action = test.test.at("when").at("action");
  const auto body =
      subject(test, action.value("actor", std::string("body")), turns.handlers.values.identity);
  const auto owner = turns.authority.owner[body.slot()];
  if (!owner || owner >= turns.profiles.rows.size() || turns.profiles.rows[owner].body != body)
    throw std::invalid_argument("fixture action actor must be a captured player body");
  ActionRequest request;
  request.caller = owner;
  request.action = term(turns.handlers.values.terms, action.at("term"));
  if (action.contains("target"))
    request.target =
        subject(test, action.at("target").get<std::string>(), turns.handlers.values.identity);
  auto arguments = action.value("args", Json::object());
  // Named handles keep a test independent of capture generation and slot layout.
  for (auto &[name, input] : arguments.items()) {
    (void)name;
    if (input.is_object() && input.size() == 1 && input.contains("handle"))
      input = to_string(
          subject(test, input.at("handle").get<std::string>(), turns.handlers.values.identity));
  }
  if (!turns.declarations.rows.contains(request.action))
    request.args =
        decode_arguments(turns.handlers.values.terms.rows[request.action], arguments, body);
  return perform_action(turns, std::move(request), std::move(arguments), {});
}
std::uint64_t steps(const Case &test) {
  const auto &when = test.test.at("when");
  return when.contains("steps") ? when.at("steps").get<std::uint64_t>() : 1;
}
elaborate::TestRun observe(const react::ValueRead &values, const Case &test, Step now, double hz) {
  const auto &expected = test.test.at("expect");
  const auto object = subject(test, expected.at("subject").get<std::string>(), values.identity);
  const auto id = term(values.terms, expected.at("term"));
  const auto actual = react::read(values, object, id, now, hz);
  bool passed = false;
  if (actual.present) {
    if (expected.contains("equals")) {
      const auto wanted = value(test, expected.at("equals"), values.identity);
      passed = actual.value.is_numeric() && wanted.is_numeric()
                   ? actual.value.numeric() == wanted.numeric()
                   : actual.value == wanted;
    } else if (actual.value.is_numeric()) {
      passed = expected.contains("above")
                   ? actual.value.numeric() > expected.at("above").get<double>()
                   : actual.value.numeric() < expected.at("below").get<double>();
    }
  }
  return {passed,
          {{"subject", to_string(object)},
           {"term", values.terms.rows[id].name},
           {"present", actual.present},
           {"value", actual.present ? json_value(actual.value) : Json(nullptr)}},
          {}};
}
} // namespace amber::bindings::fixture
