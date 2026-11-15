#include "step/step.hpp"
#include <iostream>
#include <stdexcept>
using namespace amber;
namespace re = amber::react;
namespace bi = amber::step;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
void test_mass_provider() {
  Objects objects;
  ObjectInit init;
  init.model = 1;
  init.material = 10;
  auto first = create(objects, init), second = create(objects, init);
  init.model = 2;
  auto root = create(objects, init);
  re::Terms terms;
  terms.rows.resize(4);
  for (TermId id = 1; id <= 3; ++id) {
    auto &t = terms.rows[id];
    t.id = id;
    t.name = "author.quantity" + std::to_string(id);
  }
  terms.rows[1].source = re::Source::Intrinsic;
  terms.rows[1].dimensions = {-3, 1, 0, 0, 0, 0, 0, 0};
  terms.rows[2].source = re::Source::Measured;
  terms.rows[2].measurement = re::MeasurementRole::Mass;
  terms.rows[2].dimensions = {0, 1, 0, 0, 0, 0, 0, 0};
  terms.rows[3].source = re::Source::Measured;
  terms.rows[3].measurement = re::MeasurementRole::Runtime;
  terms.rows[3].dimensions = terms.rows[2].dimensions;
  re::Materials materials;
  materials.rows[10][1] = Value(2000.);
  materials.rows[11][1] = Value(3000.);
  re::AssetMeasurements assets;
  assets.rows[1].volume = .005;
  assets.rows[2].volume = 0;
  re::Properties properties;
  re::Processes processes;
  re::InstanceSettings instance;
  re::ModelSettings models;
  re::PlotSettings plots;
  re::WorldSettings world;
  re::SharedColumns shared;
  re::RelationsRead relations;
  re::RuntimeMeasurements runtime;
  re::ValueRead values{terms,        identity(objects),
                       objects.plot, objects.params,
                       materials,    assets,
                       properties,   processes,
                       instance,     models,
                       plots,        world,
                       shared,       relations,
                       runtime,      1};
  Relation parts;
  parts.out.set(root.slot(), std::vector<Handle>{first, second});
  parts.in.set(first.slot(), std::vector<Handle>{root});
  parts.in.set(second.slot(), std::vector<Handle>{root});
  bi::validate_mass_roles(terms, {2, 3});
  bi::AssemblyMasses masses{{2, 3}, {}, {}};
  re::DeferredEffects effects;
  record::Origin origin{19, 7, {3}, 2.5, false};
  bi::invalidate_mass(masses, identity(objects), parts, std::span(&root, 1));
  bi::refresh_mass(masses, values, parts, runtime, 0, 20, origin, effects);
  auto read = [&](Handle h, TermId term) {
    return re::read(values, h, term, 0, 20).value.numeric();
  };
  require(read(root, 3) == 20 && read(root, 2) == 0, "empty root aggregates two real10kg children");
  require(read(root, 2) + read(first, 2) + read(second, 2) == 20,
          "construction uses disjoint own mass without double count");
  require(effects.ordered.size() == 3 && effects.changed,
          "runtime provider publishes ordinary watch effects");
  effects = {};
  bi::refresh_mass(masses, values, parts, runtime, 20, 20, origin, effects);
  require(effects.ordered.empty(), "inert mass provider does no recurring work");
  objects.material[first.slot()] = 11;
  const std::array<FactKey, 1> changed_own{{{first, 2}}};
  bi::reconcile_mass(masses, values, parts, runtime, changed_own, 20, 20, origin, effects);
  require(read(first, 3) == 15 && read(root, 3) == 25,
          "material edit updates only changed node and ancestors");
  require(effects.ordered.size() == 2, "clean sibling is reused without a watch effect");
  auto pending = effects;
  step::WorkQueues queues;
  step::TimerWheel timers;
  record::Journal journal(8, {});
  step::publish_effects(std::move(pending), origin, 20, queues, timers, journal, 10);
  while (auto next = step::pop_foreground(queues)) {
    const auto &watch = std::get<step::WatchWork>(next->work);
    require(watch.origin.root == origin.root && watch.origin.cause == origin.cause,
            "mass reaction publishes with its originating native/action causal root");
  }

  effects = {};
  materials.rows[11][1] = Value(4000.);
  const std::array<FactKey, 1> density_changed{{{first, 1}}};
  bi::reconcile_mass(masses, values, parts, runtime, density_changed, 20, 20, origin, effects);
  require(read(root, 3) == 30 && read(first, 3) == 20,
          "intrinsic density notification immediately updates source resolver and ancestor mass");
  materials.rows[11][1] = Value(3000.);
  bi::reconcile_mass(masses, values, parts, runtime, density_changed, 20, 20, origin, effects);
  effects = {};
  const std::array<FactKey, 1> aggregate_feedback{{{first, 3}}};
  bi::reconcile_mass(masses, values, parts, runtime, aggregate_feedback, 20, 20, origin, effects);
  require(effects.ordered.empty() && masses.dirty.empty(),
          "derived assembly answer does not create feedback sampling work");
  effects = {};
  bi::invalidate_mass(masses, identity(objects), parts, std::span(&second, 1));
  parts.out.set(root.slot(), std::vector<Handle>{first});
  parts.in.erase(second.slot());
  bi::reconcile_mass(masses, values, parts, runtime, {}, 20, 20, origin, effects);
  require(read(root, 3) == 15 && read(second, 3) == 10,
          "old ancestor invalidation follows removed topology");
  bi::retire_mass(masses, runtime, second);
  require(!runtime.values.contains({second, 3}) && !masses.rows.contains(second.bits),
          "retired full handle has no inherited runtime mass");
  bool bad = false;
  try {
    bi::validate_mass_roles(terms, {2, 2});
  } catch (const std::invalid_argument &) {
    bad = true;
  }
  require(bad, "aggregate role cannot replace disjoint construction mass");

  bi::AssemblyMasses retry{{2, 3}, {}, {}};
  re::RuntimeMeasurements fresh;
  parts.out.set(root.slot(), std::vector<Handle>{first, second});
  materials.rows[10][1] = Value(-2000.);
  bi::invalidate_mass(retry, identity(objects), parts, std::span(&root, 1));
  bool invalid = false;
  try {
    bi::refresh_mass(retry, values, parts, fresh, 20, 20, origin, effects);
  } catch (const std::invalid_argument &) {
    invalid = true;
  }
  require(invalid && fresh.values.empty(), "invalid mass cannot partially publish subtree");
  materials.rows[10][1] = Value(2000.);
  bi::refresh_mass(retry, values, parts, fresh, 20, 20, origin, effects);
  require(fresh.values.size() == 3, "retry publishes every newly computed descendant");
}
} // namespace
int main() {
  test_mass_provider();
  std::cout << "assembly mass provider checks passed\n";
}
