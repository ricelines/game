#include "bindings/revision/revision.hpp"
#include "bindings/structural/revision.hpp"

#include <iostream>
#include <stdexcept>

using namespace amber;
namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct Fixture {
  Objects objects;
  catalog::Models models;
  catalog::ParameterBlocks parameters;
  catalog::Assets assets;
  react::Terms terms;
  react::Materials materials;
  react::Properties properties;
  react::Processes processes;
  lifecycle::Parts parts;
  react::Vm vm{{4096, 50, 250, 64}};
  react::ScriptState state;
  react::DeferredEffects effects;
  Handle body;
  ScriptId next = 1;
  bindings::ModelRevisionRead read{
      identity(objects), objects.params, parts, models, parameters, assets, materials, terms, 1};
  bindings::ModelRevisionMeasure measure{read};
  bindings::ModelRevisionQueue queue{models, properties, processes, terms, 64};
  Fixture() {
    terms.rows.resize(3);
    terms.rows[1].id = 1;
    terms.rows[1].name = "novel.density";
    terms.rows[1].source = react::Source::Intrinsic;
    terms.rows[2].id = 2;
    terms.rows[2].name = "novel.material";
    terms.rows[2].kind = react::Kind::Material;
    terms.by_name = {{"novel.density", 1}, {"novel.material", 2}};
    materials.rows[2][1] = Value(10.0);
    models.rows.resize(4);
    for (ModelId id = 1; id < models.rows.size(); ++id) {
      auto &model = models.rows[id];
      model.id = id;
      model.author = 2;
      model.material = 2;
      model.source_hash = std::string(64, 'a' + id - 1);
      model.asset = std::string(64, 'd' + id - 1);
      model.canonical_params = "{}";
      auto &asset = assets.rows[model.asset];
      asset.hash = model.asset;
      asset.source_hash = model.source_hash;
      asset.canonical_params = "{}";
      asset.volume = id;
      asset.manifest = {{"parts", nlohmann::json::object()},
                        {"parameter_schema", nlohmann::json::object()}};
    }
    ObjectInit object;
    object.model = 1;
    object.material = 2;
    object.owner = 5;
    create(objects, object);
    object.model = 3;
    object.owner = 2;
    body = create(objects, object);
    state[std::string("stock")] = 10.0;
    react::register_binding(vm, "measure_model_change", bindings::measure_model_change_binding,
                            &measure);
    react::register_binding(vm, "revise_model", bindings::revise_model_binding, &queue);
  }
  react::RunOutcome run(std::string source, bool approved_rule = true) {
    react::ScriptDeclaration declaration;
    declaration.id = next++;
    declaration.author = 2;
    declaration.kind = approved_rule ? react::ScriptDeclaration::Kind::Rule
                                     : react::ScriptDeclaration::Kind::Instance;
    declaration.approved = approved_rule;
    const auto compiled =
        react::compile_script("return {on={run=function(self) " + source + " end}}");
    const auto loaded = react::load_script(vm, compiled, declaration);
    require(loaded.status == react::RunOutcome::Status::Ok, loaded.error.c_str());
    effects = {};
    react::Invocation invocation;
    invocation.self = {body, declaration.id};
    invocation.state = &state;
    invocation.effects = &effects;
    invocation.action_caller = PlayerId{2};
    invocation.action_body = body;
    invocation.action_target = body;
    return react::run_handler(vm, {declaration.id, "run", 2, 1, true, approved_rule}, invocation);
  }
};
void actual_measurement_and_queue() {
  Fixture f;
  const auto measured = f.run(R"(
    local change=self:measure_model_change('m:1','m:2')
    assert(change.instances==1 and change.before.mass_kg==10 and change.after.mass_kg==20)
    assert(change.before.materials[1].material==2)
    assert(not pcall(function()change.after.materials[1].mass_kg=0 end))
  )");
  require(measured.status == react::RunOutcome::Status::Ok, measured.error.c_str());
  const auto source = "local q=self:revise_model('m:1','m:2','" + std::string(64, 'a') +
                      "',{state={stock=2}});assert(q.queued)";
  const auto queued = f.run(source);
  require(queued.status == react::RunOutcome::Status::Ok, queued.error.c_str());
  require(f.effects.structural.size() == 1 && !f.effects.changed,
          "queueing a revision must not alter definitions or debit resources");
  const auto &request = f.effects.structural.front();
  require(request.kind == react::StructuralRequest::Kind::ReviseModel && request.revision &&
              request.revision->model == 1 && request.revision->proposal == 2 &&
              request.revision->expected_source_hash == std::string(64, 'a'),
          "typed revision payload must preserve exact identities and expected source");
  require(request.exchange && request.exchange->state.size() == 1 &&
              std::get<double>(*request.exchange->state[0].before) == 10 &&
              std::get<double>(request.exchange->state[0].after) == 2 &&
              std::get<double>(f.state.at(std::string("stock"))) == 10,
          "stock exchange captures actual before/after without premature mutation");
  require(f.run(source, false).status == react::RunOutcome::Status::Error &&
              f.effects.structural.empty(),
          "ordinary model/instance scripts cannot acquire definition-edit authority");
  const auto stale = "self:revise_model(1,2,'" + std::string(64, 'f') + "')";
  require(f.run(stale).status == react::RunOutcome::Status::Error && f.effects.structural.empty(),
          "stale source revisions fail before queueing");
}
} // namespace
int main() {
  try {
    actual_measurement_and_queue();
    std::cout << "Model revision Lua bindings passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
