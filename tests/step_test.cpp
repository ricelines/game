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
void native_action_input_contracts() {
  react::Terms terms;
  react::NativeActions native;
  declare_commerce(terms, native);
  declare_admin(terms, native);
  editing::declare_editing(terms, native);
  declare_elaboration(terms, native);
  declare_construction_actions(terms, native);
  declare_physical_actions(terms, native);
  const auto check = [&](std::string_view name, const Json &arguments) {
    return check_input(native.rows.at(terms.by_name.at(std::string(name))).inputs_schema,
                       arguments);
  };
  for (const auto &[name, arguments] : std::initializer_list<std::pair<std::string_view, Json>>{
           {"engine.pay", {{"to", "p:2"}, {"amount", INT64_MAX}}},
           {"engine.trade_change", {{"trade", "trade:1"}, {"money", 0}, {"goods", nullptr}}},
           {"engine.set_setting",
            {{"values", {{"novel.gain", 2.5}, {"novel.on", true}, {"novel.default", nullptr}}}}},
           {"engine.split_plot", {{"plot", 1}, {"x", 8.0}}},
           {"engine.grant",
            {{"holder", {{"script", "s:1"}, {"on", "world"}}},
             {"scope", {{"object", "o:1.1"}}},
             {"rights", {"use", "use"}}}},
           {"engine.revoke",
            {{"holder", "p:2"}, {"scope", {{"owned_on_plot", {{"owner", "p:1"}, {"plot", 1}}}}}}},
           {"engine.relate", {{"term", UINT32_MAX}, {"other", "o:1.1"}, {"present", true}}},
           {"engine.define_term",
            {{"proposal",
              {{"name", "heat"},
               {"kind", "quantity"},
               {"along", 0},
               {"paired", 0},
               {"priority", 0}}}}},
           {"engine.create", {{"model", "m:1"}, {"material", 1}, {"position", {0, 0, 0}}}},
           {"engine.create",
            {{"model", "m:1"}, {"material", "novel.metal"}, {"position", {0, 0, 0}}}},
           {"engine.answer",
            {{"request", "request:1"},
             {"kind", "rule"},
             {"script", "s:1"},
             {"subjects", {{"door", "o:1.1"}}}}},
           {"engine.answer",
            {{"request", "request:1"}, {"kind", "detail"}, {"text", "A detail"}}}}) {
    const auto problem = check(name, arguments);
    require(problem.empty(), (std::string(name) + ": " + problem).c_str());
  }
  const auto wrapped_id = (std::uint64_t{1} << 32) + 1;
  for (const auto &[name, arguments] : std::initializer_list<std::pair<std::string_view, Json>>{
           {"engine.plot_permissions", {{"plot", wrapped_id}, {"everyone", {"enter"}}}},
           {"engine.grant",
            {{"holder", "p:2"}, {"scope", {{"plot", wrapped_id}}}, {"rights", {"enter"}}}},
           {"engine.grant",
            {{"holder", "p:2"},
             {"scope", {{"owned_on_plot", {{"owner", "p:1"}, {"plot", wrapped_id}}}}},
             {"rights", {"use"}}}},
           {"engine.install",
            {{"script", "s:1"},
             {"choices",
              {{{"slot", "enter"},
                {"scope", {{"plot", wrapped_id}}},
                {"rights", {"enter"}},
                {"expires_s", 60}}}}}},
           {"engine.install",
            {{"script", "s:1"},
             {"choices",
              {{{"slot", "use"},
                {"scope", {{"owned_on_plot", {{"owner", "p:1"}, {"plot", wrapped_id}}}}},
                {"rights", {"use"}},
                {"expires_s", 60}}}}}},
           {"engine.create", {{"model", "m:1"}, {"material", wrapped_id}, {"position", {0, 0, 0}}}},
           {"engine.reconfigure", {{"model", "m:1"}, {"material", wrapped_id}}},
           {"engine.relate", {{"term", wrapped_id}, {"other", "o:1.1"}, {"present", true}}},
           {"engine.relate", {{"term", -4294967295LL}, {"other", "o:1.1"}, {"present", true}}},
           {"engine.define_material", {{"material", wrapped_id}, {"values", Json::object()}}},
           {"engine.define_term",
            {{"proposal", {{"name", "heat"}, {"kind", "quantity"}, {"along", wrapped_id}}}}},
           {"engine.define_term",
            {{"proposal",
              {{"name", "touch"},
               {"kind", "action"},
               {"inputs", {{{"name", "amount"}, {"term", wrapped_id}}}}}}}},
           {"engine.define_term",
            {{"proposal", {{"name", "metal"}, {"kind", "material"}}},
             {"choice", {{"same_as", wrapped_id}}}}},
           {"engine.pay", {{"to", "p:2"}, {"amount", -1}}},
           {"engine.pay", {{"to", "p:2"}, {"amount", 1.0}}},
           {"engine.pay", {{"to", "p:2"}, {"amount", true}}},
           {"engine.pay", {{"to", "p:2"}, {"amount", std::uint64_t{1} << 63}}},
           {"engine.pay", {{"to", "p:2"}, {"amount", 0}, {"extra", true}}},
           {"engine.set_setting", {{"values", {{"novel.gain", "loud"}}}}},
           {"engine.patch_state", {{"values", {{"novel.gain", nullptr}}}}},
           {"engine.split_plot", {{"plot", 1}}},
           {"engine.split_plot", {{"plot", 1}, {"x", 8}, {"y", 8}}},
           {"engine.join_plots", {{"plots", {1}}}},
           {"engine.join_plots", {{"plots", {1, 2, 3}}}},
           {"engine.grant",
            {{"holder", "p:2"}, {"scope", {{"object", "o:1.1"}}}, {"rights", Json::array()}}},
           {"engine.grant",
            {{"holder", "p:2"},
             {"scope", {{"object", "o:1.1"}, {"owned", "p:1"}}},
             {"rights", {"use"}}}},
           {"engine.grant",
            {{"holder", "p:2"},
             {"scope", {{"owned_on_plot", {{"owner", "p:1"}}}}},
             {"rights", {"use"}}}},
           {"engine.revoke", Json::object()},
           {"engine.revoke", {{"grant", "g:1"}, {"holder", "p:2"}}},
           {"engine.reconfigure", {{"model", "m:1"}, {"params", std::uint64_t{1} << 32}}},
           {"engine.create", {{"model", "m:1"}, {"material", false}, {"position", {0, 0, 0}}}},
           {"engine.attach", {{"parent", "o:1.1"}, {"local_pose", {{"rotation", {0, 0, 1}}}}}},
           {"engine.edit_detail", {{"id", std::uint64_t{1} << 32}, {"text", "A detail"}}},
           {"engine.answer",
            {{"request", "request:1"},
             {"kind", "rule"},
             {"script", "s:1"},
             {"note", "foreign field"}}},
           {"engine.answer",
            {{"request", "request:1"}, {"kind", "patch"}, {"text", "wrong field"}}},
           {"engine.define_term",
            {{"proposal", {{"name", "metal"}, {"kind", "material"}}}, {"choice", Json::object()}}},
           {"engine.define_term",
            {{"proposal", {{"name", "metal"}, {"kind", "material"}}},
             {"choice", {{"same_as", 1}, {"none_fits", "distinct"}}}}}}) {
    const auto problem = check(name, arguments);
    require(!problem.empty(), (std::string(name) + " accepted " + arguments.dump()).c_str());
  }
  const auto problem = check(
      "engine.attach", {{"parent", "o:1.1"}, {"local_pose", {{"position", {0, "north", 0}}}}});
  require(problem.find("local_pose.position[1]") != std::string::npos &&
              problem.find("number") != std::string::npos,
          "nested input error did not identify its field and required type");
}
void queued_and_scheduled_work_records_when_it_was_issued() {
  WorkQueues queues;
  TimerWheel wheel;
  wheel.now = 7;
  enqueue_event(queues, event(3), 100, 5);
  schedule(wheel, {10, event(3), {}});
  require(std::get<react::EventWork>(queues.causal.front().work).issued == 5 &&
              timers(wheel)[0].event.issued == 7,
          "queued and scheduled work did not record the step it was issued");
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
  require(enqueue_event(q, event(3), 1, 0) == PushResult::Queued, "event accepted");
  require(enqueue_event(q, event(4), 1, 0) == PushResult::Overflow, "event bound");
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
  enqueue_event(q, emitted, 100, 0);
  enqueue_background(q, {body, 12, 8, origin, 6});
  TimerWheel wheel;
  wheel.now = 200;
  schedule(wheel, {65536, emitted, FactKey{body, 8}});
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
  require(fired && fired->threshold && *fired->threshold == FactKey{body, 8} &&
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
  const FactKey token{subject, 8};
  auto threshold = [&](Step due, FactKey version, std::uint64_t cause) {
    auto e = event(8);
    e.receiver = version.subject;
    e.origin = {cause, 12, {2, 7}, 3.5, false};
    return react::Timer{due, std::move(e), version};
  };
  schedule(wheel, threshold(70000, token, 40));
  auto sibling = token;
  sibling.subject = other;
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
  const FactKey first{subject, 8};
  auto timer = [&](Step due, FactKey token) {
    auto e = event(8);
    e.receiver = token.subject;
    e.origin.cause = due;
    return react::Timer{due, e, token};
  };
  TimerWheel identities;
  schedule(identities, timer(10, first));
  schedule(identities, timer(10, {Handle::make(subject.slot(), 2), 8}));
  schedule(identities, timer(10, {subject, 9}));
  require(timers(identities).size() == 3 && identities.thresholds.size() == 3,
          "threshold identity includes the full object generation and watched term");
  cancel_threshold(identities, first);
  require(timers(identities).size() == 2, "a rate with no crossing cancels only its watched fact");
  cancel_threshold(identities, first);
  snap::Buffer bytes;
  snap::Output out{bytes};
  write_timers(out, identities);
  bytes.resize(out.adapter().writtenBytesCount());
  TimerWheel restored;
  snap::Input in{bytes.data(), bytes.size()};
  read_timers(in, restored);
  snap::check_complete(in);
  require(timers(restored).size() == 2 && restored.thresholds.size() == 2,
          "snapshot restores only current pending keys");
  WorkQueues q;
  promote_due(restored, 10, q);
  unsigned fired = 0;
  while (auto item = pop_foreground(q)) {
    require(item->threshold && std::get<react::EventWork>(item->work).origin.cause == 10,
            "remaining exact identities retain their cause");
    ++fired;
  }
  require(fired == 2 && restored.thresholds.empty(), "cancelled key never wakes after restore");
}
void one_same_due_fixture_continues_after_snapshot() {
  TimerWheel uninterrupted;
  const auto subject = Handle::make(9, 2);
  FactKey token{subject, 4};
  auto make_threshold = [&](TermId id, FactKey version) {
    auto e = event(id);
    e.receiver = version.subject;
    e.origin.cause = id;
    return react::Timer{70000, e, version};
  };
  schedule(uninterrupted, make_threshold(20, token));
  auto sibling = token;
  ++sibling.term;
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
  FactKey token{subject, 6};
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
  native_action_input_contracts();
  queued_and_scheduled_work_records_when_it_was_issued();
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
