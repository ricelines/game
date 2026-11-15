#include "drive.hpp"

#include <algorithm>
#include <cmath>

namespace amber::drive {
Gate check_right(std::string_view right, Caller caller, const perm::Target &target,
                 const perm::Players &players, const perm::Grants &grants, Step now) {
  if (right.empty())
    return {true, {}};
  static constexpr std::array<std::string_view, 7> rights{"use",  "take",     "move",   "edit",
                                                          "copy", "transfer", "destroy"};
  const auto found = std::find(rights.begin(), rights.end(), right);
  if (found == rights.end())
    return {false, "unknown declared right"};
  const auto holder = std::visit(
      [](const auto &actor) -> perm::Holder {
        using T = std::decay_t<decltype(actor)>;
        if constexpr (std::is_same_v<T, PlayerId>)
          return perm::Holder::player(actor);
        else
          return perm::Holder::script(actor.script, actor.object);
      },
      caller);
  const auto decision = perm::check(holder, static_cast<perm::Verb>(found - rights.begin()), target,
                                    players, grants, now);
  return {bool(decision),
          decision ? std::string{} : std::string(perm::reason_text(decision.reason))};
}
bool in_reach(Vec3 point, Bounds target, float reach) {
  const auto x = std::clamp(point.x, target.minimum.x, target.maximum.x) - point.x;
  const auto y = std::clamp(point.y, target.minimum.y, target.maximum.y) - point.y;
  const auto z = std::clamp(point.z, target.minimum.z, target.maximum.z) - point.z;
  return x * x + y * y + z * z <= reach * reach;
}
bool valid_event(const EventTurn &turn, IdentityRead identity, const react::Processes &processes,
                 const react::Subscriptions &subscriptions) {
  if (turn.threshold)
    return react::valid_threshold(*turn.threshold, identity, processes, subscriptions);
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
                                          const step::EventWork &event, Step now, double hz) {
  std::vector<PlannedInvocation> plans;
  for (const auto &handler :
       react::dispatch(triggers, values, event.receiver, event.event, now, hz)) {
    if (event.recipient_script && event.recipient_script != handler.script)
      continue;
    bindings::InvocationInput input;
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
                                           const command::Resolution &resolution,
                                           const ActionRequest &request, Handle body, Handle held,
                                           const record::Origin &origin, Step now, double hz) {
  std::vector<PlannedInvocation> plans;
  if (!resolution || resolution.route == command::Route::Native)
    return plans;
  const auto *player = std::get_if<PlayerId>(&request.caller);
  if (!player)
    body = {};
  const auto sender = resolution.route == command::Route::Body ? request.target
                      : player ? body
                               : std::get<ScriptHolder>(request.caller).object;
  for (const auto &handler :
       react::dispatch(triggers, values, resolution.receiver, request.action, now, hz)) {
    bindings::InvocationInput input;
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
    if (player && resolution.route == command::Route::Body &&
        handler.script == resolution.declaration_script)
      input.direct_body =
          react::BodyEndpoints{body, request.target, held, *player, resolution.mechanics};
    plans.push_back({handler, std::move(input)});
  }
  return plans;
}
WatchPlan plan_watch(react::Subscriptions &subscriptions, install::WatchLinks &links,
                     const react::ValueRead &values, const react::AllocationRows &allocations,
                     const react::Triggers &triggers, const catalog::Scripts &scripts,
                     const step::WatchWork &work, Step now, double hz) {
  WatchPlan plan;
  plan.changes = install::refresh_dependencies(subscriptions, links, values, work.source, now, hz,
                                               work.origin);
  std::erase_if(plan.changes.dirty, [&](const auto &dirty) { return dirty.source == work.source; });
  plan.modeled =
      install::has_applicable_watch(subscriptions, triggers, scripts, values, work.source, now, hz);
  auto result =
      install::check_changed(subscriptions, values, allocations, work.source, now, hz, work.origin);
  plan.changes.timers.insert(plan.changes.timers.end(),
                             std::make_move_iterator(result.timers.begin()),
                             std::make_move_iterator(result.timers.end()));
  for (auto &delivery : install::plan_deliveries(result, triggers, scripts, values, now, hz)) {
    bindings::InvocationInput input;
    input.receiver = delivery.receiver;
    input.sender = delivery.sender;
    input.trigger = delivery.trigger;
    input.payload = std::move(delivery.payload);
    input.acquired = std::move(delivery.acquired);
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
WatchPlan plan_background(react::Subscriptions &subscriptions, install::WatchLinks &links,
                          const react::ValueRead &values, const react::AllocationRows &allocations,
                          const react::Triggers &triggers, const catalog::Scripts &scripts,
                          const react::RuleVersions &rules, const step::BackgroundItem &item,
                          Step now, double hz) {
  WatchPlan result;
  if (!install::catch_up_valid(item, rules, values, now, hz))
    return result;
  const auto &declaration = scripts.rows.at(item.script).declaration;
  for (const auto &watch : declaration.watches)
    if (watch.term == item.trigger)
      for (auto consumer :
           install::source_consumers(values, {item.receiver, item.trigger}, watch)) {
        const auto allowed =
            install::rule_watch_terms(rules, values, consumer, item.script, now, hz);
        if (std::find(allowed.begin(), allowed.end(), item.trigger) == allowed.end())
          continue;
        auto changes = install::sync_watches(
            subscriptions, links, values, allocations, {consumer, item.script}, declaration, now,
            hz, item.origin, install::SyncMode::Change, std::span<const TermId>(allowed));
        result.changes.dirty.insert(result.changes.dirty.end(), changes.dirty.begin(),
                                    changes.dirty.end());
        result.changes.timers.insert(result.changes.timers.end(), changes.timers.begin(),
                                     changes.timers.end());
      }
  auto changed =
      install::check_changed(subscriptions, values, allocations, {item.receiver, item.trigger}, now,
                             hz, item.origin, item.script);
  result.changes.timers.insert(result.changes.timers.end(), changed.timers.begin(),
                               changed.timers.end());
  for (auto &delivery : install::plan_deliveries(changed, triggers, scripts, values, now, hz)) {
    bindings::InvocationInput input;
    input.receiver = delivery.receiver;
    input.sender = delivery.sender;
    input.trigger = delivery.trigger;
    input.payload = std::move(delivery.payload);
    input.acquired = std::move(delivery.acquired);
    input.step = now;
    input.hz = hz;
    input.origin = item.origin;
    result.invocations.push_back({std::move(delivery.handler), std::move(input)});
  }
  return result;
}
} // namespace amber::drive
