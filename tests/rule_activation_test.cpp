#include "runtime_fixture.hpp"

#include <algorithm>
#include <iostream>

using namespace amber;
using namespace amber::react;
using test_runtime::require;
namespace {
struct Runner {

  test_runtime::Fixture f{1000};
  std::deque<BackgroundItem> background;
  void activate(ScriptId id, RuleMode mode, DeferredEffects &effects) {
    update_rule(f.scripts, f.react_state, background, f.values(), id, mode, {}, effects);
  }
  void changed(DeferredEffects &effects) {
    std::vector<FactKey> facts;
    auto add = [&](FactKey fact) {
      if (std::find(facts.begin(), facts.end(), fact) == facts.end())
        facts.push_back(fact);
    };
    for (const auto &effect : effects.ordered)
      if (const auto *work = std::get_if<react::WatchWork>(&effect))
        add(work->source);
    for (auto fact : facts)
      f.run(f.changed(fact));
  }
  double runs(Handle object, ScriptId script) const {
    const auto row = f.states.find({object, script});
    if (row == f.states.end())
      return 0;
    const auto count = row->second.values.find(std::string("runs"));
    return count == row->second.values.end() ? 0 : std::get<double>(count->second);
  }
};
std::string rule() {
  return R"(return {kind='rule',applies_to={material='novel.mat'},watches={'novel.signal'},
    on={['novel.signal']=function(self,source,value)
      self.state.runs=(self.state.runs or 0)+1; self:set('novel.result',value*3)
    end},tests={{when={steps=0},expect={subject='target',term='novel.signal',equals=1}}}})";
}
void first_contribution_and_late_relation() {
  Runner r;
  auto &f = r.f;
  f.objects.material[f.a.slot()] = 9;
  const auto id = f.add(rule());
  DeferredEffects effects;
  r.activate(id, RuleMode::Enable, effects);
  require(effects.background.empty() && f.watch_answers.empty(),
          "activation on an absent quantity creates no unrelated subscriptions");
  set_contribution(f.shared, f.gives_to, f.terms.rows[4], f.c, f.a, 2, effects);
  r.changed(effects);
  require(r.runs(f.b, id) == 0, "an unrelated object is not a quantity consumer");
  effects = {};
  set_relation(f.relations.at(3), 3, f.b, f.a, true, effects);
  r.changed(effects);
  require(r.runs(f.b, id) == 1 && r.runs(f.a, id) == 0 &&
              read(f.values(), f.b, 2, 0, 20).value.numeric() == 6,
          "the term's authored fallback discovers a newly related material consumer");
  effects = {};
  set_contribution(f.shared, f.gives_to, f.terms.rows[4], f.c, f.a, 3, effects);
  r.changed(effects);
  require(r.runs(f.b, id) == 2, "existing consumer compares its saved answer without replay");
}
void catchup_is_filtered_and_replacement_preserves_state() {
  Runner r;
  auto &f = r.f;
  f.objects.material[f.a.slot()] = 9;
  f.objects.material[f.c.slot()] = 9;
  set_relation(f.relations.at(3), 3, f.b, f.a, true, f.effects);
  set_relation(f.relations.at(3), 3, f.c, f.a, true, f.effects);
  set_contribution(f.shared, f.gives_to, f.terms.rows[4], f.c, f.a, 7, f.effects);
  const auto ordinary = f.add(R"(return {kind='instance',watches={'novel.signal'},
    on={['novel.signal']=function(self)self.state.seen=true end}})");
  set_instances(f.installed, f.c, std::span(&ordinary, 1), f.scripts);
  project_instance(f.triggers, f.installed, f.scripts, f.terms, f.c);
  f.baseline(f.c);
  set_contribution(f.shared, f.gives_to, f.terms.rows[4], f.c, f.a, 8, f.effects);
  const auto id = f.add(rule());
  DeferredEffects effects;
  r.activate(id, RuleMode::Enable, effects);
  require(effects.background.size() == 1, "catch-up copies only actual shared quantity holders");
  const auto item = effects.background.front();
  require(catch_up_valid(item, f.triggers, f.scripts, f.values()),
          "current catch-up is applicable");
  auto plans = evaluate_catch_up(f.react_state, f.values(), f.scripts, item, 0, 20).invocations;
  require(plans.size() == 1 && plans[0].input.receiver == f.b,
          "catch-up resolves the current selector and term fallback");
  f.run(plans);
  require(r.runs(f.b, id) == 1 && !f.states.contains({f.c, ordinary}),
          "rule catch-up does not consume another script's pending answer");
  const auto foreground = f.changed({f.a, 4});
  require(foreground.size() == 2 && std::any_of(foreground.begin(), foreground.end(),
                                                [&](const auto &plan) {
                                                  return plan.handler.script == ordinary &&
                                                         plan.input.receiver == f.c;
                                                }),
          "queued change independently reaches the rule and ordinary consumer after catch-up");
  f.run(foreground);
  const auto newer = f.add(rule());
  effects = {};
  r.activate(newer, RuleMode::Enable, effects);
  const auto current = dispatch(f.triggers, f.values(), f.b, 4, 0, 20);
  require(current.size() == 1 && current[0].script == newer &&
              !catch_up_valid(item, f.triggers, f.scripts, f.values()) && r.runs(f.b, id) == 2,
          "replacement changes future dispatch and catch-up while retaining prior flat state");
}
void partial_replacement_and_timer_retirement() {
  Runner r;
  auto &f = r.f;
  const auto old = f.add(R"(return {kind='rule',applies_to={material='novel.mat'},
    watches={'novel.heat','novel.signal'},on={
      ['novel.heat']=function(self)self.state.runs=(self.state.runs or 0)+1 end,
      ['novel.signal']=function(self)end},
    tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=1}}}})");
  DeferredEffects effects;
  r.activate(old, RuleMode::Enable, effects);
  effects = {};
  set_state(f.properties, f.processes, f.terms.rows[1], f.a, Value(2.0), 0, 20, effects);
  r.changed(effects);
  const auto newer = f.add(rule());
  effects = {};
  r.activate(newer, RuleMode::Enable, effects);
  const auto retained = dispatch(f.triggers, f.values(), f.a, 1, 0, 20);
  require(f.scripts.rows[old].declaration.active && retained.size() == 1 &&
              retained[0].script == old,
          "partial replacement retains the nonoverlapping live trigger");
  effects = {};
  set_state(f.properties, f.processes, f.terms.rows[1], f.a, Value(3.0), 0, 20, effects);
  r.changed(effects);
  require(r.runs(f.a, old) == 2, "surviving handler keeps its real persistent state");
  const auto threshold = f.add(R"(return {kind='rule',applies_to={material='novel.mat'},
    watches={{term='novel.heat',thresholds={{predicate='above',value=2}}}},
    on={['novel.heat']=function(self)end},
    tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=1}}}})");
  effects = {};
  r.activate(threshold, RuleMode::Enable, effects);
  set_state(f.properties, f.processes, f.terms.rows[1], f.b, Value(0.0), 0, 20, effects);
  set_rate(f.react_state, f.terms.rows[1], f.b, 1, 0, 10, 0, 20, effects);
  const auto timer = threshold_timer(f.react_state, f.values(), f.scripts, {f.b, 1}, 0, 20, {});
  require(timer.has_value(), "current process arms its authored crossing");
  effects = {};
  r.activate(threshold, RuleMode::Disable, effects);
  require(evaluate_watch(f.react_state, f.values(), f.scripts, {*timer->threshold, {}}, 41, 20)
              .invocations.empty(),
          "a disabled rule cannot receive its old pending crossing");
}
} // namespace
int main() {
  first_contribution_and_late_relation();
  catchup_is_filtered_and_replacement_preserves_state();
  partial_replacement_and_timer_retirement();
  std::cout << "primary watches, rule activation, fallback catch-up and retirement passed\n";
}
