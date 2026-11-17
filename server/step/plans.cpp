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
std::vector<react::PlannedInvocation> plan_event(const react::Triggers &triggers,
                                                 const react::ValueRead &values,
                                                 const react::EventWork &event, Step now,
                                                 double hz) {
  std::vector<react::PlannedInvocation> plans;
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
std::vector<react::PlannedInvocation>
plan_action(const react::Triggers &triggers, const react::ValueRead &values,
            const react::Resolution &resolution, const ActionRequest &request, Handle body,
            const amber::Origin &origin, Step now, double hz) {
  std::vector<react::PlannedInvocation> plans;
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
void refresh_rule_thresholds(TimerWheel &timers, const react::State &react_state,
                             const react::ValueRead &values, const catalog::Scripts &scripts,
                             std::span<const ScriptId> changed, const Clock &clock) {
  std::vector<TermId> terms;
  for (auto script : changed) {
    const auto &declaration = scripts.rows.at(script).declaration;
    if (declaration.kind == react::ScriptDeclaration::Kind::Rule)
      for (const auto &watch : declaration.watches)
        terms.push_back(watch.term);
  }
  if (terms.empty())
    return;
  std::vector<react::ThresholdChange> changes;
  for (const auto &[fact, location] : timers.thresholds)
    if (std::find(terms.begin(), terms.end(), fact.term) != terms.end()) {
      const auto &timer = timers.levels[location.level][location.bucket][location.index];
      changes.push_back({fact, react::threshold_timer(react_state, values, scripts, fact,
                                                      clock.step, clock.hz, timer.event.origin)});
    }
  apply_thresholds(timers, std::move(changes));
}

namespace {
void refresh_selector_thresholds(TimerWheel &timers, const react::State &react_state,
                                 const react::ValueRead &values, const catalog::Scripts &scripts,
                                 const react::WatchWork &work, Step now, double hz) {
  for (const auto &[term, trigger] : react_state.triggers) {
    if (term == work.source.term || term >= values.processes.size() ||
        !values.processes[term].get(work.source.subject.slot()) ||
        std::none_of(trigger.by_flag.begin(), trigger.by_flag.end(),
                     [&](const auto &entry) { return entry.first == work.source.term; }))
      continue;
    const FactKey fact{work.source.subject, term};
    auto origin = work.origin;
    if (const auto found = timers.thresholds.find(fact); found != timers.thresholds.end()) {
      const auto location = found->second;
      origin = timers.levels[location.level][location.bucket][location.index].event.origin;
    }
    if (auto timer = react::threshold_timer(react_state, values, scripts, fact, now, hz, origin))
      schedule(timers, std::move(*timer));
    else
      cancel_threshold(timers, fact);
  }
}
} // namespace

std::optional<std::vector<react::PlannedInvocation>>
prepare_reactive(react::State &react_state, const react::ValueRead &values,
                 const catalog::Scripts &scripts, step::State &step_state, ForegroundItem item,
                 Step now, double hz) {
  if (auto *event = std::get_if<react::EventWork>(&item.work)) {
    if (item.threshold) {
      if (step_state.timers.thresholds.contains(*item.threshold))
        return {};
      enqueue_watch(step_state.queues, {*item.threshold, event->origin});
      return std::vector<react::PlannedInvocation>{};
    }
    if (event->receiver && !values.identity.valid(event->receiver))
      return {};
    return plan_event(react_state.triggers, values, *event, now, hz);
  }
  const auto &watch = std::get<react::WatchWork>(item.work);
  if (!values.identity.valid(watch.source.subject))
    return {};
  auto planned = react::evaluate_watch(react_state, values, scripts, watch, now, hz);
  if (values.terms.rows.at(watch.source.term).kind == react::Kind::Flag)
    refresh_selector_thresholds(step_state.timers, react_state, values, scripts, watch, now, hz);
  if (!planned.modeled && values.terms.rows.at(watch.source.term).combine != react::Combine::None &&
      watch.source.term < values.shared.size() &&
      values.shared[watch.source.term].get(watch.source.subject.slot()))
    step_state.gaps.push_back({watch.source.subject, watch.source.term, 0, 0, watch.origin});
  apply_thresholds(step_state.timers, std::move(planned.thresholds));
  return std::move(planned.invocations);
}
} // namespace amber::step
