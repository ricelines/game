#include "catalog/catalog.hpp"
#include "step.hpp"

#include <algorithm>
#include <cmath>

namespace amber::step {
bool in_reach(Vec3 point, Bounds target, float reach) {
  const auto x = std::clamp(point.x, target.minimum.x, target.maximum.x) - point.x;
  const auto y = std::clamp(point.y, target.minimum.y, target.maximum.y) - point.y;
  const auto z = std::clamp(point.z, target.minimum.z, target.maximum.z) - point.z;
  return x * x + y * y + z * z <= reach * reach;
}
bool valid_event(const EventTurn &turn, IdentityRead identity, const react::Processes &processes,
                 const react::Subscriptions &subscriptions, Step now, double hz) {
  if (turn.threshold)
    return react::valid_threshold(*turn.threshold, identity, processes, subscriptions, now, hz);
  return identity.valid(turn.event.receiver);
}
EventPayload normalize_arguments(const react::Terms &terms, TermId action, EventPayload args) {
  auto *fields = std::get_if<std::vector<EventField>>(&args);
  if (!fields)
    return args;
  for (auto &field : *fields) {
    const auto &inputs = terms.rows.at(action).inputs;
    auto input = std::find_if(inputs.begin(), inputs.end(),
                              [&](const auto &entry) { return entry.name == field.name; });
    if (input == inputs.end() || input->type != react::InputType::Term ||
        std::holds_alternative<TermRef>(field.value))
      continue;
    if (const auto *name = std::get_if<std::string>(&field.value))
      field.value = TermRef{terms.by_name.at(*name)};
    else if (const auto *value = std::get_if<Value>(&field.value); value && value->present())
      field.value = TermRef{static_cast<TermId>(value->numeric())};
  }
  return args;
}
std::vector<PlannedInvocation> plan_event(const react::Triggers &triggers,
                                          const react::ValueRead &values,
                                          const react::EventWork &event, Step now, double hz) {
  std::vector<PlannedInvocation> plans;
  for (const auto &handler :
       react::dispatch(triggers, values, event.receiver, event.event, now, hz)) {
    if (event.recipient_script && event.recipient_script != handler.script)
      continue;
    react::InvocationInput input;
    input.receiver = event.receiver;
    input.sender = event.sender;
    input.trigger = event.event;
    input.payload = event.payload;
    input.step = now;
    input.hz = hz;
    input.origin = event.origin;
    plans.push_back({handler, std::move(input)});
  }
  return plans;
}
std::vector<PlannedInvocation> plan_action(const react::Triggers &triggers,
                                           const react::ValueRead &values,
                                           const react::Resolution &resolution,
                                           const ActionRequest &request, Handle body,
                                           const amber::Origin &origin, Step now, double hz) {
  std::vector<PlannedInvocation> plans;
  if (!resolution || resolution.route == react::Route::Native)
    return plans;
  const auto *player = std::get_if<PlayerId>(&request.caller);
  if (!player)
    body = {};
  const auto sender = resolution.route == react::Route::Body ? request.target
                      : player ? body
                               : std::get<ScriptHolder>(request.caller).object;
  for (const auto &handler :
       react::dispatch(triggers, values, resolution.receiver, request.action, now, hz)) {
    react::InvocationInput input;
    input.receiver = resolution.receiver;
    input.sender = sender;
    input.trigger = request.action;
    input.payload = normalize_arguments(values.terms, request.action, request.args);
    input.step = now;
    input.hz = hz;
    input.origin = origin;
    input.action_caller = request.caller;
    input.action_body = body;
    input.action_target = request.target;
    plans.push_back({handler, std::move(input)});
  }
  return plans;
}
WatchPlan plan_watch(react::Subscriptions &subscriptions, const react::ValueRead &values,
                     const react::AllocationRows &allocations, const react::Triggers &triggers,
                     const catalog::Scripts &scripts, const react::WatchWork &work, Step now,
                     double hz) {
  WatchPlan plan;
  plan.changes =
      react::refresh_dependencies(subscriptions, values, work.source, now, hz, work.origin);
  std::erase_if(plan.changes.dirty, [&](const auto &dirty) { return dirty.source == work.source; });
  plan.modeled =
      react::has_applicable_watch(subscriptions, triggers, scripts, values, work.source, now, hz);
  auto result =
      react::check_changed(subscriptions, values, allocations, work.source, now, hz, work.origin);
  plan.changes.timers.insert(plan.changes.timers.end(),
                             std::make_move_iterator(result.timers.begin()),
                             std::make_move_iterator(result.timers.end()));
  for (auto &delivery : react::plan_deliveries(result, triggers, scripts, values, now, hz)) {
    react::InvocationInput input;
    input.receiver = delivery.receiver;
    input.sender = delivery.sender;
    input.trigger = delivery.trigger;
    input.payload = std::move(delivery.payload);
    input.acquired = std::move(delivery.acquired);
    input.watch = std::move(delivery.watch);
    input.step = now;
    input.hz = hz;
    input.origin = work.origin;
    plan.invocations.push_back({std::move(delivery.handler), std::move(input)});
  }
  if (!plan.modeled && values.identity.valid(work.source.subject) &&
      values.terms.rows.at(work.source.term).combine != react::Combine::None &&
      work.source.term < values.shared.size() &&
      values.shared[work.source.term].get(work.source.subject.slot()))
    plan.shared_gap = work.source;
  return plan;
}
WatchPlan plan_background(amber::react::State &react_state, const react::ValueRead &values,
                          const catalog::Scripts &scripts, const react::BackgroundItem &item,
                          Step now, double hz) {
  auto &subscriptions = react_state.subscriptions;
  auto &allocations = react_state.allocations;
  auto &triggers = react_state.triggers;
  auto &rules = react_state.rules;

  WatchPlan result;
  if (!react::catch_up_valid(item, rules, values, now, hz))
    return result;
  const auto &declaration = scripts.rows.at(item.script).declaration;
  for (const auto &watch : declaration.watches)
    if (watch.term == item.trigger)
      for (auto consumer : react::source_consumers(values, {item.receiver, item.trigger}, watch)) {
        const auto allowed = react::rule_watch_terms(rules, values, consumer, item.script, now, hz);
        if (std::find(allowed.begin(), allowed.end(), item.trigger) == allowed.end())
          continue;
        auto changes = react::sync_watches(
            react_state, values, {consumer, item.script}, declaration, now, hz, item.origin,
            react::SyncMode::Change, std::span<const TermId>(allowed));
        result.changes.dirty.insert(result.changes.dirty.end(), changes.dirty.begin(),
                                    changes.dirty.end());
        result.changes.timers.insert(result.changes.timers.end(), changes.timers.begin(),
                                     changes.timers.end());
      }
  auto changed =
      react::check_changed(subscriptions, values, allocations, {item.receiver, item.trigger}, now,
                           hz, item.origin, item.script);
  result.changes.timers.insert(result.changes.timers.end(), changed.timers.begin(),
                               changed.timers.end());
  for (auto &delivery : react::plan_deliveries(changed, triggers, scripts, values, now, hz)) {
    react::InvocationInput input;
    input.receiver = delivery.receiver;
    input.sender = delivery.sender;
    input.trigger = delivery.trigger;
    input.payload = std::move(delivery.payload);
    input.acquired = std::move(delivery.acquired);
    input.watch = std::move(delivery.watch);
    input.step = now;
    input.hz = hz;
    input.origin = item.origin;
    result.invocations.push_back({std::move(delivery.handler), std::move(input)});
  }
  return result;
}
std::optional<std::vector<PlannedInvocation>>
prepare_reactive(amber::react::State &react_state, const react::ValueRead &values,
                 const catalog::Scripts &scripts, amber::step::State &step_state,
                 ForegroundItem item, Step now, double hz) {
  auto &subscriptions = react_state.subscriptions;
  auto &allocations = react_state.allocations;
  auto &triggers = react_state.triggers;
  auto &queues = step_state.queues;
  auto &timers = step_state.timers;
  auto &gaps = step_state.gaps;

  if (auto *event = std::get_if<react::EventWork>(&item.work)) {
    EventTurn turn{std::move(*event), std::move(item.threshold)};
    if (!valid_event(turn, values.identity, values.processes, subscriptions, now, hz))
      return {};
    if (turn.threshold) {
      enqueue_watch(queues, {turn.threshold->source, turn.event.origin});
      return std::vector<PlannedInvocation>{};
    }
    return plan_event(triggers, values, turn.event, now, hz);
  }
  const auto &watch = std::get<react::WatchWork>(item.work);
  if (!values.identity.valid(watch.source.subject))
    return {};
  cancel_source_thresholds(timers, subscriptions, watch.source);
  auto planned = plan_watch(subscriptions, values, allocations, triggers, scripts, watch, now, hz);
  if (planned.shared_gap)
    gaps.push_back({planned.shared_gap->subject, planned.shared_gap->term, 0, 0, watch.origin});
  for (auto &dirty : planned.changes.dirty)
    enqueue_watch(queues, std::move(dirty));
  for (auto &timer : planned.changes.timers)
    schedule(timers, std::move(timer));
  return std::move(planned.invocations);
}
} // namespace amber::step
