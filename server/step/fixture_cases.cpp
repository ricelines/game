#include "run_private.hpp"

#include <fstream>

namespace amber::step::fixture {
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
void extend_vocabulary(react::Terms &terms, react::Materials &materials,
                       const catalog::MechanicalBindings &mechanical,
                       const std::filesystem::path &registry_root,
                       const catalog::Hash &candidate_registry) {
  const auto registry = catalog::read_registry(registry_root, candidate_registry);
  react::Terms candidate_terms;
  catalog::load_terms(registry, candidate_terms);
  if (candidate_terms.rows.size() < terms.rows.size() ||
      registry.data.at("mechanical") != mechanical.values)
    throw std::invalid_argument("candidate registry is not an extension of the capture vocabulary");
  for (std::size_t id = 1; id < terms.rows.size(); ++id)
    if (catalog::term_json(terms.rows[id]) != catalog::term_json(candidate_terms.rows[id]))
      throw std::invalid_argument("candidate changed a captured term definition");
  const auto previous_size = terms.rows.size();
  terms = std::move(candidate_terms);
  react::Materials candidate_materials;
  catalog::load_materials(registry, terms, candidate_materials);
  for (auto &[id, data] : candidate_materials.rows)
    if (id >= previous_size)
      materials.rows.emplace(id, std::move(data));
}
react::RuleChange install_candidate(catalog::Scripts &scripts, amber::react::State &react_state,
                                    const catalog::RuntimePins &pins,
                                    const std::filesystem::path &source_root,
                                    const std::filesystem::path &destination_root,
                                    const catalog::Hash &artifact,
                                    const catalog::Hash &registry_pin, PlayerId author, bool active,
                                    const amber::step::Clock &clock) {
  auto &vm = react_state.vm;

  const auto now = clock.step;
  const auto hz = clock.hz;

  auto &rules = react_state.rules;
  auto &terms = react_state.terms;

  ScriptId candidate = 0;
  for (const auto &script : scripts.rows)
    if (script.id && script.artifact == artifact)
      candidate = script.id;
  if (!candidate) {
    const react::RightsDeclaration rights{
        {"use", "take", "move", "edit", "copy", "transfer", "destroy"},
        {"spend", "build", "enter", "run_scripts", "animate", "camera", "issue"}};
    auto prepared = catalog::read_script(source_root, artifact, pins, registry_pin, terms, rights);
    const auto compiled = prepared.compiled;
    catalog::write_script(destination_root / prepared.hash, prepared);
    candidate = catalog::import_script(scripts, std::move(prepared), author, now / hz);
    const auto loaded = react::load_script(vm, compiled, scripts.rows[candidate].declaration);
    if (loaded.status != react::RunOutcome::Status::Ok)
      throw std::runtime_error(loaded.error);
  }
  auto &declaration = scripts.rows[candidate].declaration;
  if (declaration.kind != react::ScriptDeclaration::Kind::Rule)
    throw std::invalid_argument("fixture candidate must be a world rule");
  declaration.approved = active;
  declaration.active = active;
  react::set_activation(vm, candidate, active, active);
  return active ? react::add_rule(rules, scripts, terms, candidate) : react::RuleChange{};
}
void apply_given(amber::react::State &react_state, const react::ValueRead &values, const Case &test,
                 Step now, double hz, react::DeferredEffects &effects) {
  auto &properties = react_state.properties;
  auto &processes = react_state.processes;
  auto &shared = react_state.shared;
  auto &gives = react_state.gives_to;

  if (!test.test.contains("given"))
    return;
  const auto apply = [&](const Json &entry) {
    const auto object = subject(test, entry.at("subject").get<std::string>(), values.identity);
    if (entry.contains("state"))
      for (const auto &[name, input] : entry.at("state").items()) {
        const auto id = term(values.terms, name);
        react::set_state(properties, processes, values.terms.rows[id], object,
                         value(test, input, values.identity), now, hz, effects);
      }
    if (entry.contains("contribute"))
      for (const auto &[name, amount] : entry.at("contribute").items()) {
        const auto id = term(values.terms, name);
        react::set_contribution(shared, gives, values.terms.rows[id], object, object,
                                amount.get<double>(), effects);
      }
  };
  const auto &given = test.test.at("given");
  if (given.is_array())
    for (const auto &entry : given)
      apply(entry);
  else
    apply(given);
}
void initialize_case(const Case &test, amber::react::State &react_state,
                     const react::ValueRead &values, const catalog::Scripts &scripts,
                     const react::RuleChange &activated, WorkQueues &queues,
                     const amber::step::Clock &clock, react::DeferredEffects &effects) {
  const auto now = clock.step;
  const auto hz = clock.hz;

  auto &shared = react_state.shared;

  apply_given(react_state, values, test, now, hz, effects);
  react::reconcile_allocations(values, react_state, effects, now, hz);
  for (const auto &rule : activated.active)
    for (auto &item : react::catch_up(shared, values.identity, scripts, values.terms, rule, {}))
      enqueue_background(queues, std::move(item));
}
std::optional<NativeCall> prepare_action(const Case &test, const react::Terms &terms,
                                         const react::NativeActions &native, IdentityRead identity,
                                         AuthorityRead authority, const net::Profiles &profiles,
                                         Step now, double hz) {
  if (!test.test.at("when").contains("action"))
    return {};
  const auto &action = test.test.at("when").at("action");
  const auto body = test.subjects.at(action.value("actor", std::string("body")));
  if (!identity.valid(body))
    throw std::invalid_argument("fixture actor is stale");
  const auto owner = authority.owner[body.slot()];
  if (!owner || owner >= profiles.rows.size() || profiles.rows[owner].body != body)
    throw std::invalid_argument("fixture action actor must be a captured player body");
  ActionRequest request;
  request.caller = owner;
  request.action = terms.by_name.at(action.at("term").get<std::string>());
  if (action.contains("target"))
    request.target = test.subjects.at(action.at("target").get<std::string>());
  auto arguments = action.value("args", Json::object());
  for (auto &[name, input] : arguments.items()) {
    (void)name;
    if (input.is_object() && input.size() == 1 && input.contains("handle"))
      input = to_string(test.subjects.at(input.at("handle").get<std::string>()));
  }
  if (!native.rows.contains(request.action))
    request.args = decode_arguments(terms.rows[request.action], arguments, body);
  return NativeCall{std::move(request), std::move(arguments), body, now, hz, {}, {}};
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
void write_report(const std::filesystem::path &path, const react::ValueRead &values,
                  const Case &test, Step now, double hz, std::size_t script_errors) {
  auto result = observe(values, test, now, hz);
  if (script_errors) {
    result.passed = false;
    result.error = "fixture handlers reported execution errors";
  }
  std::ofstream output(path);
  output << Json{{"version", 1},
                 {"passed", result.passed},
                 {"observed", result.observed},
                 {"error", result.error}}
                .dump(2)
         << '\n';
  if (!output)
    throw std::runtime_error("cannot write fixture report");
}
} // namespace amber::step::fixture
