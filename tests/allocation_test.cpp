#include "runtime_fixture.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

using namespace amber;
using namespace amber::react;
using test_runtime::require;
namespace {
void near(double actual, double expected, const char *message) {
  require(std::abs(actual - expected) < 1e-9, message);
}
struct Fixture {
  test_runtime::Fixture f{1000};
  amber::react::State &react_state = f.react_state;

  MeterRows &meters = react_state.meters;
  Handle subject = f.c, third, supplier, substitute;
  ScriptId consumer;
  Step now = 0;
  Fixture(Combine combine = Combine::ShareEqual) {
    f.terms.rows.resize(13);
    for (TermId id : {10u, 11u, 12u}) {
      auto &term = f.terms.rows[id];
      term.id = id;
      term.name = id == 10 ? "novel.available" : id == 11 ? "novel.wanted" : "novel.priority";
      f.terms.by_name[term.name] = id;
    }
    f.terms.rows[10].combine = Combine::Sum;
    f.terms.rows[11].combine = combine;
    f.terms.rows[11].paired = 10;
    f.terms.rows[11].along = 3;
    f.terms.rows[12].source = Source::Setting;
    if (combine == Combine::SharePriority)
      f.terms.rows[11].priority = 12;
    validate_shared_terms(f.terms);
    ObjectInit init;
    init.model = 1;
    init.material = 8;
    third = create(f.objects, init);
    supplier = create(f.objects, init);
    substitute = create(f.objects, init);
    consumer = f.add(R"(return {kind='instance', watches={'novel.wanted'},
      on={['novel.wanted']=function(self,subject,total)
        self.state.count=(self.state.count or 0)+1
        self.state.total=total
        self.state.allocation=if subject then self:get_share(subject,'novel.wanted') else 0
      end}})");
    for (auto object : {f.a, f.b, third}) {
      set_relation(f.relations.at(3), 3, object, subject, true, f.effects);
      set_instances(f.installed, object, std::span(&consumer, 1), f.scripts);
      project_instance(f.triggers, f.installed, f.scripts, f.terms, object);
    }
    f.effects = {};
  }
  void contribute(Handle from, TermId term, double amount, Handle target = {}) {
    set_contribution(f.shared, f.gives_to, f.terms.rows[term], from, target ? target : subject,
                     amount, f.effects);
  }
  void publish() {
    reconcile_allocations(f.values(), f.react_state, f.effects, now, 20);
    std::vector<FactKey> facts;
    for (const auto &effect : f.effects.ordered)
      if (const auto *work = std::get_if<react::WatchWork>(&effect);
          work && std::find(facts.begin(), facts.end(), work->source) == facts.end())
        facts.push_back(work->source);
    f.effects = {};
    for (auto fact : facts)
      f.run(f.changed(fact, now), now);
    f.effects = {};
  }
  void baseline(double available = 7) {
    contribute(supplier, 10, available);
    contribute(f.a, 11, 6);
    contribute(f.b, 11, 6);
    contribute(third, 11, 1);
    reconcile_allocations(f.values(), f.react_state, f.effects, now, 20);
    for (auto object : {f.a, f.b, third})
      f.bind(consumer, object);
    f.effects = {};
  }
  double count(Handle object) const {
    const auto state = f.states.find({object, consumer});
    if (state == f.states.end())
      return 0;
    const auto value = state->second.values.find(std::string("count"));
    return value == state->second.values.end() ? 0 : std::get<double>(value->second);
  }
};
void redistribution_and_lifetime_meters() {
  Fixture x;
  x.baseline();
  near(get_share(x.f.allocations, x.subject, 11, x.f.a), 3, "equal waterfill starts at three");
  x.contribute(x.f.a, 11, 1);
  x.contribute(x.f.b, 11, 11);
  x.publish();
  require(x.count(x.f.a) == 1 && x.count(x.f.b) == 1 && x.count(x.third) == 0,
          "same-total redistribution wakes only demanders whose allocation changed");
  near(std::get<double>(x.f.states.at({x.f.a, x.consumer}).values.at(std::string("total"))), 13,
       "watch payload remains total demand");
  near(std::get<double>(x.f.states.at({x.f.b, x.consumer}).values.at(std::string("allocation"))), 5,
       "actual Luau reads the reconciled allocation");
  x.now = 20;
  x.contribute(x.supplier, 10, 0);
  x.contribute(x.substitute, 10, 7);
  x.publish();
  require(x.count(x.f.a) == 1 && x.count(x.f.b) == 1 && x.count(x.third) == 0,
          "same-total supplier replacement leaves unchanged demanders asleep");
  near(metered(x.meters.at({x.subject, x.supplier, 10, true}), 40, 20), 7,
       "withdrawn supplier retains its integral and stops earning");
  require(!x.f.allocations.values.contains({x.subject, x.supplier, 10, true}),
          "withdrawn supplier has no zero current-allocation row");
  near(metered(x.meters.at({x.subject, x.substitute, 10, true}), 40, 20), 7,
       "replacement supplier starts earning at replacement time");
  x.now = 40;
  x.contribute(x.substitute, 10, 0);
  x.contribute(x.supplier, 10, 7);
  x.publish();
  near(metered(x.meters.at({x.subject, x.supplier, 10, true}), 60, 20), 14,
       "rejoining preserves the prior lifetime integral");
}
void thresholds_and_relation_rebinding() {
  Fixture x;
  x.baseline();
  x.f.scripts.rows[x.consumer].declaration.watches[0].thresholds = {
      {Threshold::Predicate::Above, 1000}};
  for (auto h : {x.f.a, x.f.b, x.third})
    x.f.bind(x.consumer, h);
  x.contribute(x.f.a, 11, 8);
  x.publish();
  require(x.count(x.f.a) == 0 && x.count(x.f.b) == 0,
          "unchanged predicates suppress aggregate-only demand changes");
  x.contribute(x.f.a, 11, 1);
  x.contribute(x.f.b, 11, 13);
  x.publish();
  require(x.count(x.f.a) == 1 && x.count(x.f.b) == 1 && x.count(x.third) == 0,
          "own allocation changes remain observable with aggregate thresholds");
  ObjectInit init;
  init.model = 1;
  const auto other = create(x.f.objects, init);
  x.contribute(x.supplier, 10, .5, other);
  x.contribute(x.f.a, 11, 15, other);
  set_relation(x.f.relations.at(3), 3, x.f.a, x.subject, false, x.f.effects);
  set_relation(x.f.relations.at(3), 3, x.f.a, other, true, x.f.effects);
  x.publish();
  require(x.count(x.f.a) == 2 && x.count(x.f.b) == 1,
          "the shared term's relation fallback compares the old and new allocations");
  near(std::get<double>(x.f.states.at({x.f.a, x.consumer}).values.at(std::string("allocation"))),
       .5, "rebound Luau consumer sees the actual new subject");
}
void priority_scopes_and_retirement() {
  Fixture x(Combine::SharePriority);
  x.f.objects.model[x.f.b.slot()] = 2;
  x.f.objects.model[x.third.slot()] = 3;
  set_plot(x.f.objects, x.f.a.slot(), 1);
  set_plot(x.f.objects, x.f.b.slot(), 2);
  set_plot(x.f.objects, x.third.slot(), 3);
  x.baseline(6);
  const auto change = [&](SettingKey key) {
    reconcile_priority(x.f.values(), x.f.react_state, key, x.f.effects, x.now, 20);
    x.publish();
  };
  x.f.instance_settings.columns.resize(13);
  x.f.instance_settings.columns[12].set(x.f.b.slot(), Value(4.0));
  change({SettingKey::Scope::Instance, x.f.b.bits, 12});
  near(get_share(x.f.allocations, x.subject, 11, x.f.b), 6, "instance priority reallocates");
  x.f.model_settings.rows[1][12] = Value(5.0);
  change({SettingKey::Scope::Model, 1, 12});
  near(get_share(x.f.allocations, x.subject, 11, x.f.a), 6, "model default is inherited");
  x.f.plots.rows[2][12] = Value(8.0);
  x.f.instance_settings.columns[12].erase(x.f.b.slot());
  change({SettingKey::Scope::Plot, 2, 12});
  near(get_share(x.f.allocations, x.subject, 11, x.f.b), 6, "plot default becomes effective");
  x.f.world.values[12] = Value(10.0);
  change({SettingKey::Scope::World, 0, 12});
  near(get_share(x.f.allocations, x.subject, 11, x.third), 1, "world default serves its demander");
  near(get_share(x.f.allocations, x.subject, 11, x.f.b), 5, "remaining supply follows priority");
  require(priority_inputs(x.f.values(), {SettingKey::Scope::Model, 99, 12}).empty(),
          "unrelated model scope selects no primary demand rows");
  x.now = 20;
  withdraw_contributor(x.f.shared, x.f.gives_to, x.f.terms, x.f.a, x.f.effects);
  retire_allocations(x.f.allocations, x.meters, x.f.a);
  retire_subscriptions(x.f.subscriptions, x.f.a);
  destroy(x.f.objects, x.f.a);
  x.publish();
  near(get_share(x.f.allocations, x.subject, 11, x.f.b), 5,
       "retired demander leaves only surviving primary contributions");
  require(watch_consumers(x.f.subscriptions, x.f.a).empty(), "retirement removes exact consumer");
  retire_allocations(x.f.allocations, x.meters, x.subject);
  require(x.f.allocations.values.empty() && x.meters.empty(),
          "subject retirement removes current allocations and lifetime meters");
}
void snapshot_answers_and_live_share() {
  Fixture x;
  x.baseline();
  x.contribute(x.f.a, 11, 1);
  x.contribute(x.f.b, 11, 11);
  x.publish();
  snap::Buffer bytes;
  snap::Output out{bytes};
  write_shared(out, x.f.shared, x.f.gives_to, x.f.allocations, x.meters);
  write_script_states(out, x.f.states, x.f.subscriptions);
  bytes.resize(out.adapter().writtenBytesCount());
  snap::Input in{bytes.data(), bytes.size()};
  read_shared(in, x.f.shared, x.f.gives_to, x.f.allocations, x.meters,
              x.f.objects.generation.size());
  read_script_states(in, x.f.states, x.f.subscriptions);
  snap::check_complete(in);
  require(
      check_changed(x.f.subscriptions, x.f.values(), x.f.allocations, {x.subject, 11}, 0, 20, {})
          .deliveries.empty(),
      "restored composite answers do not replay callbacks");
  const auto script = x.f.add(R"(return {kind='instance',contributes={'novel.wanted'},on={
    ['novel.cue']=function(self)
      local subject=self:related('novel.link','out')[1]
      self:contribute(subject,'novel.wanted',1)
      self.state.immediate=self:get_share(subject,'novel.wanted')
    end}})");
  const InvocationPlan call{{script, "novel.cue"}, x.f.a, {}, 6, {}, {}};
  x.f.run(std::span(&call, 1));
  near(std::get<double>(x.f.states.at({x.f.a, script}).values.at(std::string("immediate"))), 1,
       "get_share reads current rows before any reconciliation pass");
}
} // namespace
int main() {
  redistribution_and_lifetime_meters();
  thresholds_and_relation_rebinding();
  priority_scopes_and_retirement();
  snapshot_answers_and_live_share();
  std::cout << "primary-row allocation, actual Luau watches, priorities and restore passed\n";
}
