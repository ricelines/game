#include "turns.hpp"

#include <algorithm>

namespace amber::bindings {
void reconcile_effects(HandlerTurns &turns, react::DeferredEffects &effects,
                       const record::Origin &origin) {
  reconcile_allocations(turns.values, turns.allocation, effects, turns.clock.step, turns.hz);
  if (turns.mass) {
    std::vector<FactKey> facts;
    for (const auto &effect : effects.ordered)
      if (const auto *watch = std::get_if<step::WatchWork>(&effect))
        facts.push_back(watch->source);
    reconcile_mass(*turns.mass, facts, turns.clock.step, turns.hz, origin, effects);
  }
  auto changes = install::sync_rule_watches(turns.selectors.subscriptions, turns.selectors.links,
                                            turns.values, turns.allocation.allocations,
                                            turns.selectors.scripts, turns.selectors.rules,
                                            effects.ordered, turns.clock.step, turns.hz, origin);
  for (auto &dirty : changes.dirty)
    effects.ordered.emplace_back(std::move(dirty));
  for (auto &timer : changes.timers)
    effects.ordered.emplace_back(std::move(timer));
  for (const auto &effect : effects.ordered)
    if (const auto *watch = std::get_if<step::WatchWork>(&effect))
      turns.mutated.push_back(watch->source);
}
bindings::CompletedInvocation run_invocation(HandlerTurns &turns,
                                             const drive::PlannedInvocation &plan,
                                             std::vector<Handle> *created) {
  auto result = drive::execute(turns.execution, plan, turns.structural);
  if (created)
    created->insert(created->end(), result.created.begin(), result.created.end());
  auto &completed = result.invocation;
  for (auto object : result.created)
    if (std::find(completed.things.begin(), completed.things.end(), object) ==
        completed.things.end())
      completed.things.push_back(object);
  const auto &credits = turns.execution.credits;
  if (credits.resolve && !result.created.empty()) {
    auto extra = credits.resolve(credits.context, result.created, 0, turns.hz);
    completed.object_credits.insert(completed.object_credits.end(), extra.begin(), extra.end());
  }
  reconcile_effects(turns, completed.effects, plan.input.origin);
  return std::move(completed);
}
void consume_event(void *context, drive::EventTurn turn) {
  auto &handlers = *static_cast<HandlerTurns *>(context);
  if (!drive::valid_event(turn, handlers.values.identity, handlers.values.processes,
                          handlers.selectors.subscriptions))
    return;
  if (turn.threshold) {
    step::enqueue_watch(handlers.queues, {turn.threshold->source, turn.event.origin});
    return;
  }
  auto plans = drive::plan_event(handlers.triggers, handlers.values, turn.event,
                                 handlers.clock.step, handlers.hz);
  for (const auto &plan : plans)
    publish_invocation(handlers.journal, handlers.queues, handlers.timers,
                       run_invocation(handlers, plan), turn.event.origin, handlers.event_limit);
}
void consume_watch(void *context, step::WatchWork work) {
  auto &turns = *static_cast<WatchTurns *>(context);
  auto &handlers = turns.handlers;
  if (!handlers.values.identity.valid(work.source.subject) &&
      !handlers.selectors.subscriptions.rows.contains(work.source))
    return;
  auto plan = drive::plan_watch(handlers.selectors.subscriptions, handlers.selectors.links,
                                handlers.values, handlers.allocation.allocations, handlers.triggers,
                                handlers.selectors.scripts, work, handlers.clock.step, handlers.hz);
  for (auto &dirty : plan.changes.dirty)
    step::enqueue_watch(handlers.queues, std::move(dirty));
  for (auto &timer : plan.changes.timers)
    step::schedule(handlers.timers, std::move(timer));
  for (const auto &invocation : plan.invocations)
    publish_invocation(handlers.journal, handlers.queues, handlers.timers,
                       run_invocation(handlers, invocation), work.origin, handlers.event_limit);
  if (plan.shared_gap)
    turns.gaps.push_back({plan.shared_gap->subject, plan.shared_gap->term, 0, 0, work.origin});
}
void consume_background(void *context, step::BackgroundItem work) {
  auto &handlers = *static_cast<HandlerTurns *>(context);
  auto plan = drive::plan_background(
      handlers.selectors.subscriptions, handlers.selectors.links, handlers.values,
      handlers.allocation.allocations, handlers.triggers, handlers.selectors.scripts,
      handlers.selectors.rules, work, handlers.clock.step, handlers.hz);
  for (auto &dirty : plan.changes.dirty)
    step::enqueue_watch(handlers.queues, std::move(dirty));
  for (auto &timer : plan.changes.timers)
    step::schedule(handlers.timers, std::move(timer));
  for (const auto &invocation : plan.invocations)
    publish_invocation(handlers.journal, handlers.queues, handlers.timers,
                       run_invocation(handlers, invocation), work.origin, handlers.event_limit);
}
} // namespace amber::bindings
