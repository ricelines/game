#include "runtime_fixture.hpp"

#include <chrono>
#include <iostream>

using namespace amber;
void run(unsigned unrelated) {
  test_runtime::Fixture f;
  ObjectInit init;
  init.model = 1;
  init.material = 9;
  for (unsigned i = 0; i < unrelated; ++i)
    create(f.objects, init);
  f.objects.material[f.a.slot()] = 9;
  react::set_relation(f.relations.at(3), 3, f.b, f.a, true, f.effects);
  const auto script = f.add(R"(return {kind='rule',applies_to={material='novel.mat'},
    watches={'novel.signal'},
    on={['novel.signal']=function(self,source,value)
      self.state.count=(self.state.count or 0)+1;self:set('novel.result',value)
    end},tests={{when={steps=0},expect={subject='target',term='novel.signal',equals=1}}}})");
  react::add_rule(f.rules, f.scripts, f.terms, script);
  react::project_rules(f.triggers, f.rules);
  react::GivesTo gives;
  constexpr unsigned count = 10000;
  const auto start = std::chrono::steady_clock::now();
  for (unsigned i = 1; i <= count; ++i) {
    react::DeferredEffects effects;
    react::set_contribution(f.shared, gives, f.terms.rows[4], f.c, f.a, i, effects);
    react::sync_rule_watches(f.react_state, f.values(), f.scripts, effects.ordered, i, 20, {});
    const auto plans = f.changed({f.a, 4}, i);
    test_runtime::require(plans.size() == 1, "benchmark must admit the actual term-fallback rule");
    f.effects = {};
    f.run(plans, i);
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  test_runtime::require(
      std::get<double>(f.states.at({f.b, script}).values.at(std::string("count"))) == count &&
          react::read(f.values(), f.b, 2, count, 20).value.numeric() == count,
      "benchmark must preserve real Luau state and authoritative result");
  std::cout << unrelated
            << " unrelated objects: contribution + primary rule/watch resolution + "
               "actual Luau/state write "
            << std::chrono::duration<double, std::nano>(elapsed).count() / count << " ns/op\n";
}
int main() {
  run(0);
  run(100000);
}
