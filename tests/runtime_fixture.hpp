#pragma once

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
  Subscriptions &subscriptions = react_state.subscriptions;
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
  ValueRead values() {
    return {terms,
            identity(objects),
            objects.plot,
            objects.params,
            materials,
            assets,
            properties,
            processes,
            instance_settings,
            model_settings,
            plots,
            world,
            shared,
            relation_view,
            runtime,
            0};
  }
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
  void bind(ScriptId id, Handle object, SyncMode mode = SyncMode::Initial) {
    (void)sync_watches(react_state, values(), {object, id}, scripts.rows[id].declaration,
                       amber::step::Clock{.step = 0, .hz = 20}, {}, mode);
  }
  std::vector<InvocationPlan> changed(FactKey fact, Step now = 0) {
    auto changes = refresh_dependencies(subscriptions, values(), fact, now, 20, {});
    auto result = check_changed(subscriptions, values(), allocations, fact, now, 20, {});
    auto plans = plan_deliveries(result, triggers, scripts, values(), now, 20);
    for (const auto &dirty : changes.dirty) {
      if (dirty.source == fact)
        continue;
      auto followed =
          check_changed(subscriptions, values(), allocations, dirty.source, now, 20, {});
      auto more = plan_deliveries(followed, triggers, scripts, values(), now, 20);
      plans.insert(plans.end(), std::make_move_iterator(more.begin()),
                   std::make_move_iterator(more.end()));
    }
    return plans;
  }
  void run(std::span<const InvocationPlan> plans, Step now = 0) {
    for (const auto &plan : plans) {
      Invocation invocation;
      invocation.self = {plan.receiver, plan.handler.script};
      invocation.sender = plan.sender;
      invocation.trigger = plan.trigger;
      invocation.payload = plan.payload;
      invocation.acquired = plan.acquired;
      invocation.step = now;
      invocation.hz = 20;
      invocation.state = &states[invocation.self];
      invocation.random = &random[plan.receiver.bits];
      invocation.effects = &effects;
      if (plan.watch)
        acknowledge_watch(subscriptions, invocation.self, *plan.watch);
      const auto result = run_handler(vm, plan.handler, invocation);
      if (result.status != RunOutcome::Status::Ok)
        throw std::runtime_error(result.error);
    }
  }
};
} // namespace amber::test_runtime
