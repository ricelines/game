#include "bindings/execution/execution.hpp"

#include <iostream>
#include <stdexcept>

using namespace amber;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct Fixture {
  Objects objects;
  Handle object, other;
  react::Terms terms;
  react::Properties properties;
  react::Processes processes;
  react::Materials materials;
  react::AssetMeasurements assets;
  react::InstanceSettings settings;
  react::ModelSettings models;
  react::PlotSettings plots;
  react::WorldSettings world;
  react::SharedColumns shared;
  react::RelationsRead relations;
  react::RuntimeMeasurements runtime;
  react::ScriptStates states;
  react::RandomStates random;
  Relation parts;
  record::Journal journal{8, {}};
  react::Vm vm{{4096, 50, 5, 64}};
  react::ValueRead values;
  react::ReadBinding read;
  react::StateBinding write;
  bindings::Execution execution;
  Fixture()
      : values{terms,        identity(objects),
               objects.plot, objects.params,
               materials,    assets,
               properties,   processes,
               settings,     models,
               plots,        world,
               shared,       relations,
               runtime,      0},
        read{values}, write{properties, processes, terms},
        execution{vm, states, random, identity(objects), parts, 971} {
    ObjectInit init;
    init.model = 1;
    object = create(objects, init);
    other = create(objects, init);
    terms.rows.resize(5);
    for (TermId i = 1; i < terms.rows.size(); ++i) {
      terms.rows[i].id = i;
      terms.rows[i].name = "unforeseen." + std::to_string(i);
      terms.by_name[terms.rows[i].name] = i;
    }
    terms.rows[1].kind = react::Kind::Action;
    terms.rows[2].kind = react::Kind::Event;
    terms.rows[3].source = react::Source::State;
    terms.rows[4].kind = react::Kind::Event;
    react::register_binding(vm, "get", react::get_binding, &read);
    react::register_binding(vm, "set", react::set_binding, &write);
    react::bind_events(vm, terms);
    load(1, R"(return {kind='model', sets={'unforeseen.3'},
      on={['unforeseen.1']=function(self,from,args)
        self:set('unforeseen.3',1)
        self:emit(self.handle,'unforeseen.2',{amount=1})
      end}})");
    load(2, R"(return {kind='instance', sets={'unforeseen.3'},
      on={['unforeseen.1']=function(self,from,args)
        assert(self:get('unforeseen.3')==1)
        self:set('unforeseen.3',11)
      end,
      ['unforeseen.4']=function(self)
        self.state.last=self:random()
        self.state.count=(self.state.count or 0)+1
      end}})");
    load(3, R"(return {kind='instance', sets={'unforeseen.3'},
      on={['unforeseen.1']=function(self)
        self:set('unforeseen.3',111)
        self:emit(self.handle,'unforeseen.2',111)
        error('deliberate partial failure')
      end}})");
  }
  void load(ScriptId id, std::string_view source) {
    auto analyzed = react::analyze_script(source, terms, {}, {4096, 50, 5, 64});
    require(analyzed.errors.empty(), "real source analysis");
    analyzed.declaration.id = id;
    analyzed.declaration.author = id + 10;
    const auto loaded = react::load_script(vm, analyzed.compiled, analyzed.declaration);
    require(loaded.status == react::RunOutcome::Status::Ok, loaded.error.c_str());
  }
  bindings::CompletedInvocation run(ScriptId script, TermId term = 1, Handle receiver = {},
                                    const record::Origin &origin = {}) {
    bindings::InvocationInput input;
    input.receiver = receiver ? receiver : object;
    input.sender = other;
    input.trigger = term;
    input.step = 12;
    input.origin = origin;
    input.action_caller = PlayerId{7};
    react::Invocation invocation;
    bindings::CompletedInvocation completed;
    bindings::start_invocation(execution, journal,
                               {script, terms.rows[term].name, script + 10, 1, true},
                               std::move(input), invocation, completed);
    bindings::finish_invocation(execution, journal, invocation, completed);
    return completed;
  }
};
void live_effects_and_causal_finalization() {
  Fixture f;
  auto &journal = f.journal;
  step::WorkQueues queues;
  step::TimerWheel timers;
  record::Event action;
  action.step = 12;
  action.kind = record::Kind::Action;
  action.actor = record::Actor::player(7);
  action.name = "unforeseen.1";
  const auto origin = journal.append(std::move(action));
  auto first = f.run(1, 1, {}, origin);
  auto second = f.run(2, 1, {}, origin);
  auto failed = f.run(3, 1, {}, origin);
  require(first.outcome.status == react::RunOutcome::Status::Ok &&
              second.outcome.status == react::RunOutcome::Status::Ok &&
              failed.outcome.status == react::RunOutcome::Status::Error &&
              react::read_state(f.properties, f.processes, f.object, 3, 12, 20).value.numeric() ==
                  111,
          "later handlers see prior writes and error preserves completed state");
  journal.finish(origin, record::Result::Applied, true, {});
  bindings::publish_invocation(journal, queues, timers, std::move(first), origin, 1000);
  bindings::publish_invocation(journal, queues, timers, std::move(second), origin, 1000);
  bindings::publish_invocation(journal, queues, timers, std::move(failed), origin, 1000);
  const auto logs = journal.take();
  require(logs.size() == 5 && logs[0]["kind"] == "action" && logs[0]["changed"] == true &&
              logs[1]["kind"] == "handler" && logs[1]["cause"] == logs[0]["id"] &&
              logs[2]["kind"] == "handler" && logs[3]["kind"] == "handler" &&
              logs[4]["kind"] == "system" && logs[4]["cause"] == logs[3]["id"],
          "action precedes completed handlers and their diagnostics retain direct cause");
  auto watch = step::pop_foreground(queues);
  require(watch && std::holds_alternative<step::WatchWork>(watch->work),
          "changed fact enters foreground");
  auto event = step::pop_foreground(queues);
  require(event && std::get<step::EventWork>(event->work).origin.cause == logs[1]["id"],
          "emitted event uses completed first-handler origin");
  auto failed_event = step::pop_foreground(queues);
  require(failed_event && std::holds_alternative<step::EventWork>(failed_event->work) &&
              queues.causal.empty(),
          "partial-error emissions are retained without a deferred action lane");
}
void persistence_and_activation() {
  Fixture f;
  const auto first = f.run(2, 4);
  require(first.outcome.status == react::RunOutcome::Status::Ok, "first RNG handler");
  const auto draw = std::get<double>(f.states.at({f.object, 2}).at(std::string("last")));
  f.run(2, 4);
  require(std::get<double>(f.states.at({f.object, 2}).at(std::string("count"))) == 2 &&
              std::get<double>(f.states.at({f.object, 2}).at(std::string("last"))) != draw,
          "scalar state and RNG continue across invocations");
  f.run(2, 4, f.other);
  require(std::get<double>(f.states.at({f.other, 2}).at(std::string("count"))) == 1,
          "another installation has its own persisted scalar state");
  react::set_activation(f.vm, 2, false, false);
  const auto disabled = f.run(2, 4);
  require(disabled.outcome.status == react::RunOutcome::Status::Missing &&
              std::get<double>(f.states.at({f.object, 2}).at(std::string("count"))) == 2,
          "inactive cached script cannot execute even with an old handler reference");
}
} // namespace
int main() {
  try {
    live_effects_and_causal_finalization();
    persistence_and_activation();
    std::cout << "Actual VM execution, live effects and causal finalization tests passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
