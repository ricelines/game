#pragma once

#include "install/install.hpp"
#include <stdexcept>

namespace amber::test_runtime {
using namespace react;
using namespace install;
inline void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Fixture {
  Objects objects;
  Handle a, b, c;
  Terms terms;
  Materials materials;
  AssetMeasurements assets;
  Properties properties;
  Processes processes;
  InstanceSettings instance_settings;
  ModelSettings model_settings;
  PlotSettings plots;
  WorldSettings world;
  SharedColumns shared;
  AllocationRows allocations;
  AuthoredRelations relations;
  RelationsRead relation_view;
  RuntimeMeasurements runtime;
  catalog::Models models;
  catalog::Scripts scripts;
  std::unordered_map<ScriptId, CompiledScript> code;
  InstanceScripts installed;
  Triggers triggers;
  RuleVersions rules;
  Subscriptions subscriptions;
  WatchLinks links;
  ScriptStates states;
  RandomStates random;
  DeferredEffects effects;
  std::optional<ValueRead> projection;
  std::optional<ReadBinding> reads;
  std::optional<StateBinding> writes;
  Vm vm;
  explicit Fixture(double handler_ms = 5) : vm({4096, 50, handler_ms, 64}) {
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
    reads.emplace(ReadBinding{*projection});
    writes.emplace(StateBinding{properties, processes, terms});
    register_binding(vm, "get", get_binding, &*reads);
    register_binding(vm, "read", read_binding, &*reads);
    register_binding(vm, "set", set_binding, &*writes);
    register_binding(vm, "rate", rate_binding, &*writes);
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
  ScriptId add(std::string_view source, PlayerId author = 1) {
    auto prepared = catalog::prepare_script(source, terms, {{"use", "edit"}}, std::string(64, 'a'),
                                            {}, {4096, 50, 5, 64});
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
    (void)sync_watches(subscriptions, links, values(), allocations, {object, id},
                       scripts.rows[id].declaration, 0, 20, {}, mode);
  }
  std::vector<InvocationPlan> changed(FactKey fact, Step now = 0) {
    auto changes = refresh_dependencies(subscriptions, links, values(), fact, now, 20, {});
    auto result = check_changed(subscriptions, values(), allocations, fact, now, 20, {});
    auto plans = plan_deliveries(result, triggers, scripts, values(), now, 20);
    for (const auto &dirty : changes.dirty) {
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
      const auto result = run_handler(vm, plan.handler, invocation);
      if (result.status != RunOutcome::Status::Ok)
        throw std::runtime_error(result.error);
    }
  }
};
} // namespace amber::test_runtime
