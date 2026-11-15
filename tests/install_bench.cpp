#include "install/install.hpp"

#include <chrono>
#include <iostream>

using namespace amber;
using namespace amber::react;
using namespace amber::install;
int main() {
  Objects objects;
  ObjectInit init;
  init.model = 1;
  init.material = 2;
  const auto a = create(objects, init), b = create(objects, init);
  Terms terms;
  terms.rows.resize(3);
  terms.rows[1].id = 1;
  terms.rows[1].name = "new.quantity";
  terms.rows[2].id = 2;
  terms.rows[2].kind = Kind::Relation;
  Materials materials;
  AssetMeasurements assets;
  Properties properties;
  Processes processes;
  InstanceSettings instance;
  ModelSettings models;
  PlotSettings plots;
  WorldSettings world;
  SharedColumns shared;
  Relation relation;
  RelationsRead relations{{{2, &relation}}};
  RuntimeMeasurements runtime;
  ValueRead values{terms,        identity(objects),
                   objects.plot, objects.params,
                   materials,    assets,
                   properties,   processes,
                   instance,     models,
                   plots,        world,
                   shared,       relations,
                   runtime,      0};
  DeferredEffects effects;
  AllocationRows allocations;
  set_relation(relation, 2, a, b, true, effects);
  set_state(properties, processes, terms.rows[1], b, Value(0.0), 0, 20, effects);
  ScriptDeclaration declaration;
  declaration.id = 1;
  declaration.watches = {{1, 2, {}}};
  Subscriptions subscriptions;
  WatchLinks links;
  sync_watches(subscriptions, links, values, allocations, {a, 1}, declaration, 0, 20, {},
               SyncMode::Initial);
  constexpr unsigned count = 100000;
  std::size_t delivered = 0;
  auto start = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < count; ++i) {
    properties[1].set(b.slot(), Value(double(i)));
    auto result = check_changed(subscriptions, values, allocations, {b, 1}, 0, 20, {});
    delivered += result.deliveries.size();
  }
  std::cout << "actual_watch_compare iterations=" << count << " ns/op="
            << std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                       .count() /
                   count
            << " delivered=" << delivered << '\n';
  for (unsigned i = 0; i < 10000; ++i)
    create(objects, init);
  start = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < count; ++i)
    (void)refresh_dependencies(subscriptions, links, values, {a, 2}, 0, 20, {});
  std::cout << "indexed_dependency_unchanged unrelated_objects=10000 iterations=" << count
            << " ns/op="
            << std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                       .count() /
                   count
            << " links=" << links.rows.size() << '\n';
  shared.resize(2);
  shared[1].set(a.slot(), Shared{1, {}});
  RuleVersions rules;
  rules.history.push_back({{1, "new.quantity", 1, 3, true}, 1, 2, false});
  step::BackgroundItem item{a, 1, 1, {}, 3};
  std::size_t valid = 0;
  start = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < count; ++i)
    valid += catch_up_valid(item, rules, values, 0, 20);
  std::cout << "current_catchup_validation iterations=" << count << " ns/op="
            << std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                       .count() /
                   count
            << " valid=" << valid << '\n';
}
