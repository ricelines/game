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
  case Status::Action:
    return "action";
  }
  std::terminate();
}
} // namespace
void start_invocation(Execution &execution, record::Journal &journal,
                      const react::HandlerRef &handler, InvocationInput input,
                      react::Invocation &invocation, CompletedInvocation &result) {
  result.handler = handler;
  result.receiver = input.receiver;
  result.trigger = input.trigger;
  result.step = input.step;
  if (!execution.identity.valid(input.receiver))
    return;
  invocation.self = {input.receiver, handler.script};
  invocation.step = input.step;
  invocation.hz = input.hz;
  invocation.trigger = input.trigger;
  invocation.sender = input.sender;
  invocation.payload = std::move(input.payload);
  invocation.acquired = std::move(input.acquired);
  acquire(invocation.acquired, input.sender);
  payload_handles(invocation.acquired, invocation.payload);
  acquire(invocation.event_targets, input.sender);
  payload_handles(invocation.event_targets, invocation.payload);
  invocation.action_caller = input.action_caller;
  invocation.action_body = input.action_body;
  invocation.action_target = input.action_target;
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
  invocation.deadline = input.deadline;
  invocation.state = &execution.states[invocation.self];
  const auto rng =
      execution.random
          .try_emplace(input.receiver.bits, execution.initial_seed ^ input.receiver.bits)
          .first;
  invocation.random = &rng->second;
  invocation.effects = &result.effects;
  acquire(result.things, invocation.self.object);
  acquire(result.things, invocation.sender);
  if (execution.credits.resolve) {
    result.object_credits =
        execution.credits.resolve(execution.credits.context, result.things, 0, input.hz);
    result.script_credits =
        execution.credits.resolve(execution.credits.context, {}, handler.script, input.hz);
  }
  record::Event event;
  event.step = input.step;
  event.kind = record::Kind::Handler;
  event.actor = record::Actor::script(handler.script);
  event.script_author = handler.author;
  event.name = handler.function;
  event.things = result.things;
  event.credits = result.object_credits;
  event.credits.insert(event.credits.end(), result.script_credits.begin(),
                       result.script_credits.end());
  event.detail["version"] = handler.version;
  result.origin = journal.append(std::move(event), invocation.origin);
  invocation.origin = result.origin;
  result.outcome = react::run_handler(execution.vm, handler, invocation);
}
void finish_invocation(Execution &execution, record::Journal &journal,
                       react::Invocation &invocation, CompletedInvocation &result) {
  acquire(result.things, invocation.self.object);
  acquire(result.things, invocation.sender);
  for (auto object : result.effects.consulted_handles)
    acquire(result.things, object);
  if (execution.credits.resolve) {
    result.object_credits =
        execution.credits.resolve(execution.credits.context, result.things, 0, invocation.hz);
    result.script_credits = execution.credits.resolve(execution.credits.context, {},
                                                      invocation.self.script, invocation.hz);
  }
  if (result.origin.cause) {
    auto credits = result.object_credits;
    credits.insert(credits.end(), result.script_credits.begin(), result.script_credits.end());
    result.origin = journal.finish(
        result.origin, result.effects.changed ? record::Result::Applied : record::Result::NoEffect,
        result.effects.changed, {}, credits);
    invocation.origin = result.origin;
  }
}
record::Origin publish_invocation(record::Journal &journal, step::WorkQueues &queues,
                                  step::TimerWheel &timers, CompletedInvocation &&invocation,
                                  const record::Origin &parent, std::size_t event_limit) {
  if (!invocation.origin.cause)
    return parent;
  auto origin = invocation.origin;
  auto things = nlohmann::json::array();
  for (auto object : invocation.things)
    things.push_back(to_string(object));
  auto detail = nlohmann::json{{"status", status(invocation.outcome.status)},
                               {"memory_bytes", invocation.outcome.memory_bytes},
                               {"things", std::move(things)}};
  if (!invocation.outcome.error.empty())
    detail["error"] = invocation.outcome.error;
  invocation.object_credits.insert(invocation.object_credits.end(),
                                   invocation.script_credits.begin(),
                                   invocation.script_credits.end());
  origin = journal.finish(
      origin, invocation.effects.changed ? record::Result::Applied : record::Result::NoEffect,
      invocation.effects.changed, std::move(detail), invocation.object_credits);
  react::publish_effects(std::move(invocation.effects), origin, invocation.step, queues, timers,
                         journal, event_limit);
  return origin;
}
} // namespace amber::bindings
