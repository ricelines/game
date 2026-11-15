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
  f.bind(model, f.a);
  require(f.states.empty(), "installation never replays prior events/actions");
  auto event = react::dispatch(f.triggers, identity(f.objects), {}, f.a, 6);
  auto action = react::dispatch(f.triggers, identity(f.objects), {}, f.a, 7);
  require(event.size() == 1 && action.size() == 1,
          "complete handler index projects unwatched event/action functions");
  const InvocationPlan cue{event[0], f.a, {}, 6, Value{}, {}};
  f.run(std::span<const InvocationPlan>(&cue, 1));
  require(std::get<bool>(f.states.at({f.a, model}).at(StateKey{std::string("cued")})),
          "projected event executes actual authored function");
  require(f.changed({f.a, 1}).empty(), "installation creates a baseline, not a callback");
  f.state(f.a, 1, 3);
  auto plans = f.changed({f.a, 1});
  require(plans.size() == 1, "one changed subscription yields one invocation");
  f.run(plans);
  require(read(f.values(), f.a, 2, 0, 20).value.numeric() == 6,
          "novel watch runs actual Luau setter");
  require(std::get<double>(f.states.at({f.a, model}).at(StateKey{std::string("runs")})) == 1,
          "real persistent state changed once");
  require(f.changed({f.a, 1}).empty(), "unchanged typed answer suppresses repeated callbacks");
  auto extra = f.add(
      R"(return {kind='instance',watches={'novel.heat'},on={['novel.heat']=function(self)self:set('novel.result',20)end}})",
      2);
  const std::array<ScriptId, 1> ids{extra};
  set_instances(f.installed, f.a, ids, f.scripts);
  project_instance(f.triggers, f.installed, f.scripts, f.terms, f.a);
  f.bind(extra, f.a);
  auto rule = f.add(R"(return {kind='rule',applies_to={material='novel.mat'},watches={'novel.heat'},
    on={['novel.heat']=function(self)self:set('novel.result',10)end},tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=1}}}})",
                    3);
  add_rule(f.rules, f.scripts, f.terms, rule);
  project_rules(f.triggers, f.rules);
  f.bind(rule, f.a);
  f.state(f.a, 1, 5);
  plans = f.changed({f.a, 1});
  require(plans.size() == 3 && plans[0].handler.script == rule &&
              plans[1].handler.script == model && plans[2].handler.script == extra,
          "broad-to-narrow exact consumers, no multiplied sibling dispatch");
  f.run(plans);
  require(read(f.values(), f.a, 2, 0, 20).value.numeric() == 20,
          "all authored handlers execute in declared dispatch order");
  remove_watches(f.subscriptions, {f.a, extra});
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
  f.bind(id, f.a);
  require(watch_sources(f.subscriptions, ScriptKey{f.a, id}) == std::vector<FactKey>{{f.b, 4}},
          "Term.along fallback bound to first outgoing subject");
  f.shared[4].set(f.a.slot(), Shared{0, {}});
  auto plans = f.changed({f.a, 4});
  require(plans.size() == 1 && std::get<Value>(plans[0].payload).numeric() == 0,
          "explicit own zero row wins over fallback");
  f.run(plans);
  f.shared[4].erase(f.a.slot());
  plans = f.changed({f.a, 4});
  require(plans.size() == 1 && std::get<Value>(plans[0].payload).numeric() == 10,
          "own row removal rebinds and preserves previous answer");
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
  set_rate(f.properties, f.processes, f.terms.rows[1], f.a, 1, 0, 10, 0, 20, f.effects);
  auto changes = sync_watches(f.subscriptions, f.values(), f.allocations, {f.a, id},
                              f.scripts.rows[id].declaration, 0, 20, {}, SyncMode::Initial);
  require(changes.timers.size() == 1 && changes.timers[0].due == 41,
          "real process threshold schedules strict boundary");
  const auto timer = changes.timers[0];
  require(
      valid_threshold(*timer.threshold, identity(f.objects), f.processes, f.subscriptions, 41, 20),
      "scheduled predicate crosses at its due step");
  snap::Buffer bytes;
  snap::Output out{bytes};
  const std::array<Handle, 1> chosen{f.a};
  write_instances(out, f.installed, identity(f.objects), std::span<const Handle>(chosen));
  write_script_states(out, f.states, f.subscriptions, SnapshotSelection(chosen));
  out.adapter().flush();
  bytes.resize(out.adapter().writtenBytesCount());
  InstanceScripts loaded;
  ScriptStates loaded_states;
  Subscriptions loaded_subs;
  snap::Input in{bytes.data(), bytes.size()};
  read_instances(in, loaded, f.objects.model.size());
  read_script_states(in, loaded_states, loaded_subs);
  snap::check_complete(in);
  require(loaded.get(f.a.slot()) && !loaded.get(f.b.slot()),
          "selected installation snapshot excludes unrelated object");
  auto restored = sync_watches(loaded_subs, f.values(), f.allocations, {f.a, id},
                               f.scripts.rows[id].declaration, 0, 20, {}, SyncMode::Restore);
  require(
      restored.dirty.empty() && restored.timers.empty() &&
          valid_threshold(*timer.threshold, identity(f.objects), f.processes, loaded_subs, 41, 20),
      "restore preserves answers and does not duplicate timers");
  auto result = check_changed(loaded_subs, f.values(), f.allocations, {f.a, 1}, 41, 20, {});
  auto plans = plan_deliveries(result, f.triggers, f.scripts, f.values(), 41, 20);
  require(plans.size() == 1, "restored crossing yields actual invocation");
  std::swap(f.subscriptions, loaded_subs);
  f.run(plans, 41);
  std::swap(f.subscriptions, loaded_subs);
  require(std::get<bool>(f.states.at({f.a, id}).at(StateKey{std::string("crossed")})),
          "threshold invokes real Luau after restore");
  check_changed(loaded_subs, f.values(), f.allocations, {f.a, 1}, 41, 20, {});
  set_rate(f.properties, f.processes, f.terms.rows[1], f.a, -1, 0, 10, 41, 20, f.effects);
  require(!valid_threshold(*timer.threshold, identity(f.objects), f.processes, loaded_subs, 41, 20),
          "unchanged predicate suppresses a previously delivered crossing");
  require(
      check_changed(loaded_subs, f.values(), f.allocations, {f.a, 1}, 41, 20, {}).timers[0].due ==
          42,
      "falling threshold is rearmed in opposite direction");
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
  f.bind(old, f.a);
  auto signal = added.active[1];
  if (signal.trigger != 4)
    signal = added.active[0];
  auto items = catch_up(f.shared, identity(f.objects), f.scripts, f.terms, signal, {});
  require(items.size() == 2, "late rule copies actual shared slots once");
  require(catch_up_valid(items[0], f.rules, f.values(), 0, 20), "new rule catch-up is applicable");
  InvocationPlan plan{signal.handler, f.a, f.a, 4, Value(1.0), {f.a}};
  f.run(std::span<const InvocationPlan>(&plan, 1));
  require(read(f.values(), f.a, 2, 0, 20).value.numeric() == 2,
          "late catchup executes real authored rule");
  f.objects.material[f.b.slot()] = 9;
  require(catch_up_valid(items[1], f.rules, f.values(), 0, 20),
          "catchup source validity is distinct from current consumer selector filtering");
  step::WorkQueues queue;
  for (const auto &item : items)
    queue.background.push_back(item);
  set_rule_enabled(f.rules, {4, 8, false}, false);
  cancel_catch_up(queue, signal);
  require(queue.background.empty() && !catch_up_valid(items[0], f.rules, f.values(), 0, 20),
          "disabled rule cancels pending catch-up");
  set_rule_enabled(f.rules, {4, 8, false}, true);
  auto newer = f.add(
      R"(return {kind='rule',applies_to={material='novel.mat'},watches={'novel.heat'},on={['novel.heat']=function(self)end},
    tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=1}}}})");
  add_rule(f.rules, f.scripts, f.terms, newer);
  project_rules(f.triggers, f.rules);
  const auto allowed = rule_watch_terms(f.rules, f.values(), f.a, old, 0, 20);
  require(allowed == std::vector<TermId>{4},
          "partial replacement preserves old script's other active trigger");
  sync_watches(f.subscriptions, f.values(), f.allocations, {f.a, old},
               f.scripts.rows[old].declaration, 0, 20, {}, SyncMode::Change,
               std::span<const TermId>(allowed));
  require(f.subscriptions.rows.size() == 1 && f.subscriptions.rows.begin()->second.size() == 1 &&
              f.subscriptions.rows.begin()->second[0].index == 1,
          "only retired watch removed, original watch index retained");
  add_rule(f.rules, f.scripts, f.terms, old);
  require(!catch_up_valid(items[0], f.rules, f.values(), 0, 20),
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
  require(
      added.active.size() == 1 &&
          catch_up(f.shared, identity(f.objects), f.scripts, f.terms, added.active[0], {}).empty(),
      "complete handler index does not turn an unwatched quantity into a catch-up consumer");
}
void selector_changes_publish_after_subscription_rebinding() {
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
  auto synced = sync_rule_watches(f.subscriptions, f.values(), f.allocations, f.scripts, f.rules,
                                  f.effects.ordered, 0, 20, {});
  require(synced.dirty.size() == 1 && synced.dirty[0].source == FactKey{f.a, 1},
          "new applicable rule subscription queues its current fact before any dirty publication");
  auto plans = f.changed({f.a, 1});
  require(plans.size() == 1,
          "quantity-before-flag mutation cannot be swallowed by a current baseline");
  f.run(plans);
  require(read(f.values(), f.a, 2, 0, 20).value.numeric() == 45 &&
              std::get<double>(f.states.at({f.a, rule}).at(std::string("count"))) == 1,
          "new flag-selected watch executes the actual current-state Luau handler once");
  auto unchanged = sync_rule_watches(f.subscriptions, f.values(), f.allocations, f.scripts, f.rules,
                                     f.effects.ordered, 0, 20, {});
  require(unchanged.dirty.empty(), "existing applicable subscriptions do not get replayed");
  f.effects = {};
  f.state(f.a, 5, 0);
  sync_rule_watches(f.subscriptions, f.values(), f.allocations, f.scripts, f.rules,
                    f.effects.ordered, 0, 20, {});
  require(watch_sources(f.subscriptions, ScriptKey{f.a, rule}).empty(),
          "false selector removes the formerly applicable world-rule consumer");
}
} // namespace
int main() {
  real_watch_drives_authored_state();
  term_fallback_tracks_own_row_existence();
  threshold_restore_and_selected_instance_codec();
  rule_versions_partial_replacement_and_catchup();
  unwatched_quantity_never_creates_catchup();
  selector_changes_publish_after_subscription_rebinding();
  std::cout << "installation, shared fallback, real VM, catch-up and restore checks passed\n";
}
