#include "react.hpp"

#include <algorithm>
#include <stdexcept>

namespace amber::react {
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
} // namespace
void refresh_parts(Invocation &invocation, IdentityRead identity, const Relation &declared_parts) {
  invocation.parts.clear();
  if (!identity.valid(invocation.self.object))
    return;
  auto append = [&](Handle parent) {
    if (const auto *children = declared_parts.out.get(parent.slot()))
      for (auto child : *children)
        if (identity.valid(child))
          invocation.parts.push_back(child);
  };
  append(invocation.self.object);
  for (std::size_t i = 0; i < invocation.parts.size(); ++i)
    append(invocation.parts[i]);
}
bool start_invocation(Vm &vm, ScriptStates &states, RandomStates &random, const ValueRead &values,
                      const Triggers &triggers, const Relation &declared_parts,
                      std::uint64_t initial_seed, AttributionRead attribution,
                      const catalog::Models &models, const catalog::Scripts &scripts,
                      record::Journal &journal, const react::HandlerRef &handler,
                      InvocationInput input, react::Invocation &invocation,
                      CompletedInvocation &result) {
  result.handler = handler;
  result.receiver = input.receiver;
  result.trigger = input.trigger;
  result.step = input.step;
  const auto identity = values.identity;
  // A prior handler may have changed this object's installation while this
  // candidate waited. Eligibility itself must not recreate retired state.
  const auto current =
      dispatch(triggers, values, input.receiver, input.trigger, input.step, input.hz);
  if (std::none_of(current.begin(), current.end(), [&](const HandlerRef &candidate) {
        return candidate.script == handler.script && candidate.function == handler.function &&
               candidate.version == handler.version;
      }))
    return false;
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
  refresh_parts(invocation, identity, declared_parts);
  invocation.origin = std::move(input.origin);
  invocation.deadline = input.deadline;
  invocation.state = &states[invocation.self];
  const auto rng =
      random.try_emplace(input.receiver.bits, initial_seed ^ input.receiver.bits).first;
  invocation.random = &rng->second;
  invocation.effects = &result.effects;
  acquire(result.things, invocation.self.object);
  acquire(result.things, invocation.sender);
  result.object_credits =
      record::credits(identity, attribution, models, scripts, result.things, 0, input.hz);
  result.script_credits =
      record::credits(identity, attribution, models, scripts, {}, handler.script, input.hz);
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
  result.outcome = react::run_handler(vm, handler, invocation);
  return true;
}
void finish_invocation(IdentityRead identity, AttributionRead attribution,
                       const catalog::Models &models, const catalog::Scripts &scripts,
                       record::Journal &journal, react::Invocation &invocation,
                       CompletedInvocation &result) {
  acquire(result.things, invocation.self.object);
  acquire(result.things, invocation.sender);
  for (auto object : result.effects.consulted_handles)
    acquire(result.things, object);
  result.object_credits =
      record::credits(identity, attribution, models, scripts, result.things, 0, invocation.hz);
  result.script_credits = record::credits(identity, attribution, models, scripts, {},
                                          invocation.self.script, invocation.hz);
  if (result.origin.cause) {
    auto credits = result.object_credits;
    credits.insert(credits.end(), result.script_credits.begin(), result.script_credits.end());
    result.origin = journal.finish(
        result.origin, result.effects.changed ? record::Result::Applied : record::Result::NoEffect,
        result.effects.changed, {}, credits);
    invocation.origin = result.origin;
  }
}
} // namespace amber::react
