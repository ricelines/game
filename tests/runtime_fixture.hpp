#pragma once

#include "catalog/catalog.hpp"
#include "react/react.hpp"
#include <stdexcept>

namespace amber::test_runtime {
using namespace react;
inline void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Fixture {
  amber::ObjectState objects_state;
  amber::react::State react_state;
  amber::catalog::State catalog_state;

  Objects &objects = objects_state.objects;
  Handle a, b, c;
  Terms &terms = react_state.terms;
  Materials &materials = react_state.materials;
  AssetMeasurements &assets = catalog_state.baked_measurements;
  Properties &properties = react_state.properties;
  Processes &processes = react_state.processes;
  InstanceSettings &instance_settings = react_state.instance_settings;
  ModelSettings &model_settings = react_state.model_settings;
  PlotSettings &plots = react_state.plot_settings;
  WorldSettings &world = react_state.world_settings;
  SharedColumns &shared = react_state.shared;
  GivesTo &gives_to = react_state.gives_to;
  AllocationRows &allocations = react_state.allocations;
  AuthoredRelations &relations = react_state.authored_relations;
  RelationsRead &relation_view = react_state.relations;
  RuntimeMeasurements &runtime = react_state.runtime_measurements;
  catalog::Models &models = catalog_state.models;
  catalog::Scripts &scripts = catalog_state.scripts;
  std::unordered_map<ScriptId, CompiledScript> code;
  InstanceScripts &installed = react_state.installations;
  Triggers &triggers = react_state.triggers;
  RuleVersions &rules = react_state.rules;
  WatchAnswers &watch_answers = react_state.watch_answers;
  ScriptStates &states = react_state.states;
  RandomStates &random = react_state.random;
  DeferredEffects effects;
  std::optional<ValueRead> projection;
  Vm &vm = react_state.vm;
  explicit Fixture(double handler_ms = 5) : react_state({4096, 50, handler_ms, 64}) {
    ObjectInit init;
    init.model = 1;
    init.material = 8;
    a = create(objects, init);
    b = create(objects, init);
    c = create(objects, init);
    terms.rows.resize(10);
    const std::array<const char *, 9> names{"novel.heat",   "novel.result", "novel.link",
                                            "novel.signal", "novel.ready",  "novel.cue",
                                            "novel.touch",  "novel.mat",    "novel.other"};
    for (TermId id = 1; id < 10; ++id) {
      terms.rows[id].id = id;
      terms.rows[id].name = names[id - 1];
      terms.by_name[names[id - 1]] = id;
    }
    terms.rows[3].kind = Kind::Relation;
    terms.rows[4].combine = Combine::Sum;
    terms.rows[4].along = 3;
    terms.rows[5].kind = Kind::Flag;
    terms.rows[6].kind = Kind::Event;
    terms.rows[7].kind = Kind::Action;
    terms.rows[8].kind = Kind::Material;
    terms.rows[9].kind = Kind::Material;
    relations.emplace(3, Relation{});
    relation_view.columns[3] = &relations.at(3);
    models.rows.resize(2);
    models.rows[1].id = 1;
    projection.emplace(values());
    bind_values(vm, *projection, react_state);
    bind_events(vm, terms);
  }
  ValueRead values() { return value_read(objects_state, catalog_state, react_state); }
  ScriptId add(std::string_view source, PlayerId author = 1,
               RightsDeclaration rights = {{"use", "edit"}}) {
    auto prepared =
        catalog::prepare_script(source, terms, rights, std::string(64, 'a'), {}, {4096, 50, 5, 64});
    const auto compiled = prepared.compiled;
    auto id = catalog::import_script(scripts, std::move(prepared), author, 0);
    if (scripts.rows[id].declaration.kind == ScriptDeclaration::Kind::Rule) {
      scripts.rows[id].declaration.approved = true;
      scripts.rows[id].declaration.active = true;
    }
    code[id] = compiled;
    auto loaded = load_script(vm, compiled, scripts.rows[id].declaration);
    require(loaded.status == RunOutcome::Status::Ok, "real script loads");
    return id;
  }
  void state(Handle object, TermId term, double value, Step now = 0) {
    set_state(properties, processes, terms.rows[term], object, Value(value), now, 20, effects);
  }
  void baseline(Handle object) {
    baseline_watches(react_state, values(), scripts, object, 0, 20, {}, effects);
  }
  std::vector<PlannedInvocation> changed(FactKey fact, Step now = 0) {
    return evaluate_watch(react_state, values(), scripts, {fact, {}}, now, 20).invocations;
  }
  void run(std::span<const PlannedInvocation> plans, Step now = 0) {
    record::Journal journal(8, {});
    for (const auto &plan : plans) {
      Invocation invocation;
      CompletedInvocation completed;
      auto input = plan.input;
      input.step = now;
      require(start_invocation(react_state, objects_state, 0, catalog_state, journal, plan.handler,
                               std::move(input), invocation, completed),
              "the selected installed handler starts");
      if (completed.outcome.status != RunOutcome::Status::Ok)
        throw std::runtime_error(completed.outcome.error);
      finish_invocation(identity(objects), attribution(objects), models, scripts, journal,
                        invocation, completed);
      effects.changed |= completed.effects.changed;
      effects.ordered.insert(effects.ordered.end(),
                             std::make_move_iterator(completed.effects.ordered.begin()),
                             std::make_move_iterator(completed.effects.ordered.end()));
      effects.consulted_handles.insert(effects.consulted_handles.end(),
                                       completed.effects.consulted_handles.begin(),
                                       completed.effects.consulted_handles.end());
      effects.background.insert(effects.background.end(), completed.effects.background.begin(),
                                completed.effects.background.end());
    }
  }
};
} // namespace amber::test_runtime
