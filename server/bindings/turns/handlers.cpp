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
bindings::CompletedInvocation run_invocation(ActionTurns &actions,
                                             const drive::PlannedInvocation &plan,
                                             std::vector<Handle> *created) {
  auto &turns = actions.handlers;
  CompletedInvocation completed;
  react::Invocation invocation;
  start_invocation(turns.execution, turns.journal, plan.handler, plan.input, invocation, completed);
  while (completed.outcome.status == react::RunOutcome::Status::Action) {
    if (std::chrono::steady_clock::now() >= invocation.deadline) {
      completed.outcome = react::resume_handler(turns.execution.vm, invocation, {});
      break;
    }
    finish_invocation(turns.execution, turns.journal, invocation, completed);
    reconcile_effects(turns, completed.effects, completed.origin);
    react::DeferredEffects prefix;
    prefix.ordered.swap(completed.effects.ordered);
    prefix.background.swap(completed.effects.background);
    react::publish_effects(std::move(prefix), completed.origin, plan.input.step, turns.queues,
                           turns.timers, turns.journal, turns.event_limit);
    auto action = std::move(*completed.outcome.action);
    const auto result =
        perform_action(actions, std::move(action.request), std::move(action.arguments),
                       completed.origin, {}, invocation.deadline);
    completed.effects.changed |= result.reply.value("effect", "") == "applied";
    if (created && result.reply.contains("created"))
      for (const auto &value : result.reply.at("created"))
        if (auto h = parse_handle(value.get<std::string>());
            h && std::ranges::find(*created, *h) == created->end())
          created->push_back(*h);
    const auto state = turns.execution.states.find(invocation.self);
    if (!turns.values.identity.valid(invocation.self.object) ||
        state == turns.execution.states.end() || &state->second != invocation.state) {
      invocation.state = nullptr;
      invocation.random = nullptr;
    }
    completed.outcome = react::resume_handler(turns.execution.vm, invocation, result.reply);
  }
  finish_invocation(turns.execution, turns.journal, invocation, completed);
  reconcile_effects(turns, completed.effects, completed.origin);
  return completed;
}
void consume_event(void *context, drive::EventTurn turn) {
  auto &actions = *static_cast<ActionTurns *>(context);
  auto &handlers = actions.handlers;
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
                       run_invocation(actions, plan), turn.event.origin, handlers.event_limit);
}
void consume_watch(void *context, step::WatchWork work) {
  auto &actions = *static_cast<ActionTurns *>(context);
  auto &handlers = actions.handlers;
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
                       run_invocation(actions, invocation), work.origin, handlers.event_limit);
  if (plan.shared_gap)
    actions.gaps.push_back({plan.shared_gap->subject, plan.shared_gap->term, 0, 0, work.origin});
}
void consume_background(void *context, step::BackgroundItem work) {
  auto &actions = *static_cast<ActionTurns *>(context);
  auto &handlers = actions.handlers;
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
                       run_invocation(actions, invocation), work.origin, handlers.event_limit);
}
} // namespace amber::bindings
