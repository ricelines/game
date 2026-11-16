#include "catalog/catalog.hpp"
#include "react/react.hpp"

#include <iostream>
#include <stdexcept>

using namespace amber;
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Fixture {
  amber::ObjectState objects_state;
  amber::react::State react_state{{4096, 50, 5, 64}};
  amber::catalog::State catalog_state;

  Objects &objects = objects_state.objects;
  react::Terms &terms = react_state.terms;
  catalog::Models &models = catalog_state.models;
  catalog::Scripts &scripts = catalog_state.scripts;
  react::InstalledScripts installed;
  react::NativeActions &native = react_state.native_declarations;
  Handle body, target;
  Fixture() {
    for (auto name : {"invented.turn", "invented.unanswered", "invented.signal"}) {
      react::Term term;
      term.id = terms.rows.size();
      term.name = name;
      term.kind = term.id == 3 ? react::Kind::Quantity : react::Kind::Action;
      terms.by_name.emplace(name, term.id);
      terms.rows.push_back(term);
    }
    models.rows.resize(3);
    scripts.rows.resize(4);
    models.rows[1].id = 1;
    models.rows[1].script = 1;
    models.rows[2].id = 2;
    models.rows[2].script = 2;
    scripts.rows[1].declaration.actions.push_back({1, "use", "invented.turn", true});
    scripts.rows[2].declaration.actions.push_back({1, "edit", "invented.turn", false});
    scripts.rows[3].declaration.kind = react::ScriptDeclaration::Kind::Instance;
    scripts.rows[3].declaration.actions.push_back({1, "move", "invented.turn", false});
    ObjectInit init;
    init.model = 1;
    init.owner = 1;
    body = create(objects, init);
    init.model = 2;
    init.owner = 2;
    target = create(objects, init);
  }
  react::Resolution resolve(Caller caller, TermId action, Handle selected) {
    return react::resolve(native, terms, models, scripts, installed, identity(objects),
                          {caller, action, selected, Value{}}, body);
  }
};
void body_receiver_never_becomes_the_foreign_permission_target() {
  Fixture f;
  auto result = f.resolve(PlayerId{1}, 1, f.target);
  require(result && result.route == react::Route::Body && result.receiver == f.body &&
              result.permission_target == f.body && result.right == "use" && result.reach,
          "body action must check its installed receiver and separately require selected reach");
  f.installed.set(f.body.slot(), std::vector<ScriptId>{3});
  result = f.resolve(PlayerId{1}, 1, {});
  require(result && result.receiver == f.body && result.permission_target == f.body &&
              result.declaration_script == 3 && result.right == "move",
          "instance action did not override the body declaration");
  f.installed.erase(f.body.slot());
  require(f.resolve(PlayerId{1}, 1, {}).failure == react::Failure::MissingTarget,
          "reach-required body action silently lost its selected target");
  f.scripts.rows[1].declaration.actions.clear();
  result = f.resolve(PlayerId{1}, 1, f.target);
  require(result && result.route == react::Route::Selected && result.receiver == f.target &&
              result.permission_target == f.target && result.right == "edit",
          "removing body handler did not expose selected declaration");
}
void script_callers_fallbacks_and_reserved_native_actions_are_distinct() {
  Fixture f;
  const auto script = ScriptHolder{1, f.body};
  auto result = f.resolve(script, 1, f.target);
  require(result && result.route == react::Route::Selected && result.declaration_script == 2,
          "script caller borrowed supplied player body");
  result = f.resolve(script, 2, f.target);
  require(result && result.route == react::Route::Fallback && result.receiver == f.target &&
              result.right == "use" && result.reach,
          "undeclared actions keep the ordinary Use and reach checks before rule fallback");
  require(f.resolve(script, 2, {}).failure == react::Failure::MissingTarget,
          "script action invented a player receiver");
  f.native.rows.emplace(1,
                        react::NativeDeclaration{
                            1, {}, false, nlohmann::json::object(), react::MechanicAction::Grasp});
  result = f.resolve(PlayerId{1}, 1, {});
  require(result && result.route == react::Route::Native &&
              react::collides(f.native, f.scripts.rows[1].declaration),
          "native action collision was hidden by script declaration");
  const auto old = f.target;
  destroy(f.objects, f.target);
  ObjectInit init;
  init.model = 2;
  f.target = create(f.objects, init);
  require(f.resolve(PlayerId{1}, 1, old).failure == react::Failure::StaleTarget,
          "action accepted a reused selected slot");
  require(f.resolve(PlayerId{1}, 3, f.target).failure == react::Failure::UnknownAction,
          "quantity treated as action");
}
void typed_inputs_preserve_symbolic_terms_and_reject_ambiguous_payloads() {
  Fixture f;
  auto &action = f.terms.rows[1];
  action.inputs = {{"amount", react::InputType::Number, 3, false},
                   {"where", react::InputType::Vector, 0, false},
                   {"quantity", react::InputType::Term, 0, true},
                   {"count", react::InputType::Integer, 0, true}};
  const std::vector<EventField> valid{{"amount", Value(2.0)},
                                      {"where", Vec3{1, 2, 3}},
                                      {"quantity", std::string("invented.signal")}};
  auto validate = [&](const EventPayload &value) {
    return react::validate_arguments(f.terms, action, value);
  };
  require(validate(valid).empty(), "valid symbolic typed inputs rejected");
  auto changed = valid;
  changed.back().value = Value(std::int64_t{3});
  require(validate(changed).empty(), "Luau numeric term id rejected");
  changed.back().value = TermRef{3};
  require(validate(changed).empty(), "tagged term id rejected");
  changed.back().value = std::string("invented.missing");
  require(!validate(changed).empty(), "unknown symbolic term accepted");
  changed = valid;
  changed.push_back({"count", Value(1.5)});
  require(!validate(changed).empty(), "fraction accepted as integer");
  changed = valid;
  changed.push_back(valid.front());
  require(!validate(changed).empty(), "duplicate argument accepted");
  changed = valid;
  changed.front().value = Value(true);
  require(!validate(changed).empty(), "boolean accepted as quantity");
  changed = valid;
  changed.push_back({"unexpected", Value(1.0)});
  require(!validate(changed).empty(), "unknown argument ignored");
  require(!validate(Value{}).empty() && !validate(Value(1.0)).empty(),
          "missing arguments or scalar record accepted");
}
void signatures_follow_authored_units_and_native_schemas() {
  Fixture f;
  f.terms.rows[3].dimensions[1] = 1;
  f.terms.rows[1].inputs = {{"amount", react::InputType::Number, 3, false},
                            {"note", react::InputType::String, 0, true}};
  require(react::action_signature(f.terms, f.native, 1) ==
              "invented.turn(amount: number [kg], note?: string)",
          "authored signature lost registered units or optional input");
  react::NativeDeclaration native;
  native.action = 1;
  native.inputs_schema = nlohmann::json::parse(R"({
    "type":"object", "required":["amount"], "properties":{
      "amount":{"type":"number","unit":"kg"}, "note":{"type":"string"},
      "choices":{"type":"array","items":{"type":"string"}},
      "limit":{"oneOf":[{"type":"integer"},{"type":"null"}]}
    }})");
  f.native.rows[1] = native;
  require(react::action_signature(f.terms, f.native, 1) ==
              "invented.turn(amount: number [kg], choices?: array<string>, limit?: integer|null, "
              "note?: string)",
          "native one-line signature differs from its actual structured schema");
  require(react::format_unit({2, 1, -2, 0, 0, 0, 0, 0}) == "m^2*kg*s^-2" &&
              react::format_unit({}) == "1",
          "shared SI formatting changed registry dimensions");
}
} // namespace
int main() {
  try {
    body_receiver_never_becomes_the_foreign_permission_target();
    script_callers_fallbacks_and_reserved_native_actions_are_distinct();
    typed_inputs_preserve_symbolic_terms_and_reject_ambiguous_payloads();
    signatures_follow_authored_units_and_native_schemas();
    std::cout << "command resolution tests passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
