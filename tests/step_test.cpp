#include "step/step.hpp"
#include <iostream>
#include <stdexcept>
using namespace amber;
using namespace amber::step;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
react::EventWork event(TermId id) {
  react::EventWork e;
  e.event = id;
  return e;
}
void storing_discards_work_without_a_suspended_execution() {
  WorkQueues queues;
  TimerWheel wheel;
  const auto stored = Handle::make(1, 1), live = Handle::make(2, 1);
  auto work = event(3);
  work.receiver = stored;
  enqueue_event(queues, work, 100);
  queues.turns.push_back({work, {}});
  queues.background.push_back({stored, 3, 1, {}, 1});
  schedule(wheel, {10, work, {}});
  work.receiver = live;
  enqueue_event(queues, work, 100);
  schedule(wheel, {10, work, {}});
  discard_script_work(queues, wheel, std::span(&stored, 1));
  require(queues.emitted_events == 1 && queues.causal.size() == 1 && queues.turns.empty() &&
              queues.background.empty() && timers(wheel).size() == 1 &&
              timers(wheel)[0].event.receiver == live,
          "storage retained work or removed another object's work");
}
void publication_stamps_completed_origin() {
  record::Journal journal(8, {});
  record::Event action;
  action.kind = record::Kind::Action;
  action.name = "build";
  action.changed = true;
  auto parent = journal.append(action);
  record::Event handler;
  handler.kind = record::Kind::Handler;
  handler.name = "constructor";
  handler.changed = true;
  auto origin = journal.append(handler, parent);
  react::DeferredEffects effects;
  effects.ordered.push_back(
      react::EventWork{Handle::make(1, 1), Handle::make(2, 1), 1, Value(1.0), {}});
  effects.ordered.push_back(
      react::Timer{3, {Handle::make(1, 1), Handle::make(1, 1), 3, Value{}, {}}, {}});
  step::WorkQueues q;
  step::TimerWheel wheel;
  publish_effects(std::move(effects), origin, 0, q, wheel, journal, 10);
  auto event = std::get<react::EventWork>(step::pop_foreground(q)->work);
  require(event.origin.cause == origin.cause && event.origin.root == parent.root,
          "child stamps completed handler origin");
  require(step::timers(wheel)[0].event.origin == origin, "timer keeps complete origin");
}
void ordering_and_overflow() {
  WorkQueues q;
  append_inputs(q, {event(1), event(2)});
  require(std::get<react::EventWork>(pop_foreground(q)->work).event == 1, "first external turn");
  amber::Origin first;
  first.cause = 41;
  require(enqueue_watch(q, {{Handle::make(3, 1), 8}, first}) == PushResult::Queued, "watch queued");
  amber::Origin second;
  second.cause = 42;
  require(enqueue_watch(q, {{Handle::make(3, 1), 8}, second}) == PushResult::Coalesced,
          "watch coalesces");
  require(enqueue_event(q, event(3), 1) == PushResult::Queued, "event accepted");
  require(enqueue_event(q, event(4), 1) == PushResult::Overflow, "event bound");
  enqueue_watch(q, {{Handle::make(3, 2), 8}, second});
  require(std::get<react::WatchWork>(pop_foreground(q)->work).origin.cause == 41,
          "first watch origin retained");
  require(std::get<react::EventWork>(pop_foreground(q)->work).event == 3, "causal FIFO");
  require(std::get<react::WatchWork>(pop_foreground(q)->work).source.subject.generation() == 2,
          "full generation key");
  require(std::get<react::EventWork>(pop_foreground(q)->work).event == 2,
          "second turn waits for causes");
  require(!pop_foreground(q), "empty queue");
  enqueue_watch(q, {{Handle::make(4, 1), 5}, {}});
  q.watch_queued.clear();
  rebuild_queue_index(q);
  require(enqueue_watch(q, {{Handle::make(4, 1), 5}, {}}) == PushResult::Coalesced,
          "restored queue index");
}
void timer_cascades() {
  WorkQueues q;
  TimerWheel wheel;
  for (Step due :
       {Step{1}, Step{255}, Step{256}, Step{257}, Step{65535}, Step{65536}, Step{70000}}) {
    auto e = event(static_cast<TermId>(due));
    e.origin.cause = due;
    schedule(wheel, {due, e, {}});
  }
  require(timers(wheel).size() == 7, "timer snapshot enumeration");
  for (Step now = 1; now <= 70000; ++now) {
    promote_due(wheel, now, q);
    while (auto item = pop_foreground(q))
      require(std::get<react::EventWork>(item->work).origin.cause == now,
              "timer exact due after cascade");
  }
  require(timers(wheel).empty(), "all timers consumed");
  schedule(wheel, {wheel.now, event(99), {}});
  promote_due(wheel, wheel.now, q);
  require(!pop_foreground(q), "after zero cannot run now");
  promote_due(wheel, wheel.now + 1, q);
  require(pop_foreground(q).has_value(), "after zero runs next step");
}
void intents() {
  Intents intents;
  auto body = Handle::make(1, 1);
  auto a = start_move(intents, body, Vec3{1, 2, 3}, {});
  auto b = start_move(intents, body, Handle::make(5, 3), {});
  require(b.replaced && b.replaced->intent.id == a.id && b.replaced->reason == IntentEnd::Replaced,
          "intent replacement");
  auto end = cancel_move(intents, body);
  require(end && end->intent.id == b.id, "cancel current intent");
  require(!cancel_move(intents, body), "cancel empty");
}
void saved_work_continues_in_causal_order_with_typed_payload_and_origin() {
  const auto body = Handle::make(17, 3);
  WorkQueues q;
  amber::Origin origin{41, 12, {2, 7}, 3.5, true};
  append_inputs(q, {event(10), InputWork{987654321}});
  enqueue_watch(q, {{body, 8}, origin});
  react::EventWork emitted{body, Handle::make(19, 2), 9,
                           std::vector<EventField>{{"destination", Value(body)},
                                                   {"point", Vec3{1, 2, 3}},
                                                   {"text", std::string("actual value")},
                                                   {"term", TermRef{77}}},
                           origin};
  enqueue_event(q, emitted, 100);
  enqueue_background(q, {body, 12, 8, origin, 6});
  TimerWheel wheel;
  wheel.now = 200;
  schedule(wheel, {65536, emitted, react::ThresholdToken{{body, 8}, body, 5, 2}});
  Intents moving;
  const auto intent = start_move(moving, body, Handle::make(22, 4), origin, .35f);
  snap::Buffer queues, timers;
  snap::Output qout{queues}, tout{timers};
  write_queues(qout, q, moving, 50);
  write_timers(tout, wheel);
  queues.resize(qout.adapter().writtenBytesCount());
  timers.resize(tout.adapter().writtenBytesCount());
  WorkQueues restored;
  TimerWheel restored_wheel;
  Intents restored_intents;
  snap::Input qin{queues.data(), queues.size()}, tin{timers.data(), timers.size()};
  require(read_queues(qin, restored, restored_intents) == 50, "record id lost on restore");
  const auto background = pop_background(restored);
  require(background && background->version == 6 && background->origin == origin,
          "catch-up version and provenance survive restore");
  snap::check_complete(qin);
  read_timers(tin, restored_wheel);
  require(enqueue_watch(restored, {{body, 8}, origin}) == PushResult::Coalesced,
          "restored watch index was not rebuilt");
  const auto watch = std::get<react::WatchWork>(pop_foreground(restored)->work);
  require(watch.source == FactKey{body, 8} && watch.origin == origin,
          "restored watch provenance lost");
  const auto resumed = std::get<react::EventWork>(pop_foreground(restored)->work);
  require(resumed.payload == emitted.payload && resumed.origin == origin,
          "typed payload or causal chain changed on restore");
  require(std::get<react::EventWork>(pop_foreground(restored)->work).event == 10,
          "external input overtook saved causal work");
  require(!pop_foreground(restored), "pending network inputs are not restored");
  promote_due(restored_wheel, 65536, restored);
  const auto fired = pop_foreground(restored);
  require(fired && fired->threshold && fired->threshold->watch_index == 2 &&
              std::get<react::EventWork>(fired->work).origin == origin,
          "saved threshold token did not fire with provenance");
  const auto ending = cancel_move(restored_intents, body);
  require(ending && ending->intent.id == intent.id && ending->intent.origin == origin &&
              std::get<Handle>(ending->intent.goal) == Handle::make(22, 4) &&
              ending->intent.speed_m_s == std::optional<float>{.35f},
          "saved intent did not continue");
}
void threshold_replacement_preserves_other_events_and_cascade_locations() {
  TimerWheel wheel;
  WorkQueues q;
  const auto subject = Handle::make(70, 1), other = Handle::make(71, 1);
  const react::ThresholdToken token{{subject, 8}, subject, 5, 2};
  auto threshold = [&](Step due, react::ThresholdToken version, std::uint64_t cause) {
    auto e = event(8);
    e.receiver = version.consumer;
    e.origin = {cause, 12, {2, 7}, 3.5, false};
    return react::Timer{due, std::move(e), version};
  };
  schedule(wheel, threshold(70000, token, 40));
  auto sibling = token;
  sibling.consumer = other;
  schedule(wheel, threshold(70000, sibling, 41));
  schedule(wheel, {70000, event(100), {}});
  schedule(wheel, {70000, event(101), {}});
  auto current = token;
  std::uint64_t cause = 40;
  for (Step due : {Step{65536}, Step{256}, Step{1}, (Step{1} << 24) + 1, Step{257}}) {
    schedule(wheel, threshold(due, current, ++cause));
    require(timers(wheel).size() == 4 && wheel.thresholds.size() == 2,
            "rescheduling replaces only its threshold across all wheel levels");
  }
  promote_due(wheel, 257, q);
  auto fired = pop_foreground(q);
  require(fired && fired->threshold &&
              std::get<react::EventWork>(fired->work).origin.cause == cause && !pop_foreground(q) &&
              wheel.thresholds.size() == 1,
          "newest crossing fires with its actual cause, after replacing each previous due time");
  schedule(wheel, threshold(65536, current, 77));
  promote_due(wheel, 65536, q);
  fired = pop_foreground(q);
  require(fired && fired->threshold && std::get<react::EventWork>(fired->work).origin.cause == 77 &&
              !pop_foreground(q),
          "a promoted key can be rearmed and traverse a real level-two cascade");
  promote_due(wheel, 70000, q);
  std::size_t ordinary = 0, thresholds = 0;
  while (auto remaining = pop_foreground(q)) {
    ordinary += !remaining->threshold;
    thresholds += bool(remaining->threshold);
  }
  require(ordinary == 2 && thresholds == 1 && timers(wheel).empty() && wheel.thresholds.empty(),
          "swap removal preserves sibling threshold and independent ordinary events");
  TimerWheel distant;
  schedule(distant, threshold((Step{1} << 24) + 1, token, 88));
  // No intermediate rows exist. Place the isolated wheel immediately before its
  // level-three boundary without spending the test on 16 million empty steps.
  distant.now = (Step{1} << 24) - 1;
  promote_due(distant, (Step{1} << 24) + 1, q);
  fired = pop_foreground(q);
  require(fired && fired->threshold && std::get<react::EventWork>(fired->work).origin.cause == 88 &&
              distant.thresholds.empty(),
          "level-three cascade and promotion retire the derived threshold location");
}
void threshold_identity_and_exact_cancellation() {
  const auto subject = Handle::make(7, 1);
  const react::ThresholdToken first{{subject, 8}, subject, 5, 2};
  auto timer = [&](Step due, react::ThresholdToken token) {
    auto e = event(8);
    e.receiver = token.consumer;
    e.origin.cause = due;
    return react::Timer{due, e, token};
  };
  TimerWheel identities;
  schedule(identities, timer(10, first));
  for (unsigned field = 0; field < 5; ++field) {
    auto distinct = first;
    if (field == 0)
      distinct.source.subject = Handle::make(subject.slot(), 2);
    else if (field == 1)
      distinct.source.term = 9;
    else if (field == 2)
      distinct.consumer = Handle::make(subject.slot(), 2);
    else if (field == 3)
      ++distinct.script;
    else
      ++distinct.watch_index;
    schedule(identities, timer(10, distinct));
  }
  require(timers(identities).size() == 6 && identities.thresholds.size() == 6,
          "threshold identity includes full source/consumer generations, script and watch index");
  cancel_threshold(identities, {first.source, first.consumer, first.script, first.watch_index});
  require(timers(identities).size() == 5,
          "rate with no crossing cancels precisely its pending key");
  cancel_threshold(identities, {first.source, first.consumer, first.script, first.watch_index});
  snap::Buffer bytes;
  snap::Output out{bytes};
  write_timers(out, identities);
  bytes.resize(out.adapter().writtenBytesCount());
  TimerWheel restored;
  snap::Input in{bytes.data(), bytes.size()};
  read_timers(in, restored);
  snap::check_complete(in);
  require(timers(restored).size() == 5 && restored.thresholds.size() == 5,
          "snapshot restores only current pending keys");
  WorkQueues q;
  promote_due(restored, 10, q);
  unsigned fired = 0;
  while (auto item = pop_foreground(q)) {
    require(item->threshold && std::get<react::EventWork>(item->work).origin.cause == 10,
            "remaining exact identities retain their cause");
    ++fired;
  }
  require(fired == 5 && restored.thresholds.empty(), "cancelled key never wakes after restore");
}
void one_same_due_fixture_continues_after_snapshot() {
  TimerWheel uninterrupted;
  const auto subject = Handle::make(9, 2);
  react::ThresholdToken token{{subject, 4}, subject, 3, 1};
  auto make_threshold = [&](TermId id, react::ThresholdToken version) {
    auto e = event(id);
    e.receiver = version.consumer;
    e.origin.cause = id;
    return react::Timer{70000, e, version};
  };
  schedule(uninterrupted, make_threshold(20, token));
  auto sibling = token;
  ++sibling.watch_index;
  schedule(uninterrupted, make_threshold(21, sibling));
  schedule(uninterrupted, {70000, event(22), {}});
  schedule(uninterrupted, {70000, event(23), {}});
  WorkQueues before;
  promote_due(uninterrupted, 65000, before);
  require(!pop_foreground(before), "tie snapshot setup did not reach its timers yet");
  schedule(uninterrupted, {70000, event(24), {}});
  schedule(uninterrupted, make_threshold(25, token));
  snap::Buffer bytes;
  snap::Output out{bytes};
  write_timers(out, uninterrupted);
  bytes.resize(out.adapter().writtenBytesCount());
  TimerWheel resumed;
  snap::Input in{bytes.data(), bytes.size()};
  read_timers(in, resumed);
  snap::check_complete(in);
  WorkQueues after;
  promote_due(uninterrupted, 70000, before);
  promote_due(resumed, 70000, after);
  std::vector<TermId> actual, restored;
  while (auto item = pop_foreground(before))
    actual.push_back(std::get<react::EventWork>(item->work).event);
  while (auto item = pop_foreground(after))
    restored.push_back(std::get<react::EventWork>(item->work).event);
  require(actual.size() == 5 && actual == restored &&
              std::find(actual.begin(), actual.end(), 20) == actual.end(),
          "mixed timer ties preserve uninterrupted continuation after replacement and snapshot");
}

void mixed_due_replacement_preserves_events_across_rebucketing() {
  TimerWheel original;
  const auto subject = Handle::make(15, 3);
  react::ThresholdToken token{{subject, 6}, subject, 7, 1};
  auto e = event(30);
  e.receiver = subject;
  e.origin.cause = 60;
  schedule(original, {1100, e, token});
  auto a = event(31), b = event(32);
  a.origin.cause = 61;
  b.origin.cause = 62;
  schedule(original, {1110, a, {}});
  schedule(original, {1110, b, {}});
  WorkQueues empty;
  promote_due(original, 900, empty);
  snap::Buffer bytes;
  snap::Output out{bytes};
  write_timers(out, original);
  bytes.resize(out.adapter().writtenBytesCount());
  TimerWheel restored;
  snap::Input in{bytes.data(), bytes.size()};
  read_timers(in, restored);
  snap::check_complete(in);
  e.origin.cause = 63;
  for (auto *wheel : {&original, &restored}) {
    schedule(*wheel, {1200, e, token});
    WorkQueues q;
    promote_due(*wheel, 1100, q);
    require(!pop_foreground(q), "replaced threshold cannot fire at its obsolete earlier deadline");
    promote_due(*wheel, 1110, q);
    append_inputs(q, {event(33)});
    const auto first = std::get<react::EventWork>(pop_foreground(q)->work);
    const auto second = std::get<react::EventWork>(pop_foreground(q)->work);
    require(
        ((first.event == 31 && second.event == 32) || (first.event == 32 && second.event == 31)) &&
            first.origin.cause + second.origin.cause == 123 &&
            std::get<react::EventWork>(pop_foreground(q)->work).event == 33 && !pop_foreground(q),
        "rebucketing may change timer ties but loses no ordinary event or post-promotion FIFO");
    promote_due(*wheel, 1200, q);
    const auto actual = pop_foreground(q);
    require(
        actual && actual->threshold &&
            std::get<react::EventWork>(actual->work).origin.cause == 63 && !pop_foreground(q) &&
            timers(*wheel).empty(),
        "replacement preserves the exact new threshold deadline and latest cause across restore");
  }
}
} // namespace
int main() {
  storing_discards_work_without_a_suspended_execution();
  publication_stamps_completed_origin();
  ordering_and_overflow();
  timer_cascades();
  threshold_replacement_preserves_other_events_and_cascade_locations();
  threshold_identity_and_exact_cancellation();
  one_same_due_fixture_continues_after_snapshot();
  mixed_due_replacement_preserves_events_across_rebucketing();
  intents();
  saved_work_continues_in_causal_order_with_typed_payload_and_origin();
  std::cout << "scheduler checks passed\n";
}
