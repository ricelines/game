#include "runtime_fixture.hpp"
#include "step/step.hpp"

#include <iostream>

using namespace amber;
using namespace amber::react;
using test_runtime::Fixture;
using test_runtime::require;
namespace {
void clock_budget_and_background_progress() {
  step::PhaseCosts costs(20);
  for (int i = 0; i < 21; ++i)
    costs.observe(i);
  require(costs.mean_ms() == 10.5, "phase average retains the latest twenty samples");
  step::Clock clock;
  step::WorkQueues queues;
  step::TimerWheel timers;
  auto now = step::Monotonic::now();
  auto frame = step::begin_frame(clock, {20, 5, 20}, costs, queues, timers, {}, now);
  const auto allowance = std::chrono::duration<double, std::milli>(frame.handler_deadline - now);
  require(frame.step == 1 && std::abs(allowance.count() - 34.5) < .001,
          "admission budget subtracts average other phases and margin");
  auto late = frame.boundary + std::chrono::milliseconds(7);
  require(step::finish_frame(clock, costs, frame, 4, late) == late && clock.overruns == 1,
          "actual overrun advances one quantum without a catch-up burst");

  step::PhaseCosts overloaded(20);
  overloaded.observe(60);
  step::Clock busy;
  step::WorkQueues pending;
  step::TimerWheel due;
  std::deque<elaborate::FixtureResult> fixtures;
  std::deque<authoring::Completion> authored;
  std::vector<std::string> trace;
  step::enqueue_event(pending, {{}, {}, 1, {}, {}}, 100);
  step::append_inputs(pending, {step::InputWork{9}});
  step::schedule(due, {1, {{}, {}, 2, {}, {}}, {}});
  for (unsigned i = 0; i < 5; ++i)
    step::enqueue_background(pending, {});
  for (unsigned i = 0; i < 5; ++i) {
    const auto started = step::Monotonic::now();
    auto current = step::begin_frame(
        busy, {20, 5, 20}, overloaded, pending, due,
        i == 0 ? std::vector<step::Work>{step::InputWork{10}} : std::vector<step::Work>{}, started);
    require(current.handler_deadline == started, "prediction can leave zero admission allowance");
    const auto admitted = step::next_work(pending, current, fixtures, authored, started);
    require(admitted && std::holds_alternative<step::ForegroundItem>(*admitted),
            "prediction starved the oldest foreground item");
    const auto &item = std::get<step::ForegroundItem>(*admitted);
    if (const auto *event = std::get_if<step::EventWork>(&item.work)) {
      trace.push_back("event" + std::to_string(event->event));
      if (event->event == 1)
        step::enqueue_event(pending, {{}, {}, 3, {}, {}}, 100);
    } else
      trace.push_back("input" + std::to_string(std::get<step::InputWork>(item.work).id));
    const auto background = step::next_work(pending, current, fixtures, authored, started);
    require(background && std::holds_alternative<step::BackgroundItem>(*background),
            "expired foreground allowance starved the background minimum");
    trace.push_back("background");
    require(!step::next_work(pending, current, fixtures, authored, started),
            "zero allowance must admit exactly one item from each nonempty lane");
  }
  require(trace == std::vector<std::string>{"event1", "background", "event3", "background",
                                            "input9", "background", "event2", "background",
                                            "input10", "background"},
          "zero allowance changed causal leftovers, older inputs, due timers and new inputs");
  require(pending.causal.empty() && pending.turns.empty() && pending.background.empty(),
          "persistent expensive-phase prediction starved queued work");
  step::Frame empty;
  empty.handler_deadline = step::Monotonic::now();
  require(!step::next_work(pending, empty, fixtures, authored, empty.handler_deadline),
          "foreground minimum invented work for an empty lane");
}

void completion_priority_and_stale_admission_refunds() {
  step::WorkQueues queues;
  std::deque<elaborate::FixtureResult> fixtures;
  std::deque<authoring::Completion> authored;
  elaborate::FixtureResult fixture;
  fixture.job = 11;
  fixtures.push_back(fixture);
  fixture.job = 12;
  fixtures.push_back(fixture);
  authored.push_back({21, authoring::Failure{"first completed submission"}});
  authored.push_back({22, authoring::Failure{"second completed submission"}});
  step::enqueue_event(queues, {{}, {}, 7, {}, {}}, 100);
  step::append_inputs(queues, {step::InputWork{9}});
  step::enqueue_background(queues, {{}, 31, 0, {}, 0});
  step::enqueue_background(queues, {{}, 32, 0, {}, 0});
  const auto now = step::Monotonic::time_point{};
  step::Frame open;
  open.handler_deadline = now + std::chrono::seconds(1);
  std::vector<std::string> trace;
  while (auto work = step::next_work(queues, open, fixtures, authored, now)) {
    if (const auto *item = std::get_if<step::ForegroundItem>(&*work)) {
      if (const auto *event = std::get_if<step::EventWork>(&item->work))
        trace.push_back("event" + std::to_string(event->event));
      else
        trace.push_back("input" + std::to_string(std::get<step::InputWork>(item->work).id));
    } else if (const auto *result = std::get_if<elaborate::FixtureResult>(&*work))
      trace.push_back("fixture" + std::to_string(result->job));
    else if (const auto *result = std::get_if<authoring::Completion>(&*work))
      trace.push_back("authoring" + std::to_string(result->input_id));
    else
      trace.push_back("background" + std::to_string(std::get<step::BackgroundItem>(*work).trigger));
  }
  require(trace == std::vector<std::string>{"event7", "input9", "fixture11", "fixture12",
                                            "authoring21", "authoring22", "background31",
                                            "background32"},
          "admission changed foreground/completion priority or FIFO within a source");

  step::Frame closed;
  closed.handler_deadline = now;
  step::append_inputs(queues, {step::InputWork{40}, step::InputWork{41}});
  fixtures.push_back(fixture);
  authored.push_back({42, authoring::Failure{"awaiting writer admission"}});
  step::enqueue_background(queues, {{}, 43, 0, {}, 0});
  auto first = step::next_work(queues, closed, fixtures, authored, now);
  require(first && std::get<step::InputWork>(std::get<step::ForegroundItem>(*first).work).id == 40,
          "expired allowance skipped the first foreground minimum");
  --closed.foreground; // The caller discarded this missing/awaiting input.
  const auto retry = step::next_work(queues, closed, fixtures, authored, now);
  require(retry && std::get<step::InputWork>(std::get<step::ForegroundItem>(*retry).work).id == 41,
          "a stale input consumed the only foreground admission");
  const auto background = step::next_work(queues, closed, fixtures, authored, now);
  require(background && std::holds_alternative<step::BackgroundItem>(*background) &&
              !step::next_work(queues, closed, fixtures, authored, now),
          "pending completions bypassed exhausted foreground allowance or starved background");

  step::Frame next;
  next.handler_deadline = now;
  const auto stale_fixture = step::next_work(queues, next, fixtures, authored, now);
  require(stale_fixture && std::holds_alternative<elaborate::FixtureResult>(*stale_fixture),
          "fixture completion lost its foreground minimum when ordinary work was empty");
  --next.foreground; // The caller discarded an already-closed fixture job.
  const auto submission = step::next_work(queues, next, fixtures, authored, now);
  require(submission && std::get<authoring::Completion>(*submission).input_id == 42 &&
              !step::next_work(queues, next, fixtures, authored, now),
          "stale fixture refund failed or completion sources gained separate minima");
  step::enqueue_background(queues, {{}, 44, 0, {}, 0});
  step::enqueue_background(queues, {{}, 45, 0, {}, 0});
  const auto stale_background = step::next_work(queues, next, fixtures, authored, now);
  require(stale_background && std::get<step::BackgroundItem>(*stale_background).trigger == 44,
          "background minimum skipped the oldest catch-up item");
  --next.background; // The caller discarded invalid catch-up work.
  const auto catch_up = step::next_work(queues, next, fixtures, authored, now);
  require(catch_up && std::get<step::BackgroundItem>(*catch_up).trigger == 45 &&
              !step::next_work(queues, next, fixtures, authored, now),
          "a stale catch-up item consumed the background minimum");
}

void copied_inputs_and_restore() {
  step::Inputs inputs;
  step::Input one;
  one.kind = net::ControlKind::RegisterModel;
  one.caller = ScriptHolder{4, Handle{123}};
  one.session = 17;
  one.sequence = 8;
  one.request = 23;
  one.fields = {{"source", "copied source"}};
  one.origin = {7, 5, {2, 3}, 1.5, false};
  auto first = step::enqueue_input(inputs, one);
  one.fields["source"] = "later caller mutation";
  inputs.rows.at(first.id).awaiting = true;
  require(inputs.rows.at(first.id).fields["source"] == "copied source",
          "pending input retained mutable caller fields");
  auto two = one;
  two.session = 18;
  const auto second = step::enqueue_input(inputs, two);
  const auto immediate = step::complete_input(inputs, second.id, {{"ok", true}});
  require(immediate.session == 18 && inputs.rows.contains(first.id),
          "independent completion removed another pending input");
  snap::Buffer bytes;
  snap::Output out{bytes};
  step::write_inputs(out, inputs);
  out.adapter().flush();
  step::Inputs restored;
  snap::Input in{bytes.data(), bytes.size()};
  step::read_inputs(in, restored);
  const auto &row = restored.rows.at(first.id);
  require(row.awaiting && row.origin == one.origin && row.caller == one.caller &&
              row.fields["source"] == "copied source" && restored.next_id == inputs.next_id,
          "pending input codec lost typed authority, copied data or identity high water");
  auto reply = step::complete_input(restored, first.id, {{"ok", true}, {"model", 42}});
  require(reply.session == 17 && reply.sequence == 8 && reply.request == 23 &&
              reply.fields["model"] == 42 && restored.rows.empty(),
          "completion did not return to its exact original request");
}

void explicit_watch_planning_and_live_dispatch() {
  Fixture f;
  const auto bare = f.add(R"(return {kind='model',
    on={['novel.signal']=function(self)error('not an explicit watch')end}})");
  f.models.rows[1].script = bare;
  project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  f.shared.resize(5);
  f.shared[4].set(f.a.slot(), Shared{0, {}});
  auto gap = step::plan_watch(f.subscriptions, f.values(), f.allocations, f.triggers, f.scripts,
                              {{f.a, 4}, {}}, 1, 20);
  require(!gap.modeled && gap.shared_gap == FactKey{f.a, 4} && gap.invocations.empty(),
          "bare quantity handler gained an implicit watch");
  const auto watcher = f.add(R"(return {kind='instance',watches={'novel.signal'},
    on={['novel.signal']=function(self)self.state.woke=true end}})");
  const std::array ids{watcher};
  set_instances(f.installed, f.a, ids, f.scripts);
  project_instance(f.triggers, f.installed, f.scripts, f.terms, f.a);
  f.bind(watcher, f.a);
  const auto unchanged = step::plan_watch(f.subscriptions, f.values(), f.allocations, f.triggers,
                                          f.scripts, {{f.a, 4}, {}}, 1, 20);
  require(unchanged.modeled && !unchanged.shared_gap && unchanged.invocations.empty(),
          "applicable explicit watch failed to model an unchanged answer");
  f.shared[4].set(f.a.slot(), Shared{1, {}});
  const auto changed = step::plan_watch(f.subscriptions, f.values(), f.allocations, f.triggers,
                                        f.scripts, {{f.a, 4}, {}}, 1, 20);
  require(changed.invocations.size() == 1 && changed.invocations[0].handler.script == watcher &&
              changed.invocations[0].input.receiver == f.a,
          "changed shared value did not select the actual subscribed handler");

  const auto rule = f.add(R"(return {kind='rule',applies_to={flag='novel.ready'},
    on={['novel.cue']=function(self)end},tests={{when={steps=0},
    expect={subject='target',term='novel.heat',equals=0}}}})");
  add_rule(f.rules, f.scripts, f.terms, rule);
  project_rules(f.triggers, f.rules);
  const auto before = step::plan_event(f.triggers, f.values(), {f.a, {}, 6, {}, {}}, 1, 20);
  f.state(f.a, 5, 1);
  const auto after = step::plan_event(f.triggers, f.values(), {f.a, {}, 6, {}, {}}, 1, 20);
  require(before.empty() && after.size() == 1 && after[0].handler.script == rule,
          "dispatch did not resolve the current authored selector value");
  const auto stale = Handle::make(f.a.slot(), f.a.generation() + 1);
  require(!step::valid_event({{stale, {}, 6, {}, {}}, {}}, identity(f.objects), f.processes,
                             f.subscriptions, 1, 20),
          "stale queued event passed full-generation validation");
}

void action_plans_keep_authority_separate() {
  Fixture f;
  const auto body = f.add(R"(return {kind='model',actions={['novel.touch']={right='use',
    reach=true}},on={['novel.touch']=function(self)end}})");
  const auto sibling = f.add(R"(return {kind='instance',
    on={['novel.touch']=function(self)end}})");
  f.models.rows[1].script = body;
  project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  const std::array ids{sibling};
  set_instances(f.installed, f.b, ids, f.scripts);
  project_instance(f.triggers, f.installed, f.scripts, f.terms, f.b);
  ActionRequest request{PlayerId{1}, 7, f.a, {}};
  auto resolution =
      resolve({}, f.terms, f.models, f.scripts, f.installed, identity(f.objects), request, f.b);
  auto plans = step::plan_action(f.triggers, f.values(), resolution, request, f.b, {}, 1, 20);
  require(plans.size() == 2 && plans[0].input.receiver == f.b && plans[1].input.receiver == f.b &&
              plans[0].input.sender == f.a && plans[0].input.action_body == f.b &&
              plans[0].input.action_target == f.a && plans[0].input.action_caller == request.caller,
          "body handlers lost actual receiver, selected sender or descriptive player metadata");
  perm::Players players;
  players.rows.resize(2);
  f.objects.owner[f.a.slot()] = 1;
  const auto selected = perm::target(identity(f.objects), authority(f.objects), {}, f.a);
  require(!perm::check(perm::Holder::script(body, f.b), perm::Verb::Transfer, selected, players, {},
                       1) &&
              perm::check(perm::Holder::player(1), perm::Verb::Transfer, selected, players, {}, 1),
          "planned script identity inherited the player's Transfer authority");
  request.caller = ScriptHolder{sibling, f.c};
  resolution =
      resolve({}, f.terms, f.models, f.scripts, f.installed, identity(f.objects), request, f.b);
  plans = step::plan_action(f.triggers, f.values(), resolution, request, f.b, {}, 1, 20);
  require(plans.size() == 1 && !plans[0].input.action_body && plans[0].input.sender == f.c &&
              plans[0].input.action_caller == request.caller,
          "script-generated action borrowed an authenticated player body");
  require(step::in_reach({99, 0, 1}, {{-100, -1, 0}, {100, 1, 3}}, 1) &&
              !step::in_reach({99, 4, 1}, {{-100, -1, 0}, {100, 1, 3}}, 1),
          "reach used the center instead of the nearest measured bounds");
  f.terms.rows[7].inputs.push_back({"resource", InputType::Term});
  for (const auto &atom :
       std::array<EventAtom, 3>{std::string("novel.heat"), Value(1.0), TermRef{1}}) {
    const auto normalized =
        step::normalize_arguments(f.terms, 7, std::vector<EventField>{{"resource", atom}});
    require(std::get<TermRef>(std::get<std::vector<EventField>>(normalized)[0].value).id == 1,
            "declared term input did not normalize to one actual registry reference");
  }
}
} // namespace
int main() {
  try {
    clock_budget_and_background_progress();
    completion_priority_and_stale_admission_refunds();
    copied_inputs_and_restore();
    explicit_watch_planning_and_live_dispatch();
    action_plans_keep_authority_separate();
    std::cout << "Scheduler, copied-input and live invocation planning checks passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
