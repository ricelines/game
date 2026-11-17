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
      f.baseline(object);
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
void redistribution_and_supplier_replacement() {
  Fixture x;
  x.baseline();
  near(get_share(x.f.allocations, x.subject, 11, x.f.a), 3, "equal waterfill starts at three");
  x.contribute(x.f.a, 11, 1);
  x.contribute(x.f.b, 11, 11);
  near(read_share(x.f.values(), x.f.react_state, x.f.effects, {x.subject, 11}, x.f.a, 0, 20), 1,
       "reading after same-total redistribution sees the new allocation");
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
  near(get_share(x.f.allocations, x.subject, 11, x.f.b), 5,
       "replacement supply preserves the delivered shares");
  x.contribute(x.substitute, 10, 0);
  near(read_share(x.f.values(), x.f.react_state, x.f.effects, {x.subject, 11}, x.f.b, x.now, 20), 0,
       "reading after supply withdrawal sees zero before publication");
  x.publish();
  require(x.count(x.f.a) == 2 && x.count(x.f.b) == 2 && x.count(x.third) == 1,
          "loss of all supply wakes every previously served demander");
  near(get_share(x.f.allocations, x.subject, 11, x.f.b), 0,
       "withdrawn supply leaves no live allocation");
  x.contribute(x.supplier, 10, 7);
  x.publish();
  require(x.count(x.f.a) == 3 && x.count(x.f.b) == 3 && x.count(x.third) == 2,
          "restored supply wakes the demanders again");
  near(get_share(x.f.allocations, x.subject, 11, x.f.b), 5,
       "restored supplier serves the current demands");
}
void thresholds_and_relation_rebinding() {
  Fixture x;
  x.baseline();
  x.f.scripts.rows[x.consumer].declaration.watches[0].thresholds = {
      {Threshold::Predicate::Above, 1000}};
  for (auto h : {x.f.a, x.f.b, x.third})
    x.f.baseline(h);
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
  x.f.models.rows.resize(4);
  for (ModelId id = 1; id <= 3; ++id)
    x.f.models.rows[id].id = id;
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
  x.f.instance_settings.columns[12].set(x.f.a.slot(), Value(6.0));
  change({SettingKey::Scope::Instance, x.f.a.bits, 12});
  near(get_share(x.f.allocations, x.subject, 11, x.f.a), 6,
       "instance priority changes reallocate the contributor's circuit");
  x.f.instance_settings.columns[12].erase(x.f.a.slot());
  change({SettingKey::Scope::Instance, x.f.a.bits, 12});
  near(get_share(x.f.allocations, x.subject, 11, x.f.b), 6,
       "removing an instance setting restores inherited priority");
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
  require(priority_inputs(x.f.values(), x.f.gives_to, {SettingKey::Scope::Model, 99, 12}).empty(),
          "unrelated model scope selects no primary demand rows");
  x.now = 20;
  const std::array retired{x.f.a};
  retire_reactions(x.f.values(), x.f.react_state, retired, x.now, 20, {}, x.f.effects);
  destroy(x.f.objects, x.f.a);
  x.publish();
  near(get_share(x.f.allocations, x.subject, 11, x.f.b), 5,
       "retired demander leaves only surviving primary contributions");
  require(!has_watch(x.f.react_state, x.f.values(), x.f.scripts, {x.f.a, 11}, x.now, 20),
          "retired objects cannot consume quantity changes");
  const std::array retired_subject{x.subject};
  retire_reactions(x.f.values(), x.f.react_state, retired_subject, x.now, 20, {}, x.f.effects);
  require(x.f.allocations.values.empty(), "subject retirement removes current allocations");
}
void batch_retirement_reconciles_live_circuits() {
  Fixture x;
  x.baseline();
  x.contribute(x.supplier, 10, 9);
  x.contribute(x.supplier, 10, 5, x.substitute);
  x.contribute(x.f.a, 11, 2, x.substitute);
  x.contribute(x.f.b, 11, 4, x.substitute);
  x.publish();
  near(get_share(x.f.allocations, x.substitute, 11, x.f.b), 3,
       "second circuit divides supply before retirement");
  require(x.count(x.f.a) == 1 && x.count(x.f.b) == 1,
          "actual quantity handlers establish both objects' script state");
  const std::array retired{x.subject, x.f.a};
  retire_reactions(x.f.values(), x.f.react_state, retired, x.now, 20, {}, x.f.effects);
  require(identity(x.f.objects).valid(x.subject) && identity(x.f.objects).valid(x.f.a),
          "reaction retirement finishes while identities remain live");
  near(get_share(x.f.allocations, x.subject, 11, x.f.b), 0,
       "retired circuit loses all allocations");
  near(get_share(x.f.allocations, x.substitute, 11, x.f.a), 0,
       "retired demander loses its allocation on another circuit");
  near(get_share(x.f.allocations, x.substitute, 11, x.f.b), 4,
       "surviving circuit reallocates supply to its remaining demander");
  require(!x.f.states.contains({x.f.a, x.consumer}) && x.count(x.f.b) == 1,
          "batch retirement removes the retired script state and preserves the live state");
}
void snapshot_answers_and_immediate_share_reads() {
  Fixture x;
  x.baseline();
  x.contribute(x.f.a, 11, 1);
  x.contribute(x.f.b, 11, 11);
  x.publish();
  snap::Buffer bytes;
  snap::Output out{bytes};
  write_shared(out, x.f.shared, x.f.gives_to, x.f.allocations);
  write_script_states(out, x.f.states, x.f.watch_answers);
  bytes.resize(out.adapter().writtenBytesCount());
  snap::Input in{bytes.data(), bytes.size()};
  read_shared(in, x.f.shared, x.f.gives_to, x.f.allocations, x.f.objects.generation.size());
  read_script_states(in, x.f.states, x.f.watch_answers);
  snap::check_complete(in);
  require(evaluate_watch(x.f.react_state, x.f.values(), x.f.scripts, {{x.subject, 11}, {}}, 0, 20)
              .invocations.empty(),
          "restored composite answers do not replay callbacks");
  const auto script = x.f.add(R"(return {kind='instance',contributes={'novel.wanted'},on={
    ['novel.cue']=function(self)
      local subject=self:related('novel.link','out')[1]
      self:contribute(subject,'novel.wanted',6)
      self.state.first=self:get_share(subject,'novel.wanted')
      self.state.repeated=self:get_share(subject,'novel.wanted')
      self:contribute(subject,'novel.wanted',2)
      self.state.later=self:get_share(subject,'novel.wanted')
    end}})");
  const std::array installed{x.consumer, script};
  set_instances(x.f.installed, x.f.a, installed, x.f.scripts);
  project_instance(x.f.triggers, x.f.installed, x.f.scripts, x.f.terms, x.f.a);
  const auto handlers = dispatch(x.f.triggers, x.f.values(), x.f.a, 6, 0, 20);
  require(handlers.size() == 1, "the test event selects its installed contribution writer");
  PlannedInvocation call{handlers.front(), {}};
  call.input.receiver = x.f.a;
  call.input.trigger = 6;
  x.f.run(std::span(&call, 1));
  const auto &state = x.f.states.at({x.f.a, script}).values;
  near(std::get<double>(state.at(std::string("first"))), 3,
       "get_share sees the handler's actual changed demand");
  near(std::get<double>(state.at(std::string("repeated"))), 3,
       "a repeated read preserves the current allocation");
  near(std::get<double>(state.at(std::string("later"))), 2,
       "a later write in the same handler is visible to the next read");
  x.publish();
  require(x.count(x.f.a) == 2 && x.count(x.f.b) == 2 && x.count(x.third) == 1,
          "immediate reads preserve notifications for changed allocations and aggregate demand");
  for (auto object : {x.f.a, x.f.b, x.third})
    near(std::get<double>(x.f.states.at({object, x.consumer}).values.at(std::string("total"))), 14,
         "every consumer receives the changed aggregate demand");
  near(std::get<double>(x.f.states.at({x.third, x.consumer}).values.at(std::string("allocation"))),
       1, "aggregate demand changes also notify a consumer whose allocation stays fixed");
}
void reading_one_circuit_preserves_other_pending_changes() {
  Fixture x;
  x.baseline();
  x.contribute(x.supplier, 10, 4, x.substitute);
  x.contribute(x.f.a, 11, 4, x.substitute);
  x.contribute(x.f.a, 11, 1);
  near(read_share(x.f.values(), x.f.react_state, x.f.effects, {x.subject, 11}, x.f.a, 0, 20), 1,
       "the requested circuit reflects its changed demand");
  x.publish();
  near(get_share(x.f.allocations, x.substitute, 11, x.f.a), 4,
       "reading another circuit does not lose this circuit's pending contributions");
}
} // namespace
int main() {
  redistribution_and_supplier_replacement();
  thresholds_and_relation_rebinding();
  priority_scopes_and_retirement();
  batch_retirement_reconciles_live_circuits();
  snapshot_answers_and_immediate_share_reads();
  reading_one_circuit_preserves_other_pending_changes();
  std::cout << "primary-row allocation, actual Luau watches, priorities and restore passed\n";
}
