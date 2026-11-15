#include "drive/drive.hpp"
#include "runtime_fixture.hpp"

#include <iostream>
#include <stdexcept>

using namespace amber;
using namespace amber::react;
using namespace amber::install;
using test_runtime::Fixture;
using test_runtime::require;
namespace {
void clock_budget_and_background_progress() {
  drive::PhaseCosts costs(20);
  for (int i = 0; i < 21; ++i)
    costs.observe(i);
  require(costs.mean_ms() == 10.5, "phase average retains the latest twenty samples");
  drive::Clock clock;
  step::WorkQueues queues;
  step::TimerWheel timers;
  auto now = drive::Monotonic::now();
  auto frame = drive::begin_frame(clock, {20, 5, 20}, costs, queues, timers, {}, now);
  const auto allowance = std::chrono::duration<double, std::milli>(frame.handler_deadline - now);
  require(frame.step == 1 && std::abs(allowance.count() - 34.5) < .001,
          "handler budget subtracts average other phases and margin");
  auto late = frame.boundary + std::chrono::milliseconds(7);
  require(drive::finish_frame(clock, costs, frame, 4, late) == late && clock.overruns == 1,
          "overrun advances exactly one simulation quantum without catch-up burst");
  frame.handler_deadline = now - std::chrono::milliseconds(1);
  step::enqueue_background(queues, {});
  step::enqueue_background(queues, {});
  step::append_inputs(queues, {step::InputWork{9}});
  unsigned completed = 0;
  drive::BackgroundConsumer background{
      &completed, [](void *p, step::BackgroundItem) { ++*static_cast<unsigned *>(p); }};
  drive::drain_background(queues, frame, background, {}, {}, {}, {});
  require(completed == 1 && queues.background.size() == 1 && queues.turns.size() == 1,
          "one background item progresses after budget exhaustion without admitting inputs");
}

void copied_inputs_and_restore() {
  drive::Inputs inputs;
  drive::InputOperations operations;
  unsigned calls = 0;
  drive::register_input(operations, "author",
                        {[](void *p, const drive::Input &input) {
                           ++*static_cast<unsigned *>(p);
                           auto origin = input.origin;
                           origin.cause = 99;
                           return drive::InputReply{input.fields.value("async", false),
                                                    {{"ok", true}, {"sequence", input.sequence}},
                                                    origin};
                         },
                         &calls});
  drive::Input one;
  one.operation = "author";
  one.caller = ScriptHolder{4, Handle{123}};
  one.session = 17;
  one.sequence = 8;
  one.request = 23;
  one.fields = {{"async", true}};
  one.origin = {7, 5, {2, 3}, 1.5, false};
  auto first = drive::enqueue_input(inputs, one);
  require(!drive::invoke_input(inputs, operations, first) && calls == 1,
          "asynchronous authoring retains only a copied pending descriptor");
  require(!drive::invoke_input(inputs, operations, first) && calls == 1,
          "pending work is never started twice");
  auto completed_origin = one.origin;
  completed_origin.cause = 99;
  require(inputs.rows.at(first.id).origin == completed_origin && one.origin.cause == 7,
          "pending input retains the completed action origin without mutating its copied input");
  auto two = one;
  two.session = 18;
  two.fields = {{"async", false}};
  auto second = drive::enqueue_input(inputs, two);
  require(drive::invoke_input(inputs, operations, second)->session == 18 && calls == 2,
          "one pending session does not stop another session's work");
  snap::Buffer bytes;
  snap::Output out{bytes};
  drive::write_inputs(out, inputs);
  out.adapter().flush();
  drive::Inputs restored;
  snap::Input in{bytes.data(), bytes.size()};
  drive::read_inputs(in, restored);
  const auto &row = restored.rows.at(first.id);
  require(row.awaiting && row.origin == completed_origin && row.caller == one.caller &&
              row.fields == one.fields && restored.next_id == inputs.next_id,
          "pending input codec preserves typed authority, provenance and identity high water");
  auto reply = drive::complete_input(restored, first.id, {{"ok", true}, {"model", 42}});
  require(reply.session == 17 && reply.sequence == 8 && reply.request == 23 &&
              reply.fields["model"] == 42 && restored.rows.empty(),
          "completion returns to its exact original request without retaining live context");
}

struct Runner {
  Fixture f;
  Relation parts;
  bindings::Execution execution{f.vm, f.states, f.random, identity(f.objects), parts, 17};
  drive::StructuralOperations structural;
  step::WorkQueues queues;
  step::TimerWheel timers;
  record::Journal journal{8, {}};
  command::NativeActions native;
  std::vector<std::string> trace;
  Step now = 1;
  void action(step::ActionWork work) {
    trace.push_back("action");
    auto resolution = command::resolve(native, f.terms, f.models, f.scripts, f.installed,
                                       identity(f.objects), work.request, f.b);
    std::vector<bindings::CompletedInvocation> calls;
    for (const auto &plan : drive::plan_action(f.triggers, f.values(), resolution, work.request,
                                               f.b, {}, work.origin, now, 20)) {
      auto completed = drive::execute(execution, plan, structural);
      require(completed.invocation.outcome.status == RunOutcome::Status::Ok,
              completed.invocation.outcome.error.c_str());
      calls.push_back(std::move(completed.invocation));
    }
    record::Event event;
    event.step = now;
    event.kind = record::Kind::Action;
    event.actor = record::Actor::player(1);
    event.name = "novel.touch";
    bindings::publish_action(journal, queues, timers, std::move(event), std::move(calls),
                             work.origin, {}, 1000);
  }
  void event(drive::EventTurn turn) {
    require(drive::valid_event(turn, identity(f.objects), f.processes, f.subscriptions),
            "live event passes generation validation");
    trace.push_back("event");
    for (const auto &plan : drive::plan_event(f.triggers, f.values(), turn.event, now, 20)) {
      auto completed = drive::execute(execution, plan, structural);
      require(completed.invocation.outcome.status == RunOutcome::Status::Ok,
              completed.invocation.outcome.error.c_str());
      bindings::publish_invocation(journal, queues, timers, std::move(completed.invocation),
                                   turn.event.origin, 1000);
    }
  }
  void watch(step::WatchWork work) {
    trace.push_back("watch");
    auto plan = drive::plan_watch(f.subscriptions, f.links, f.values(), f.allocations, f.triggers,
                                  f.scripts, work, now, 20);
    for (const auto &dirty : plan.changes.dirty)
      step::enqueue_watch(queues, dirty);
    for (const auto &timer : plan.changes.timers)
      step::schedule(timers, timer);
    for (const auto &invocation : plan.invocations) {
      auto completed = drive::execute(execution, invocation, structural);
      bindings::publish_invocation(journal, queues, timers, std::move(completed.invocation),
                                   work.origin, 1000);
    }
  }
  void drain() {
    drive::Frame frame;
    frame.handler_deadline = drive::Monotonic::now() + std::chrono::seconds(2);
    drive::drain_foreground(
        queues, frame,
        {this,
         [](void *p, drive::EventTurn turn) { static_cast<Runner *>(p)->event(std::move(turn)); }},
        {this,
         [](void *p, step::WatchWork work) { static_cast<Runner *>(p)->watch(std::move(work)); }},
        {this,
         [](void *p, step::ActionWork work) { static_cast<Runner *>(p)->action(std::move(work)); }},
        {});
  }
};

void real_vm_causal_order_and_explicit_watch_modeling() {
  Runner runner;
  auto &f = runner.f;
  auto script = f.add(R"(return {kind='model',actions={['novel.touch']={right='use'}},
    on={['novel.touch']=function(self)
      self.state.actions=(self.state.actions or 0)+1
      if self.state.actions==1 then self:emit(self.handle,'novel.cue',true)
      else assert(self.state.cued);self:set('novel.result',17) end
    end,['novel.cue']=function(self)self.state.cued=true end,
    ['novel.signal']=function(self)error('an unwatched quantity must not run')end}})");
  f.models.rows[1].script = script;
  project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  ActionRequest request;
  request.caller = PlayerId{1};
  request.action = 7;
  request.target = f.a;
  step::append_inputs(runner.queues,
                      {step::ActionWork{request, {}}, step::ActionWork{request, {}}});
  runner.drain();
  require(runner.trace.size() >= 3 && runner.trace[0] == "action" && runner.trace[1] == "event" &&
              runner.trace[2] == "action" && read(f.values(), f.b, 2, 1, 20).value.numeric() == 17,
          "actual first action's emitted Luau handler runs before the second external action");
  const auto logs = runner.journal.take();
  require(logs[0]["kind"] == "action" && logs[1]["kind"] == "handler" &&
              logs[2]["kind"] == "handler" && logs[2]["cause"] == logs[1]["id"],
          "causal event retains completed parent-handler origin");
  f.shared.resize(5);
  f.shared[4].set(f.a.slot(), Shared{0, {}});
  auto gap = drive::plan_watch(f.subscriptions, f.links, f.values(), f.allocations, f.triggers,
                               f.scripts, {{f.a, 4}, {}}, 1, 20);
  require(!gap.modeled && gap.shared_gap == FactKey{f.a, 4} && gap.invocations.empty(),
          "bare quantity handler never consumes a shared gap or gains an implicit watch");
  auto watcher = f.add(R"(return {kind='instance',watches={'novel.signal'},
    on={['novel.signal']=function(self)self.state.woke=true end}})");
  const std::array<ScriptId, 1> ids{watcher};
  set_instances(f.installed, f.a, ids, f.scripts);
  project_instance(f.triggers, f.installed, f.scripts, f.terms, f.a);
  f.bind(watcher, f.a);
  auto unchanged = drive::plan_watch(f.subscriptions, f.links, f.values(), f.allocations,
                                     f.triggers, f.scripts, {{f.a, 4}, {}}, 1, 20);
  require(unchanged.modeled && !unchanged.shared_gap && unchanged.invocations.empty(),
          "applicable explicit watch models even an unchanged zero answer");
  f.shared[4].set(f.a.slot(), Shared{1, {}});
  runner.watch({{f.a, 4}, {}});
  require(std::get<bool>(f.states.at({f.a, watcher}).at(std::string("woke"))),
          "changed shared fact passes driver planning through the actual subscribed Luau handler");
}

struct Audit {
  Handle self, sender, body, target;
  bool physical = false;
};
int audit(lua_State *state) {
  const auto &inv = invocation(state);
  static_cast<std::vector<Audit> *>(binding_data(state))
      ->push_back({inv.self.object, inv.sender, inv.action_body, inv.action_target,
                   inv.direct_body.has_value()});
  return 0;
}
void body_scope_flags_and_reach() {
  Runner runner;
  auto &f = runner.f;
  std::vector<Audit> audits;
  register_binding(f.vm, "audit", audit, &audits);
  auto body = f.add(R"(return {kind='model',actions={['novel.touch']={right='use',
    right_on='selected',reach=true,mechanics={'grasp'}}},
    on={['novel.touch']=function(self)self:audit()end}})");
  auto sibling = f.add(R"(return {kind='instance',
    on={['novel.touch']=function(self)self:audit()end}})");
  f.models.rows[1].script = body;
  project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  const std::array<ScriptId, 1> ids{sibling};
  set_instances(f.installed, f.b, ids, f.scripts);
  project_instance(f.triggers, f.installed, f.scripts, f.terms, f.b);
  ActionRequest request{PlayerId{1}, 7, f.a, {}};
  auto resolution = command::resolve({}, f.terms, f.models, f.scripts, f.installed,
                                     identity(f.objects), request, f.b);
  auto plans = drive::plan_action(f.triggers, f.values(), resolution, request, f.b, f.c, {}, 1, 20);
  require(plans.size() == 2 && plans[0].input.direct_body && !plans[1].input.direct_body,
          "only the resolved body's declaration receives mechanical authority");
  for (const auto &plan : plans)
    drive::execute(runner.execution, plan, {});
  require(audits.size() == 2 && audits[0].self == f.b && audits[0].sender == f.a &&
              audits[0].body == f.b && audits[0].target == f.a && audits[0].physical &&
              !audits[1].physical,
          "actual VM body invocation stays installed on the body and passes selected as sender");
  request.caller = ScriptHolder{sibling, f.c};
  resolution = command::resolve({}, f.terms, f.models, f.scripts, f.installed, identity(f.objects),
                                request, f.b);
  plans = drive::plan_action(f.triggers, f.values(), resolution, request, f.b, {}, {}, 1, 20);
  require(plans.size() == 1 && !plans[0].input.action_body && !plans[0].input.direct_body &&
              plans[0].input.sender == f.c,
          "script-generated action cannot borrow an authenticated player body");
  require(drive::in_reach({99, 0, 1}, {{-100, -1, 0}, {100, 1, 3}}, 1) &&
              !drive::in_reach({99, 4, 1}, {{-100, -1, 0}, {100, 1, 3}}, 1),
          "reach uses nearest measured bounds rather than distant object center");
  auto rule = f.add(R"(return {kind='rule',applies_to={flag='novel.ready'},
    on={['novel.cue']=function(self)end},tests={{when={steps=0},
    expect={subject='target',term='novel.heat',equals=0}}}})");
  add_rule(f.rules, f.scripts, f.terms, rule);
  project_rules(f.triggers, f.rules);
  auto before = drive::plan_event(f.triggers, f.values(), {f.a, {}, 6, {}, {}}, 1, 20);
  f.state(f.a, 5, 1);
  auto after = drive::plan_event(f.triggers, f.values(), {f.a, {}, 6, {}, {}}, 1, 20);
  require(before.empty() && after.size() == 1 && after[0].handler.script == rule,
          "dispatch resolves current authored flag values through the declared value source");
  auto stale = Handle{f.a.bits + (std::uint64_t{1} << 32)};
  require(!drive::valid_event({{stale, {}, 6, {}, {}}, {}}, identity(f.objects), f.processes,
                              f.subscriptions),
          "stale queued events are rejected by full generation");
}

void declared_term_inputs_have_one_runtime_type() {
  Runner runner;
  auto &f = runner.f;
  f.terms.rows[7].inputs.push_back({"resource", InputType::Term});
  auto script = f.add(R"(return {kind='model',actions={['novel.touch']={right='use'}},
    on={['novel.touch']=function(self,from,args)
      assert(args.resource==1)
      self.state.resource=args.resource
      self.state.count=(self.state.count or 0)+1
    end}})");
  f.models.rows[1].script = script;
  project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  for (const auto &atom :
       std::array<EventAtom, 3>{std::string("novel.heat"), Value(1.0), TermRef{1}}) {
    ActionRequest request{PlayerId{1}, 7, f.a, std::vector<EventField>{{"resource", atom}}};
    require(command::validate_arguments(f.terms, f.terms.rows[7], request.args).empty(),
            "declared term accepts registry name, numeric id and typed term reference");
    runner.action({request, {}});
  }
  require(std::get<double>(f.states.at({f.b, script}).at(std::string("count"))) == 3,
          "real Luau sees the identical numeric term id for all three wire encodings");
}

int birth(lua_State *state) {
  StructuralRequest request;
  request.binding.model = 1;
  require(queue_structural(invocation(state), request), "approved world rule may queue creation");
  return 0;
}
void structural_completion_and_native_boundaries() {
  Runner runner;
  auto &f = runner.f;
  register_binding(f.vm, "birth", birth, nullptr);
  auto rule = f.add(R"(return {kind='rule',applies_to={material='novel.mat'},
    on={['novel.touch']=function(self)self:birth();self:emit(self.handle,'novel.cue',true)end},
    tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=0}}}})");
  drive::StructuralOperations operations;
  operations.rows[StructuralRequest::Kind::Create] = {
      [](void *context, const drive::StructuralCall &call) {
        auto &objects = *static_cast<Objects *>(context);
        ObjectInit init;
        init.model = call.request.binding.model;
        auto made = create(objects, init);
        record::Event event;
        event.kind = record::Kind::Create;
        event.things = {made};
        return drive::StructuralCompletion{true, {made}, {event}, {}, {}};
      },
      &f.objects};
  drive::PlannedInvocation plan;
  plan.handler = {rule, "novel.touch", 1, 1, true, true};
  plan.input.receiver = f.a;
  plan.input.trigger = 7;
  auto completed = drive::execute(runner.execution, plan, operations);
  require(completed.invocation.outcome.status == RunOutcome::Status::Ok &&
              completed.invocation.effects.changed && completed.created.size() == 1 &&
              identity(f.objects).valid(completed.created[0]) &&
              completed.invocation.effects.structural.empty(),
          "typed creation output allocates after VM return and changes the direct action result");
  record::Event event;
  event.kind = record::Kind::Action;
  event.name = "novel.touch";
  std::vector<bindings::CompletedInvocation> invocations;
  invocations.push_back(std::move(completed.invocation));
  bindings::publish_action(runner.journal, runner.queues, runner.timers, event,
                           std::move(invocations), {}, {}, 1000);
  auto logs = runner.journal.take();
  require(logs.size() == 3 && logs[0]["changed"] == true && logs[1]["kind"] == "handler" &&
              logs[2]["kind"] == "create" && logs[2]["cause"] == logs[1]["id"],
          "completed action and handler precede causal creation without another chosen action");
  auto child = step::pop_foreground(runner.queues);
  require(child && std::get<step::EventWork>(child->work).origin.cause == logs[1]["id"],
          "descendants are published only after structural effects and completed records");
  struct NativeState {
    unsigned calls = 0;
    bool borrowed_body = false;
    std::optional<drive::InputOrder> order;
  } native_state;
  drive::NativeOperations native;
  drive::register_native(native, 7,
                         {[](const nlohmann::json &args) -> std::string {
                            return args.contains("value") ? "" : "missing value";
                          },
                          [](void *context, const drive::NativeCall &call) {
                            auto &state = *static_cast<NativeState *>(context);
                            ++state.calls;
                            state.borrowed_body = bool(call.body);
                            state.order = call.order;
                            drive::NativeCompletion result;
                            result.changed = true;
                            result.deferred = call.order.has_value();
                            result.reply = {{"ok", true}};
                            return result;
                          },
                          &native_state});
  drive::NativeCall call;
  call.request = {ScriptHolder{rule, f.a}, 7, f.a, {}};
  call.body = f.b;
  call.order = drive::InputOrder{501, 17, 8, 23};
  auto denied = drive::invoke_native(native, call);
  require(denied.result == record::Result::Denied && !native_state.calls &&
              denied.reply["error"] == "bad_input" && denied.reply["problem"] == "missing value",
          "native typed validation runs before its narrow mutation adapter");
  call.arguments = {{"value", 4}};
  auto applied = drive::invoke_native(native, call);
  require(applied.changed && native_state.calls == 1 && !native_state.borrowed_body &&
              !native_state.order,
          "native adapter receives its own context and no borrowed script caller body");
  auto player_call = call;
  player_call.request.caller = PlayerId{1};
  auto pending = drive::invoke_native(native, player_call);
  require(pending.deferred && native_state.order && native_state.order->input == 501 &&
              native_state.order->session == 17 && native_state.order->sequence == 8 &&
              native_state.order->request == 23,
          "native authoring receives exact copied input order only from a player call");
  event.actor = record::Actor::script(rule);
  drive::publish_native(runner.journal, runner.queues, runner.timers, event, std::move(applied), {},
                        {}, 1, 1000);
  logs = runner.journal.take();
  require(logs.size() == 1 && logs[0]["kind"] == "action" && logs[0]["changed"] == true &&
              logs[0]["response"]["ok"] == true,
          "native action records actual effect without synthesizing a script handler");
}
} // namespace
int main() {
  try {
    clock_budget_and_background_progress();
    copied_inputs_and_restore();
    real_vm_causal_order_and_explicit_watch_modeling();
    body_scope_flags_and_reach();
    declared_term_inputs_have_one_runtime_type();
    structural_completion_and_native_boundaries();
    std::cout << "Single-writer phase, actual VM, causal order and authority checks passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
