#include "execution.hpp"

#include <algorithm>
#include <stdexcept>

namespace amber::bindings {
namespace {
void acquire(std::vector<Handle> &handles, Handle object) {
  if (object && std::find(handles.begin(), handles.end(), object) == handles.end())
    handles.push_back(object);
}
void payload_handles(std::vector<Handle> &handles, const EventPayload &payload) {
  if (const auto *value = std::get_if<Value>(&payload)) {
    if (value->kind == Value::Kind::Object)
      acquire(handles, value->object);
  } else if (const auto *fields = std::get_if<std::vector<EventField>>(&payload))
    for (const auto &field : *fields)
      if (const auto *v = std::get_if<Value>(&field.value); v && v->kind == Value::Kind::Object)
        acquire(handles, v->object);
}
std::string_view status(react::RunOutcome::Status status) {
  using Status = react::RunOutcome::Status;
  switch (status) {
  case Status::Ok:
    return "ok";
  case Status::Error:
    return "error";
  case Status::Cutoff:
    return "cutoff";
  case Status::Missing:
    return "missing";
  }
  std::terminate();
}
} // namespace
CompletedInvocation execute(Execution &execution, const react::HandlerRef &handler,
                            InvocationInput input) {
  CompletedInvocation result;
  result.handler = handler;
  result.receiver = input.receiver;
  result.trigger = input.trigger;
  result.step = input.step;
  if (!execution.identity.valid(input.receiver))
    return result;
  react::Invocation invocation;
  invocation.self = {input.receiver, handler.script};
  invocation.step = input.step;
  invocation.hz = input.hz;
  invocation.trigger = input.trigger;
  invocation.sender = input.sender;
  invocation.payload = std::move(input.payload);
  invocation.acquired = std::move(input.acquired);
  acquire(invocation.acquired, input.sender);
  payload_handles(invocation.acquired, invocation.payload);
  invocation.action_caller = input.action_caller;
  invocation.action_body = input.action_body;
  invocation.action_target = input.action_target;
  invocation.direct_body = std::move(input.direct_body);
  if (invocation.direct_body) {
    acquire(invocation.acquired, invocation.direct_body->selected);
    acquire(invocation.acquired, invocation.direct_body->held);
  }
  auto append_parts = [&](Handle parent) {
    if (const auto *children = execution.declared_parts.out.get(parent.slot()))
      for (auto child : *children)
        if (execution.identity.valid(child))
          invocation.parts.push_back(child);
  };
  append_parts(input.receiver);
  for (std::size_t i = 0; i < invocation.parts.size(); ++i)
    append_parts(invocation.parts[i]);
  invocation.origin = std::move(input.origin);
  invocation.state = &execution.states[invocation.self];
  const auto rng =
      execution.random
          .try_emplace(input.receiver.bits, execution.initial_seed ^ input.receiver.bits)
          .first;
  invocation.random = &rng->second;
  invocation.effects = &result.effects;
  result.outcome = react::run_handler(execution.vm, handler, invocation);
  acquire(result.things, input.receiver);
  acquire(result.things, input.sender);
  for (auto object : result.effects.consulted_handles)
    acquire(result.things, object);
  if (execution.credits.resolve) {
    result.object_credits =
        execution.credits.resolve(execution.credits.context, result.things, 0, input.hz);
    result.script_credits =
        execution.credits.resolve(execution.credits.context, {}, handler.script, input.hz);
  }
  return result;
}
record::Origin publish_invocation(record::Journal &journal, step::WorkQueues &queues,
                                  step::TimerWheel &timers, CompletedInvocation &&invocation,
                                  const record::Origin &parent, std::size_t event_limit) {
  if (!invocation.effects.structural.empty())
    throw std::logic_error("apply structural requests before finalizing handler records");
  if (invocation.outcome.status == react::RunOutcome::Status::Missing)
    return parent;
  record::Event event;
  event.step = invocation.step;
  event.kind = record::Kind::Handler;
  event.actor = record::Actor::script(invocation.handler.script);
  event.script_author = invocation.handler.author;
  event.name = invocation.handler.function;
  event.things = invocation.things.empty() ? std::vector{invocation.receiver} : invocation.things;
  event.credits = invocation.object_credits;
  event.credits.insert(event.credits.end(), invocation.script_credits.begin(),
                       invocation.script_credits.end());
  event.changed = invocation.effects.changed;
  event.result = event.changed ? record::Result::Applied : record::Result::NoEffect;
  event.detail = {{"status", status(invocation.outcome.status)},
                  {"memory_bytes", invocation.outcome.memory_bytes},
                  {"version", invocation.handler.version}};
  if (!invocation.outcome.error.empty())
    event.detail["error"] = invocation.outcome.error;
  const auto origin = journal.append(std::move(event), parent);
  for (auto &structural : invocation.structural_records)
    journal.append(std::move(structural), origin);
  react::publish_effects(std::move(invocation.effects), origin, invocation.step, queues, timers,
                         journal, event_limit);
  return origin;
}
ActionCompletion publish_action(record::Journal &journal, step::WorkQueues &queues,
                                step::TimerWheel &timers, record::Event action,
                                std::vector<CompletedInvocation> invocations,
                                const record::Origin &parent,
                                std::span<const record::Credit> credits, std::size_t event_limit) {
  ActionCompletion completed;
  for (const auto &invocation : invocations) {
    if (!invocation.effects.structural.empty())
      throw std::logic_error("apply structural requests before finalizing an action");
    if (invocation.outcome.status != react::RunOutcome::Status::Missing)
      ++completed.handlers;
    completed.changed |= invocation.effects.changed;
    action.credits.insert(action.credits.end(), invocation.object_credits.begin(),
                          invocation.object_credits.end());
    for (auto object : invocation.things)
      acquire(action.things, object);
    if (!invocation.outcome.error.empty())
      completed.errors.push_back(invocation.outcome.error);
  }
  action.changed = completed.changed;
  action.result = completed.changed        ? record::Result::Applied
                  : completed.handlers > 0 ? record::Result::NoEffect
                                           : record::Result::NotModeled;
  completed.origin = journal.append(std::move(action), parent, credits);
  for (auto &invocation : invocations)
    publish_invocation(journal, queues, timers, std::move(invocation), completed.origin,
                       event_limit);
  return completed;
}
} // namespace amber::bindings
