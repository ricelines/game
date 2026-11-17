#include "action_private.hpp"
#include "catalog/catalog.hpp"
namespace amber::step {
ExecutionFrame *advance_execution(std::deque<ExecutionFrame> &execution, react::State &react_state,
                                  const ObjectState &objects_state,
                                  const catalog::State &catalog_state, record::Journal &journal) {
  while (!execution.empty()) {
    if (auto *action = std::get_if<ActionRun>(&execution.back())) {
      if (action->outcome || action->resolution.route == react::Route::Native ||
          action->next_handler == action->handlers.size())
        return &execution.back();
      auto plan = std::move(action->handlers[action->next_handler++]);
      execution.emplace_back(std::in_place_type<HandlerRun>);
      std::get<HandlerRun>(execution.back()).plan = std::move(plan);
    }
    auto &handler = std::get<HandlerRun>(execution.back());
    if (!handler.started) {
      handler.started = true;
      if (!react::start_invocation(react_state, objects_state, 0, catalog_state, journal,
                                   handler.plan.handler, std::move(handler.plan.input),
                                   handler.invocation, handler.completed)) {
        execution.pop_back();
        continue;
      }
    }
    react::finish_invocation(identity(objects_state.objects), attribution(objects_state.objects),
                             catalog_state.models, catalog_state.scripts, journal,
                             handler.invocation, handler.completed);
    return &execution.back();
  }
  return nullptr;
}

std::optional<std::pair<InputOrder, ActionOutcome>>
return_action(std::deque<ExecutionFrame> &execution, react::Vm &vm, IdentityRead identity,
              const Relation &declared_parts) {
  auto &action = std::get<ActionRun>(execution.back());
  auto outcome = std::move(*action.outcome);
  const auto order = action.call.order;
  const bool changed = action.changed;
  auto created = std::move(action.created);
  execution.pop_back();
  if (execution.empty()) {
    if (order)
      return std::pair{*order, std::move(outcome)};
  } else {
    auto &handler = std::get<HandlerRun>(execution.back());
    handler.completed.effects.changed |= changed;
    if (execution.size() > 1)
      if (auto *parent = std::get_if<ActionRun>(&execution[execution.size() - 2]))
        parent->created.insert(parent->created.end(), created.begin(), created.end());
    react::refresh_parts(handler.invocation, identity, declared_parts);
    handler.completed.outcome = react::resume_handler(vm, handler.invocation, outcome.reply);
  }
  return {};
}

std::optional<NativeCall> finish_handler_segment(std::deque<ExecutionFrame> &execution,
                                                 record::Journal &journal, WorkQueues &queues,
                                                 TimerWheel &timers, Step now, double hz,
                                                 std::size_t event_limit) {
  auto &handler = std::get<HandlerRun>(execution.back());
  if (handler.completed.outcome.status == react::RunOutcome::Status::Action) {
    react::DeferredEffects prefix;
    prefix.ordered.swap(handler.completed.effects.ordered);
    prefix.background.swap(handler.completed.effects.background);
    publish_effects(std::move(prefix), handler.completed.origin, now, queues, timers, journal,
                    event_limit);
    auto yielded = std::move(*handler.completed.outcome.action);
    return NativeCall{std::move(yielded.request),
                      std::move(yielded.arguments),
                      {},
                      now,
                      hz,
                      handler.completed.origin,
                      {}};
  }
  if (execution.size() > 1)
    if (auto *action = std::get_if<ActionRun>(&execution[execution.size() - 2]))
      accept_handler(*action, handler.completed);
  const auto origin = handler.completed.origin;
  publish_invocation(journal, queues, timers, std::move(handler.completed), origin, event_limit);
  execution.pop_back();
  return {};
}

void invalidate_execution(std::deque<ExecutionFrame> &execution, IdentityRead identity,
                          const react::ScriptStates &states) {
  // Invalidate every suspended ancestor before a later nested call can install
  // a fresh state row for the same object and script.
  for (auto &frame : execution)
    if (auto *handler = std::get_if<HandlerRun>(&frame); handler && handler->started) {
      auto &invocation = handler->invocation;
      if (invocation.state &&
          ((invocation.self.object && !identity.valid(invocation.self.object)) ||
           !states.contains(invocation.self))) {
        invocation.state = nullptr;
        invocation.random = nullptr;
      }
    }
}

void reconcile_effects(const react::ValueRead &values, amber::react::State &react_state,
                       amber::step::State &step_state, const catalog::Scripts &scripts,
                       const amber::step::Clock &clock, react::DeferredEffects &effects,
                       const amber::Origin &origin) {
  const auto now = clock.step;
  const auto hz = clock.hz;

  auto &mutated = step_state.mutated;

  react::reconcile_allocations(values, react_state, effects, now, hz);
  auto changes =
      react::sync_rule_watches(react_state, values, scripts, effects.ordered, now, hz, origin);
  for (auto &dirty : changes.dirty)
    effects.ordered.emplace_back(std::move(dirty));
  for (auto &timer : changes.timers)
    effects.ordered.emplace_back(std::move(timer));
  for (const auto &effect : effects.ordered)
    if (const auto *watch = std::get_if<react::WatchWork>(&effect))
      mutated.push_back(watch->source);
}
} // namespace amber::step
