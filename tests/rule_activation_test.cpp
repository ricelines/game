#include "bindings/installation/installation.hpp"
#include "drive/drive.hpp"
#include "runtime_fixture.hpp"

#include <iostream>

using namespace amber;
using test_runtime::require;
namespace {
struct Runner {
  test_runtime::Fixture f;
  step::WorkQueues queues;
  std::optional<react::ValueRead> values;
  std::optional<bindings::RuleActivation> activation;
  Runner() {
    values.emplace(f.values());
    activation.emplace(bindings::RuleActivation{f.scripts,
                                                f.rules,
                                                f.triggers,
                                                f.subscriptions,
                                                f.links,
                                                queues,
                                                f.vm,
                                                *values,
                                                f.allocations,
                                                {}});
  }
  void enable(ScriptId id, react::DeferredEffects &effects) {
    bindings::update_rule(*activation, id, bindings::RuleMode::Enable, 0, 20, {}, effects);
  }
  void run(const drive::WatchPlan &plan) {
    for (const auto &entry : plan.invocations) {
      install::InvocationPlan call{entry.handler,       entry.input.receiver, entry.input.sender,
                                   entry.input.trigger, entry.input.payload,  entry.input.acquired};
      f.run(std::span<const install::InvocationPlan>(&call, 1));
    }
  }
  void change(react::DeferredEffects &effects) {
    auto synced = install::sync_rule_watches(f.subscriptions, f.links, f.values(), f.allocations,
                                             f.scripts, f.rules, effects.ordered, 0, 20, {});
    for (auto &dirty : synced.dirty)
      effects.ordered.emplace_back(std::move(dirty));
    std::vector<FactKey> facts;
    for (const auto &effect : effects.ordered)
      if (const auto *work = std::get_if<step::WatchWork>(&effect))
        if (std::find(facts.begin(), facts.end(), work->source) == facts.end())
          facts.push_back(work->source);
    for (auto fact : facts)
      run(drive::plan_watch(f.subscriptions, f.links, f.values(), f.allocations, f.triggers,
                            f.scripts, {fact, {}}, 0, 20));
  }
};
std::string rule(bool explicit_along) {
  return std::string("return {kind='rule',applies_to={material='novel.mat'},watches={") +
         (explicit_along ? "{term='novel.signal',along='novel.link'}" : "'novel.signal'") +
         R"(},on={['novel.signal']=function(self,source,value)
          self.state.runs=(self.state.runs or 0)+1
          self:set('novel.result',value*3)
         end},tests={{when={steps=0},expect={subject='target',term='novel.signal',equals=1}}}})";
}
double runs(Runner &r, Handle object, ScriptId id) {
  auto state = r.f.states.find({object, id});
  return state == r.f.states.end() ? 0 : std::get<double>(state->second.at(std::string("runs")));
}
void activation_and_late_first_contribution() {
  for (bool along : {false, true}) {
    Runner r;
    auto &f = r.f;
    f.objects.material[f.a.slot()] = 9;
    react::set_relation(f.relations.at(3), 3, f.b, f.a, true, f.effects);
    const auto id = f.add(rule(along));
    react::DeferredEffects effects;
    r.enable(id, effects);
    require(effects.background.empty() && f.subscriptions.rows.empty(),
            "activation on empty quantity does not scan or bind unrelated objects");
    react::GivesTo gives;
    effects = {};
    react::set_contribution(f.shared, gives, f.terms.rows[4], f.c, f.a, 2, effects);
    r.change(effects);
    require(runs(r, f.b, id) == 1 && runs(r, f.a, id) == 0 &&
                react::read(f.values(), f.b, 2, 0, 20).value.numeric() == 6,
            "first contribution discovers material consumer through explicit or fallback along");
    require(std::count_if(
                f.links.rows.begin(), f.links.rows.end(),
                [&](const auto &row) { return row.first.consumer == ScriptKey{f.b, id}; }) == 1,
            "watch primary rows retain the actual consumer and immutable script");
    effects = {};
    react::set_contribution(f.shared, gives, f.terms.rows[4], f.c, f.a, 3, effects);
    r.change(effects);
    require(runs(r, f.b, id) == 2, "existing along consumer retains last answer without replay");
  }
}
void late_relation_and_two_hop_fallback() {
  Runner r;
  auto &f = r.f;
  f.objects.material[f.a.slot()] = 9;
  f.objects.material[f.c.slot()] = 9;
  const auto id = f.add(rule(true));
  react::DeferredEffects effects;
  react::GivesTo gives;
  react::set_contribution(f.shared, gives, f.terms.rows[4], f.c, f.a, 4, effects);
  r.enable(id, effects);
  effects = {};
  // b watches c through its explicit relation; c falls back to a through the
  // quantity's own authored along relation. Neither b nor c holds a local row.
  react::set_relation(f.relations.at(3), 3, f.c, f.a, true, effects);
  react::set_relation(f.relations.at(3), 3, f.b, f.c, true, effects);
  r.change(effects);
  require(runs(r, f.b, id) == 1 && react::read(f.values(), f.b, 2, 0, 20).value.numeric() == 12,
          "relation rebind finds unbound explicit-plus-fallback consumer by inverse indexes");
  effects = {};
  react::set_contribution(f.shared, gives, f.terms.rows[4], f.c, f.a, 5, effects);
  r.change(effects);
  require(runs(r, f.b, id) == 2, "two-hop fallback follows later indexed source contribution");
}
void catchup_replacement_and_timer_retirement() {
  Runner r;
  auto &f = r.f;
  f.objects.material[f.a.slot()] = 9;
  react::set_relation(f.relations.at(3), 3, f.b, f.a, true, f.effects);
  react::GivesTo gives;
  react::set_contribution(f.shared, gives, f.terms.rows[4], f.c, f.a, 7, f.effects);
  f.objects.material[f.c.slot()] = 9;
  react::set_relation(f.relations.at(3), 3, f.c, f.a, true, f.effects);
  const auto ordinary = f.add(R"(return {kind='instance',
    watches={{term='novel.signal',along='novel.link'}},
    on={['novel.signal']=function(self)self.state.seen=true end}})");
  install::set_instances(f.installed, f.c, std::span<const ScriptId>(&ordinary, 1), f.scripts);
  install::project_instance(f.triggers, f.installed, f.scripts, f.terms, f.c);
  f.bind(ordinary, f.c);
  react::set_contribution(f.shared, gives, f.terms.rows[4], f.c, f.a, 8, f.effects);
  const auto id = f.add(rule(true));
  react::DeferredEffects effects;
  r.enable(id, effects);
  require(effects.background.size() == 1 && r.queues.background.empty(),
          "activation copies only actual quantity holders and buffers causal catch-up");
  step::TimerWheel timers;
  record::Journal journal(10, {});
  record::Origin final;
  final.cause = 700;
  react::publish_effects(std::move(effects), final, 0, r.queues, timers, journal, 1000);
  require(r.queues.background.size() == 1 && r.queues.background[0].origin.cause == 700,
          "catch-up receives final completed activation cause before enqueue");
  auto item = r.queues.background.front();
  r.queues.background.pop_front();
  auto plans = drive::plan_background(f.subscriptions, f.links, f.values(), f.allocations,
                                      f.triggers, f.scripts, f.rules, item, 0, 20);
  require(plans.invocations.size() == 1 && plans.invocations[0].input.receiver == f.b,
          "background source resolves actual material-selected along consumer");
  r.run(plans);
  require(runs(r, f.b, id) == 1, "late catch-up executes real Luau once");
  const auto &ordinary_last = f.subscriptions.rows.at({f.a, 4}).front();
  require(
      ordinary_last.consumer.script == ordinary &&
          std::get<react::Answer>(ordinary_last.last).value.numeric() == 7 &&
          !f.states.contains({f.c, ordinary}),
      "background rule catch-up neither observes nor executes another consumer's pending change");
  const auto ordinary_plans = f.changed({f.a, 4});
  require(ordinary_plans.size() == 1 && ordinary_plans.front().handler.script == ordinary,
          "unrelated pending foreground watch remains deliverable after catch-up");
  f.run(ordinary_plans);
  const auto newer = f.add(rule(true));
  effects = {};
  r.enable(newer, effects);
  require(std::none_of(f.links.rows.begin(), f.links.rows.end(),
                       [&](const auto &row) { return row.first.consumer.script == id; }) &&
              !f.subscriptions.by_consumer.contains({f.b, id}) &&
              !install::catch_up_valid(item, f.rules, f.values(), 0, 20),
          "superseded rule loses exact watches and pending obsolete background version");
  require(runs(r, f.b, id) == 1, "replacement preserves old flat state and never replays history");
  react::Invocation stale;
  stale.self = {f.b, id};
  stale.trigger = 4;
  stale.payload = Value(99.0);
  stale.hz = 20;
  stale.state = &f.states.at({f.b, id});
  stale.effects = &effects;
  const auto stale_result = react::run_handler(f.vm, plans.invocations[0].handler, stale);
  require(stale_result.status == react::RunOutcome::Status::Missing &&
              !f.scripts.rows[id].declaration.active && runs(r, f.b, id) == 1,
          "fully superseded script cannot execute through a stale cached handler reference");
  const auto threshold = f.add(R"(return {kind='rule',applies_to={material='novel.mat'},
    watches={{term='novel.heat',thresholds={{predicate='above',value=2}}}},
    on={['novel.heat']=function(self)end},
    tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=1}}}})");
  effects = {};
  r.enable(threshold, effects);
  f.state(f.b, 1, 0);
  react::set_rate(f.properties, f.processes, f.terms.rows[1], f.b, 1, 0, 10, 0, 20, effects);
  auto synced = install::sync_rule_watches(f.subscriptions, f.links, f.values(), f.allocations,
                                           f.scripts, f.rules, effects.ordered, 0, 20, {});
  require(!synced.timers.empty(), "current process arms authored threshold timer");
  const auto token = *synced.timers.front().threshold;
  effects = {};
  bindings::update_rule(*r.activation, threshold, bindings::RuleMode::Disable, 0, 20, {}, effects);
  require(!react::valid_threshold(token, identity(f.objects), f.processes, f.subscriptions),
          "disabled rule's existing threshold token cannot dispatch a stale cached handler");
}
void partial_replacement_keeps_live_cache_and_prior_state() {
  Runner r;
  auto &f = r.f;
  const auto old = f.add(R"(return {kind='rule',applies_to={material='novel.mat'},
    watches={'novel.heat','novel.signal'},on={
    ['novel.heat']=function(self)self.state.heats=(self.state.heats or 0)+1 end,
    ['novel.signal']=function(self)self.state.signals=(self.state.signals or 0)+1 end},
    tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=1}}}})");
  react::DeferredEffects effects;
  r.enable(old, effects);
  effects = {};
  react::set_state(f.properties, f.processes, f.terms.rows[1], f.a, Value(2.0), 0, 20, effects);
  r.change(effects);
  require(std::get<double>(f.states.at({f.a, old}).at(std::string("heats"))) == 1,
          "original current trigger executes actual script state");
  const auto newer = f.add(rule(false));
  effects = {};
  r.enable(newer, effects);
  require(
      f.scripts.rows[old].declaration.active &&
          std::count_if(
              f.links.rows.begin(), f.links.rows.end(),
              [&](const auto &row) { return row.first.consumer == ScriptKey{f.a, old}; }) == 1,
      "partial replacement retires only replaced watch, preserving cached live sibling trigger");
  effects = {};
  react::set_state(f.properties, f.processes, f.terms.rows[1], f.a, Value(3.0), 0, 20, effects);
  r.change(effects);
  require(std::get<double>(f.states.at({f.a, old}).at(std::string("heats"))) == 2,
          "surviving old version trigger continues through actual VM without resetting state");
}
} // namespace
int main() {
  activation_and_late_first_contribution();
  late_relation_and_two_hop_fallback();
  catchup_replacement_and_timer_retirement();
  partial_replacement_keeps_live_cache_and_prior_state();
  std::cout << "indexed rule activation, along/fallback catch-up and retirement checks passed\n";
}
