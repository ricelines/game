#include "react/react.hpp"
#include "runtime_fixture.hpp"

#include <iostream>
#include <stdexcept>

using namespace amber;
using namespace amber::react;
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
using Fixture = test_runtime::Fixture;

void real_watch_drives_authored_state() {
  Fixture f;
  f.state(f.a, 1, 1);
  auto model =
      f.add(R"(return {kind='model',watches={'novel.heat'},actions={['novel.touch']={right='use'}},
    on={['novel.heat']=function(self) self:set('novel.result',self:get('novel.heat')*2)
      self.state.runs=(self.state.runs or 0)+1 end,
      ['novel.cue']=function(self)self.state.cued=true end,
      ['novel.touch']=function(self)self:set('novel.result',99)end}})");
  f.models.rows[1].script = model;
  project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  f.baseline(f.a);
  require(f.states.empty(), "installation never replays prior events/actions");
  auto event = react::dispatch(f.triggers, f.values(), f.a, 6, 0, 20);
  auto action = react::dispatch(f.triggers, f.values(), f.a, 7, 0, 20);
  require(event.size() == 1 && action.size() == 1,
          "complete handler index projects unwatched event/action functions");
  PlannedInvocation cue{event[0], {}};
  cue.input.receiver = f.a;
  cue.input.trigger = 6;
  f.run(std::span(&cue, 1));
  require(std::get<bool>(f.states.at({f.a, model}).values.at(StateKey{std::string("cued")})),
          "projected event executes actual authored function");
  require(f.changed({f.a, 1}).empty(), "installation creates a baseline, not a callback");
  f.state(f.a, 1, 3);
  auto plans = f.changed({f.a, 1});
  require(plans.size() == 1, "one changed subscription yields one invocation");
  f.run(plans);
  require(read(f.values(), f.a, 2, 0, 20).value.numeric() == 6,
          "novel watch runs actual Luau setter");
  require(std::get<double>(f.states.at({f.a, model}).values.at(StateKey{std::string("runs")})) == 1,
          "real persistent state changed once");
  require(f.changed({f.a, 1}).empty(), "unchanged typed answer suppresses repeated callbacks");
  auto extra = f.add(
      R"(return {kind='instance',watches={'novel.heat'},on={['novel.heat']=function(self)self:set('novel.result',20)end}})",
      2);
  const std::array<ScriptId, 1> ids{extra};
  set_instances(f.installed, f.a, ids, f.scripts);
  project_instance(f.triggers, f.installed, f.scripts, f.terms, f.a);
  f.baseline(f.a);
  auto rule = f.add(R"(return {kind='rule',applies_to={material='novel.mat'},watches={'novel.heat'},
    on={['novel.heat']=function(self)self:set('novel.result',10)end},tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=1}}}})",
                    3);
  add_rule(f.rules, f.scripts, f.terms, rule);
  project_rules(f.triggers, f.rules);
  f.state(f.a, 1, 5);
  plans = f.changed({f.a, 1});
  require(plans.size() == 3 && plans[0].handler.script == rule &&
              plans[1].handler.script == model && plans[2].handler.script == extra,
          "broad-to-narrow exact consumers, no multiplied sibling dispatch");
  f.run(plans);
  require(read(f.values(), f.a, 2, 0, 20).value.numeric() == 20,
          "all authored handlers execute in declared dispatch order");
  set_instances(f.installed, f.a, {}, f.scripts);
  project_instance(f.triggers, f.installed, f.scripts, f.terms, f.a);
  f.state(f.a, 1, 6);
  require(f.changed({f.a, 1}).size() == 2, "removed instance contributes no subscription/handler");
}
void term_fallback_tracks_own_row_existence() {
  Fixture f;
  f.shared.resize(5);
  f.shared[4].set(f.b.slot(), Shared{10, {}});
  set_relation(f.relations.at(3), 3, f.a, f.b, true, f.effects);
  auto id = f.add(
      R"(return {kind='model',watches={'novel.signal'},on={['novel.signal']=function(self,source,value)self.state.signal=value end}})");
  f.models.rows[1].script = id;
  project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  f.baseline(f.a);
  f.shared[4].set(f.a.slot(), Shared{0, {}});
  auto plans = f.changed({f.a, 4});
  require(plans.size() == 1 && std::get<Value>(plans[0].input.payload).numeric() == 0,
          "explicit own zero row wins over fallback");
  f.run(plans);
  f.shared[4].erase(f.a.slot());
  plans = f.changed({f.a, 4});
  require(plans.size() == 1 && std::get<Value>(plans[0].input.payload).numeric() == 10,
          "own row removal reveals fallback and preserves the previous answer");
}
void along_changes_use_first_source_local_override_and_one_hop() {
  Fixture f;
  ObjectInit init;
  init.model = 1;
  const auto second = create(f.objects, init);
  f.shared.resize(5);
  f.shared[4].set(f.a.slot(), Shared{10, {}});
  f.shared[4].set(second.slot(), Shared{20, {}});
  set_relation(f.relations.at(3), 3, f.b, f.a, true, f.effects);
  set_relation(f.relations.at(3), 3, f.b, second, true, f.effects);
  set_relation(f.relations.at(3), 3, f.c, f.b, true, f.effects);
  const auto script = f.add(R"(return {kind='instance',watches={'novel.signal'},on={
    ['novel.signal']=function(self,source,value)
      assert(self:get('novel.signal')==value)
      self.state.source=source
      self.state.value=value
    end}})");
  for (auto consumer : {f.b, f.c}) {
    set_instances(f.installed, consumer, std::span(&script, 1), f.scripts);
    project_instance(f.triggers, f.installed, f.scripts, f.terms, consumer);
    f.baseline(consumer);
  }
  f.shared[4].set(f.a.slot(), Shared{11, {}});
  auto plans = f.changed({f.a, 4});
  require(plans.size() == 1 && plans[0].input.receiver == f.b && plans[0].input.sender == f.a,
          "source change wakes its direct consumer, not the next relation hop");
  f.run(plans);
  f.shared[4].set(second.slot(), Shared{22, {}});
  require(f.changed({second, 4}).empty(), "a non-first source cannot replace the effective value");
  require(!evaluate_watch(f.react_state, f.values(), f.scripts, {{second, 4}, {}}, 0, 20).modeled,
          "a watcher of another first source does not suppress this source's missing-rule gap");
  set_relation(f.relations.at(3), 3, f.b, f.a, false, f.effects);
  plans = f.changed({f.b, 3});
  require(plans.size() == 1 && plans[0].input.sender == second &&
              std::get<Value>(plans[0].input.payload).numeric() == 22,
          "removing the first link reveals the next source and supplies its handle");
  f.run(plans);
  f.shared[4].set(f.b.slot(), Shared{0, {}});
  plans = f.changed({f.b, 4});
  require(plans.size() == 2, "an explicit zero overrides fallback and reaches direct consumers");
  f.run(plans);
  f.shared[4].set(second.slot(), Shared{23, {}});
  require(f.changed({second, 4}).empty(), "local override suppresses an upstream value change");
  require(!evaluate_watch(f.react_state, f.values(), f.scripts, {{second, 4}, {}}, 0, 20).modeled,
          "a local override does not count as modeling its unused upstream source");
  f.shared[4].erase(f.b.slot());
  plans = f.changed({f.b, 4});
  require(plans.size() == 2, "local removal changes the owner and its immediate consumer");
  for (const auto &plan : plans)
    if (plan.input.receiver == f.b)
      require(plan.input.sender == second && std::get<Value>(plan.input.payload).numeric() == 23,
              "local removal restores the current first source");
    else
      require(plan.input.receiver == f.c && plan.input.sender == f.b &&
                  !std::get<Value>(plan.input.payload).present(),
              "shared fallback never follows a second hop");
  f.run(plans);
  set_relation(f.relations.at(3), 3, f.b, second, false, f.effects);
  plans = f.changed({f.b, 3});
  require(plans.size() == 1 && plans[0].input.receiver == f.b &&
              !std::get<Value>(plans[0].input.payload).present(),
          "removing the final edge delivers absence to its consumer");
  f.run(plans);
}
void threshold_restore_and_selected_instance_codec() {
  Fixture f;
  f.state(f.a, 1, 0);
  auto id = f.add(
      R"(return {kind='instance',watches={{term='novel.heat',thresholds={{predicate='above',value=2}}}},
    on={['novel.heat']=function(self)self.state.crossed=true end}})");
  const std::array<ScriptId, 1> ids{id};
  set_instances(f.installed, f.a, ids, f.scripts);
  set_instances(f.installed, f.b, ids, f.scripts);
  project_instance(f.triggers, f.installed, f.scripts, f.terms, f.a);
  set_rate(f.react_state, f.terms.rows[1], f.a, 1, 0, 10, 0, 20, f.effects);
  f.baseline(f.a);
  const auto timer = threshold_timer(f.react_state, f.values(), f.scripts, {f.a, 1}, 0, 20, {});
  require(timer && timer->due == 41, "real process threshold schedules strict boundary");
  require(f.changed({f.a, 1}, 40).empty(), "strict predicate remains silent at its boundary");
  snap::Buffer bytes;
  snap::Output out{bytes};
  const std::array<Handle, 1> chosen{f.a};
  write_instances(out, f.installed, identity(f.objects), std::span<const Handle>(chosen));
  write_script_states(out, f.states, f.watch_answers, react::Placements{},
                      SnapshotSelection(chosen));
  bytes.resize(out.adapter().writtenBytesCount());
  const auto saved = f.watch_answers.at({f.a, 1});
  snap::Input in{bytes.data(), bytes.size()};
  read_instances(in, f.installed, f.objects.model.size());
  {
    react::Placements placed;
    read_script_states(in, f.states, f.watch_answers, placed);
  }
  snap::check_complete(in);
  require(f.installed.get(f.a.slot()) && !f.installed.get(f.b.slot()),
          "selected installation snapshot excludes unrelated object");
  require(f.watch_answers.at({f.a, 1}) == saved, "restore preserves the previous watched answer");
  auto plans = f.changed({f.a, 1}, 41);
  require(plans.size() == 1, "restored crossing yields actual invocation");
  f.run(plans, 41);
  require(std::get<bool>(f.states.at({f.a, id}).values.at(StateKey{std::string("crossed")})),
          "threshold invokes real Luau after restore");
  set_rate(f.react_state, f.terms.rows[1], f.a, -1, 0, 10, 41, 20, f.effects);
  require(f.changed({f.a, 1}, 41).empty(),
          "unchanged predicate suppresses a previously delivered crossing");
  const auto falling = threshold_timer(f.react_state, f.values(), f.scripts, {f.a, 1}, 41, 20, {});
  require(falling && falling->due == 42, "falling threshold is rearmed in opposite direction");
}
void rule_versions_partial_replacement_and_catchup() {
  Fixture f;
  f.shared.resize(5);
  f.shared[4].set(f.a.slot(), Shared{1, {}});
  f.shared[4].set(f.b.slot(), Shared{2, {}});
  auto old = f.add(
      R"(return {kind='rule',applies_to={material='novel.mat'},watches={'novel.heat','novel.signal'},
    on={['novel.heat']=function(self)end,['novel.signal']=function(self)self:set('novel.result',self:get('novel.signal')+1)end},
    tests={{when={steps=0},expect={subject='target',term='novel.signal',equals=1}}}})");
  auto added = add_rule(f.rules, f.scripts, f.terms, old);
  project_rules(f.triggers, f.rules);
  auto signal = added.active[1];
  if (signal.trigger != 4)
    signal = added.active[0];
  auto items = catch_up(f.values(), f.scripts, signal, {});
  require(items.size() == 2, "late rule copies actual shared slots once");
  require(catch_up_valid(items[0], f.triggers, f.scripts, f.values()),
          "new rule catch-up is applicable");
  auto caught = evaluate_catch_up(f.react_state, f.values(), f.scripts, items[0], 0, 20);
  require(caught.invocations.size() == 1, "catch-up selects the current matching rule");
  f.run(caught.invocations);
  require(read(f.values(), f.a, 2, 0, 20).value.numeric() == 2,
          "late catchup executes real authored rule");
  f.objects.material[f.b.slot()] = 9;
  require(catch_up_valid(items[1], f.triggers, f.scripts, f.values()),
          "catchup source validity is distinct from current consumer selector filtering");
  std::deque<BackgroundItem> background;
  for (const auto &item : items)
    background.push_back(item);
  set_rule_enabled(f.rules, {4, 8, false}, false);
  project_rules(f.triggers, f.rules);
  cancel_catch_up(background, signal);
  require(background.empty() && !catch_up_valid(items[0], f.triggers, f.scripts, f.values()),
          "disabled rule cancels pending catch-up");
  set_rule_enabled(f.rules, {4, 8, false}, true);
  project_rules(f.triggers, f.rules);
  auto newer = f.add(
      R"(return {kind='rule',applies_to={material='novel.mat'},watches={'novel.heat'},on={['novel.heat']=function(self)end},
    tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=1}}}})");
  add_rule(f.rules, f.scripts, f.terms, newer);
  project_rules(f.triggers, f.rules);
  const auto heat = dispatch(f.triggers, f.values(), f.a, 1, 0, 20);
  const auto remaining = dispatch(f.triggers, f.values(), f.a, 4, 0, 20);
  require(heat.size() == 1 && heat[0].script == newer && remaining.size() == 1 &&
              remaining[0].script == old,
          "partial replacement preserves the old script's nonoverlapping trigger");
  add_rule(f.rules, f.scripts, f.terms, old);
  project_rules(f.triggers, f.rules);
  require(!catch_up_valid(items[0], f.triggers, f.scripts, f.values()),
          "restoring same script creates new version and rejects obsolete catchup");
  require(f.rules.history.size() == 5, "immutable rule version history retained");
}
void unwatched_quantity_never_creates_catchup() {
  Fixture f;
  f.shared.resize(5);
  f.shared[4].set(f.a.slot(), Shared{1, {}});
  auto rule = f.add(R"(return {kind='rule',applies_to={material='novel.mat'},
    on={['novel.signal']=function(self)error('not a subscription')end},
    tests={{when={steps=0},expect={subject='target',term='novel.signal',equals=1}}}})");
  auto added = add_rule(f.rules, f.scripts, f.terms, rule);
  require(added.active.size() == 1 && catch_up(f.values(), f.scripts, added.active[0], {}).empty(),
          "complete handler index does not turn an unwatched quantity into a catch-up consumer");
}
void queued_watch_uses_current_selector() {
  Fixture f;
  auto rule = f.add(R"(return {kind='rule',applies_to={flag='novel.ready'},
    watches={'novel.heat'},on={['novel.heat']=function(self)
      self:set('novel.result',self:get('novel.heat')+40)
      self.state.count=(self.state.count or 0)+1
    end},tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=0}}}})");
  add_rule(f.rules, f.scripts, f.terms, rule);
  project_rules(f.triggers, f.rules);
  f.state(f.a, 5, 0);
  f.effects = {};
  f.state(f.a, 1, 5); // Deliberately dirties quantity before the selector in one invocation.
  f.state(f.a, 5, 1);
  auto plans = f.changed({f.a, 1});
  require(plans.size() == 1,
          "quantity-before-flag mutation cannot be swallowed by a current baseline");
  f.run(plans);
  require(read(f.values(), f.a, 2, 0, 20).value.numeric() == 45 &&
              std::get<double>(f.states.at({f.a, rule}).values.at(std::string("count"))) == 1,
          "new flag-selected watch executes the actual current-state Luau handler once");
  require(f.changed({f.a, 1}).empty(), "an unchanged answer does not replay the rule");
  f.effects = {};
  f.state(f.a, 5, 0);
  f.state(f.a, 1, 6);
  require(f.changed({f.a, 1}).empty(),
          "a rule whose current selector is false does not handle the changed value");
}
} // namespace
int main() {
  real_watch_drives_authored_state();
  term_fallback_tracks_own_row_existence();
  along_changes_use_first_source_local_override_and_one_hop();
  threshold_restore_and_selected_instance_codec();
  rule_versions_partial_replacement_and_catchup();
  unwatched_quantity_never_creates_catchup();
  queued_watch_uses_current_selector();
  std::cout << "installation, shared fallback, real VM, catch-up and restore checks passed\n";
}
