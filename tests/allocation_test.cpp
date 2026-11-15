#include "bindings/installation/installation.hpp"
#include "bindings/turns/turns.hpp"
#include "react/snapshot.hpp"
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
  test_runtime::Fixture f;
  GivesTo gives;
  MeterRows meters;
  PriorityLinks priorities;
  bindings::AllocationReconciliation allocation{f.allocations, meters, priorities};
  RelationBinding relations{f.relation_view, f.terms};
  ContributionBinding contributions{f.shared, gives, f.terms, f.relation_view};
  ShareBinding shares{f.allocations, f.terms};
  Handle subject = f.c, third, supplier, substitute;
  ScriptId consumer = 0, producer = 0, bare = 0;
  Relation declared;
  lifecycle::Parts parts;
  lifecycle::Instances lifetime{f.objects, parts, declared};
  bindings::Execution execution{f.vm, f.states, f.random, identity(f.objects), declared, 17};
  drive::StructuralOperations structural;
  drive::Clock clock;
  step::WorkQueues queues;
  step::TimerWheel timers;
  record::Journal journal{8, {}};
  std::vector<FactKey> mutated;
  std::vector<bindings::GapDraft> gaps;
  bindings::HandlerTurns handlers{execution,
                                  structural,
                                  f.triggers,
                                  *f.projection,
                                  {f.subscriptions, f.links, f.scripts, f.rules},
                                  journal,
                                  queues,
                                  timers,
                                  mutated,
                                  clock,
                                  20,
                                  1000,
                                  allocation};
  bindings::WatchTurns watches{handlers, gaps};
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
    f.terms.rows[12].source = Source::Setting;
    if (combine == Combine::SharePriority)
      f.terms.rows[11].priority = 12;
    ObjectInit init;
    init.model = 1;
    init.material = 8;
    third = create(f.objects, init);
    supplier = create(f.objects, init);
    substitute = create(f.objects, init);
    register_binding(f.vm, "related", related_binding, &relations);
    register_binding(f.vm, "contribute", contribute_binding, &contributions);
    register_binding(f.vm, "get_share", share_binding, &shares);
    consumer = f.add(R"(return {kind='instance',
      watches={{term='novel.wanted',along='novel.link'}},
      on={['novel.wanted']=function(self,subject,total)
        self.state.count=(self.state.count or 0)+1
        self.state.total=total
        self.state.allocation=if subject then self:get_share(subject,'novel.wanted') else 0
      end}})");
    producer = f.add(R"(return {kind='instance',contributes={'novel.available'},
      on={['novel.cue']=function(self,_,amount)
        self:contribute(self:related('novel.link','out')[1],'novel.available',amount)
      end}})");
    bare = f.add(R"(return {kind='instance',on={
      ['novel.wanted']=function()error('unwatched handler executed')end}})");
    for (auto object : {f.a, f.b, third, supplier})
      set_relation(f.relations.at(3), 3, object, subject, true, f.effects);
    for (auto object : {f.a, f.b, third})
      install_script(object, consumer);
    install_script(supplier, producer);
    install_script(subject, bare);
    rebuild_priorities(priorities, *f.projection);
    f.effects = {};
  }
  void install_script(Handle h, ScriptId script) {
    install::set_instances(f.installed, h, std::span{&script, 1}, f.scripts);
    install::project_instance(f.triggers, f.installed, f.scripts, f.terms, h);
  }
  void contribute(Handle from, TermId term, double amount, Handle target = {}) {
    set_contribution(f.shared, gives, f.terms.rows[term], from, target ? target : subject, amount,
                     f.effects);
  }
  void baseline(double available = 7) {
    contribute(supplier, 10, available);
    contribute(f.a, 11, 6);
    contribute(f.b, 11, 6);
    contribute(third, 11, 1);
    bindings::reconcile_allocations(*f.projection, allocation, f.effects, 0, 20);
    for (auto object : {f.a, f.b, third})
      f.bind(consumer, object);
    f.effects = {};
  }
  void drain() {
    drive::Frame frame;
    frame.handler_deadline = drive::Monotonic::now() + std::chrono::seconds(1);
    drive::drain_foreground(queues, frame, {&handlers, bindings::consume_event},
                            {&watches, bindings::consume_watch}, {}, {});
  }
  void publish() {
    bindings::reconcile_allocations(*f.projection, allocation, f.effects, clock.step, 20);
    publish_effects(std::move(f.effects), {}, clock.step, queues, timers, journal, 1000);
    f.effects = {};
    drain();
  }
  double count(Handle object) const {
    auto state = f.states.find({object, consumer});
    if (state == f.states.end())
      return 0;
    auto value = state->second.find(std::string("count"));
    return value == state->second.end() ? 0 : std::get<double>(value->second);
  }
  void retire(Handle object) {
    bindings::RetireReactions retirement{
        f.models, f.scripts,    *f.projection, f.installed,         f.triggers, f.subscriptions,
        f.links,  f.properties, f.processes,   f.instance_settings, f.states,   f.random,
        f.shared, gives,        f.relations,   allocation};
    const auto objects = lifetime.closure(std::span{&object, 1});
    bindings::InvocationInput input;
    input.step = clock.step;
    input.hz = 20;
    bindings::retire_reactions(retirement, objects, input, f.effects);
    lifetime.release_rows(objects);
    require(!identity(f.objects).valid(object), "actual reaction cleanup and row release complete");
    publish();
  }
};
void redistribution_is_consumer_specific() {
  Fixture x;
  x.baseline();
  near(get_share(x.f.allocations, x.subject, 11, x.f.a), 3, "equal waterfill starts at three");
  x.contribute(x.f.a, 11, 1);
  x.contribute(x.f.b, 11, 11);
  x.publish();
  require(x.count(x.f.a) == 1 && x.count(x.f.b) == 1 && x.count(x.third) == 0,
          "same-total redistribution wakes affected demanders only");
  near(std::get<double>(x.f.states.at({x.f.a, x.consumer}).at(std::string("total"))), 13,
       "watch payload remains the total demand");
  near(std::get<double>(x.f.states.at({x.f.b, x.consumer}).at(std::string("allocation"))), 5,
       "real Luau reads its reconciled allocation");
  require(!x.f.states.contains({x.subject, x.bare}), "bare quantity handlers never run");
  x.clock.step = 20;
  x.contribute(x.supplier, 10, 0);
  x.contribute(x.substitute, 10, 7);
  x.publish();
  require(x.count(x.f.a) == 1 && x.count(x.f.b) == 1 && x.count(x.third) == 0,
          "same-total supplier replacement does not wake unchanged demanders");
  near(metered(x.meters.at({x.subject, x.supplier, 10, true}), 40, 20), 7,
       "withdrawn supplier meter retains delivered integral and stops");
  near(metered(x.meters.at({x.subject, x.substitute, 10, true}), 40, 20), 7,
       "replacement supplier starts at actual replacement time");
  x.clock.step = 40;
  x.contribute(x.substitute, 10, 0);
  x.contribute(x.supplier, 10, 7);
  x.publish();
  near(metered(x.meters.at({x.subject, x.supplier, 10, true}), 60, 20), 14,
       "rejoining continues the prior delivered integral");
  x.clock.step = 60;
  x.contribute(x.f.a, 11, 0);
  x.contribute(x.f.b, 11, 12);
  x.publish();
  near(metered(x.meters.at({x.subject, x.f.a, 11, false}), 80, 20), 3,
       "withdrawn live demander retains its integral at zero rate");
  x.clock.step = 80;
  x.contribute(x.f.a, 11, 1);
  x.contribute(x.f.b, 11, 11);
  x.publish();
  near(metered(x.meters.at({x.subject, x.f.a, 11, false}), 100, 20), 4,
       "rejoining demander resumes the same lifetime integral");
}
void actual_handler_reconciles_before_causal_watch() {
  Fixture x;
  x.baseline();
  step::enqueue_event(x.queues, {x.supplier, {}, 6, Value(13.0), {}}, 1000);
  x.drain();
  require(x.count(x.f.a) == 1 && x.count(x.f.b) == 1 && x.count(x.third) == 0,
          "real contribution handler publishes allocation changes before dependent handlers");
  near(get_share(x.f.allocations, x.subject, 11, x.f.a), 6, "available surplus satisfies demand");
  const auto events = x.journal.take();
  require(events.size() >= 3 && events[1]["cause"] == events[0]["id"],
          "dependent handlers inherit the completed producer cause");
}
void thresholds_and_relation_rebinding_preserve_answer_semantics() {
  Fixture x;
  x.baseline();
  auto &declaration = x.f.scripts.rows[x.consumer].declaration;
  declaration.watches[0].thresholds = {{Threshold::Predicate::Above, 1000}};
  for (auto h : {x.f.a, x.f.b, x.third})
    x.f.bind(x.consumer, h);
  x.contribute(x.f.a, 11, 8);
  x.publish();
  require(x.count(x.f.a) == 0 && x.count(x.f.b) == 0,
          "unchanged threshold predicates suppress aggregate-only demand changes");
  x.contribute(x.f.a, 11, 1);
  x.contribute(x.f.b, 11, 13);
  x.publish();
  require(x.count(x.f.a) == 1 && x.count(x.f.b) == 1 && x.count(x.third) == 0,
          "own allocation changes remain observable independently of aggregate thresholds");
  ObjectInit init;
  init.model = 1;
  auto other = create(x.f.objects, init);
  x.contribute(x.supplier, 10, 0.5, other);
  x.contribute(x.f.a, 11, 15, other);
  set_relation(x.f.relations.at(3), 3, x.f.a, x.subject, false, x.f.effects);
  set_relation(x.f.relations.at(3), 3, x.f.a, other, true, x.f.effects);
  x.publish();
  require(x.count(x.f.a) == 2 && x.count(x.f.b) == 1,
          "along rebind compares the old own allocation with the new subject's allocation");
  near(std::get<double>(x.f.states.at({x.f.a, x.consumer}).at(std::string("allocation"))), 0.5,
       "rebound Luau consumer sees the actual new subject");
}
void priority_scopes_are_indexed_and_inherited() {
  Fixture x(Combine::SharePriority);
  x.f.models.rows.resize(4);
  x.f.objects.model[x.f.b.slot()] = 2;
  x.f.objects.model[x.third.slot()] = 3;
  x.f.objects.plot[x.f.a.slot()] = 1;
  x.f.objects.plot[x.f.b.slot()] = 2;
  x.f.objects.plot[x.third.slot()] = 3;
  x.baseline(6);
  auto change = [&](SettingKey key) {
    bindings::reconcile_priority(*x.f.projection, x.allocation, key, x.f.effects, x.clock.step, 20);
    x.publish();
  };
  x.f.instance_settings.columns.resize(13);
  x.f.instance_settings.columns[12].set(x.f.b.slot(), Value(4.0));
  change({SettingKey::Scope::Instance, x.f.b.bits, 12});
  near(get_share(x.f.allocations, x.subject, 11, x.f.b), 6, "instance priority changes allocation");
  x.f.model_settings.rows[1][12] = Value(5.0);
  change({SettingKey::Scope::Model, 1, 12});
  near(get_share(x.f.allocations, x.subject, 11, x.f.a), 6, "model default priority is inherited");
  x.f.plots.rows[2][12] = Value(8.0);
  x.f.instance_settings.columns[12].erase(x.f.b.slot());
  change({SettingKey::Scope::Plot, 2, 12});
  near(get_share(x.f.allocations, x.subject, 11, x.f.b), 6, "plot default becomes effective");
  x.f.world.values[12] = Value(10.0);
  change({SettingKey::Scope::World, 0, 12});
  near(get_share(x.f.allocations, x.subject, 11, x.third), 1, "world default serves its demander");
  near(get_share(x.f.allocations, x.subject, 11, x.f.b), 5,
       "remaining supply goes to next priority");
  const auto before = x.count(x.f.a) + x.count(x.f.b) + x.count(x.third);
  change({SettingKey::Scope::Model, 99, 12});
  require(before == x.count(x.f.a) + x.count(x.f.b) + x.count(x.third),
          "unrelated setting scopes do no allocation work");
  x.f.model_settings.rows[3][12] = Value(20.0);
  x.f.objects.model[x.f.a.slot()] = 3;
  bindings::reconcile_object_priorities(*x.f.projection, x.allocation, x.f.a, x.f.effects,
                                        x.clock.step, 20);
  x.publish();
  near(get_share(x.f.allocations, x.subject, 11, x.f.a), 6,
       "object model rebinding refreshes inherited priority and scope links");
  require(priority_inputs(x.priorities, {SettingKey::Scope::Model, 1, 12}).empty(),
          "rebinding removes the obsolete model default dependency");
}
void rebind_retirement_and_saved_answers() {
  Fixture x(Combine::SharePriority);
  x.baseline();
  x.clock.step = 20;
  x.retire(x.f.a);
  require(!x.f.allocations.by_object.contains(x.f.a.bits), "retired demander has no incident rows");
  near(get_share(x.f.allocations, x.subject, 11, x.f.b), 6,
       "retiring a demander immediately redistributes surviving supply");
  require(!x.priorities.by_setting.contains({SettingKey::Scope::Instance, x.f.a.bits, 12}),
          "retired demander leaves no priority dependency");
  x.retire(x.subject);
  require(x.f.allocations.values.empty() && x.meters.empty() && x.priorities.by_pair.empty(),
          "retired subject clears allocations, meters and priority dependencies");
  require(!x.gives.get(x.f.b.slot()) || x.gives.get(x.f.b.slot())->empty(),
          "subject retirement clears surviving contributor reverse references");
  require(x.count(x.f.b) == 2, "surviving along consumer receives one transition to absence");
  ObjectInit init;
  init.model = 1;
  auto replacement = create(x.f.objects, init);
  require(replacement != x.subject && get_share(x.f.allocations, replacement, 11, x.f.b) == 0,
          "reused generation inherits no old allocation");

  Fixture restored;
  restored.baseline();
  restored.contribute(restored.f.a, 11, 1);
  restored.contribute(restored.f.b, 11, 11);
  restored.publish();
  snap::Buffer bytes;
  snap::Output out{bytes};
  write_shared(out, restored.f.shared, restored.gives, restored.f.allocations, restored.meters);
  write_script_states(out, restored.f.states, restored.f.subscriptions);
  out.adapter().flush();
  bytes.resize(out.adapter().writtenBytesCount());
  const auto expected = restored.f.subscriptions.rows;
  snap::Input in{bytes.data(), bytes.size()};
  read_shared(in, restored.f.shared, restored.gives, restored.f.allocations, restored.meters,
              restored.f.objects.generation.size());
  read_script_states(in, restored.f.states, restored.f.subscriptions);
  snap::check_complete(in);
  rebuild_priorities(restored.priorities, *restored.f.projection);
  auto quiet = install::check_changed(restored.f.subscriptions, *restored.f.projection,
                                      restored.f.allocations, {restored.subject, 11}, 20, 20, {});
  require(quiet.deliveries.empty() && restored.f.allocations.by_object.contains(restored.f.a.bits),
          "saved composite answers and allocation incident indexes restore without replay");
  require(std::holds_alternative<SharedWatchAnswer>(expected.at({restored.subject, 11})[0].last),
          "snapshot fixture contains actual composite saved answers");
}
void installation_modes_keep_saved_answers_and_flat_state() {
  Fixture x;
  x.baseline();
  auto old = x.f.add(R"(return {kind='model',
    watches={{term='novel.heat',thresholds={{predicate='above',value=1}}}},
    on={['novel.heat']=function(self)self.state.called=true end}})");
  x.f.models.rows[1].script = old;
  install::project_model(x.f.triggers, x.f.models, x.f.scripts, x.f.terms, 1);
  set_rate(x.f.properties, x.f.processes, x.f.terms.rows[1], x.f.a, 1, 0, 10, 0, 20, x.f.effects);
  x.f.effects = {};
  bindings::InstallReactions context{x.f.models,        x.f.scripts,     x.f.installed,
                                     x.f.rules,         *x.f.projection, x.f.triggers,
                                     x.f.subscriptions, x.f.links,       x.f.allocations};
  bindings::InvocationInput input;
  input.hz = 20;
  const auto objects = std::span{&x.f.a, 1};
  bindings::install_reactions(context, objects, input, x.f.effects);
  require(std::count_if(x.f.effects.ordered.begin(), x.f.effects.ordered.end(),
                        [](const auto &row) { return std::holds_alternative<step::Timer>(row); }) ==
              1,
          "initial adapter installs and arms the real process threshold");
  const auto revision = x.f.subscriptions.next_revision;
  x.f.effects = {};
  bindings::restore_reactions(context, objects, input, x.f.effects);
  require(x.f.effects.ordered.empty() && x.f.subscriptions.next_revision == revision,
          "restore adapter rehydrates saved watch links without replay or duplicate timers");
  auto next = x.f.add(R"(return {kind='model',watches={'novel.heat'},
    on={['novel.heat']=function(self)self.state.count=(self.state.count or 0)+1 end}})");
  x.f.states[{x.f.a, old}][std::string("remember")] = 42.0;
  x.f.models.rows.resize(3);
  x.f.models.rows[2].id = 2;
  x.f.models.rows[2].script = next;
  x.f.objects.model[x.f.a.slot()] = 2;
  install::project_model(x.f.triggers, x.f.models, x.f.scripts, x.f.terms, 2);
  lifecycle::Rebinding changed;
  changed.object = x.f.a;
  changed.before = {1, 8, 0};
  changed.after = {2, 8, 0};
  bindings::rebind_reactions(context, std::span{&changed, 1}, input, x.f.effects);
  require(!x.f.subscriptions.by_consumer.contains({x.f.a, old}) &&
              x.f.subscriptions.by_consumer.contains({x.f.a, next}) &&
              std::get<double>(x.f.states.at({x.f.a, old}).at(std::string("remember"))) == 42,
          "binding change removes obsolete default watches while preserving persistent flat state");
  x.publish();
  near(std::get<double>(x.f.states.at({x.f.a, next}).at(std::string("count"))), 1,
       "replacement default receives its current watched answer once");
}
} // namespace
int main() {
  redistribution_is_consumer_specific();
  actual_handler_reconciles_before_causal_watch();
  thresholds_and_relation_rebinding_preserve_answer_semantics();
  priority_scopes_are_indexed_and_inherited();
  rebind_retirement_and_saved_answers();
  installation_modes_keep_saved_answers_and_flat_state();
  std::cout << "allocation integration passed\n";
}
