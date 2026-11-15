#include "controls.hpp"
#include "bindings/viewing/viewing.hpp"

#include <algorithm>
#include <stdexcept>

namespace amber::bindings {
namespace {
Handle handle(const nlohmann::json &value, Handle body) {
  const auto &text = value.get_ref<const std::string &>();
  if (text == "self")
    return body;
  const auto object = parse_handle(text);
  if (!object)
    throw std::invalid_argument("expected a complete object handle");
  return *object;
}
EventAtom atom(const react::Input &input, const nlohmann::json &value, Handle body) {
  if (value.is_null())
    return Value{};
  switch (input.type) {
  case react::InputType::Handle:
    return Value(handle(value, body));
  case react::InputType::Vector: {
    if (!value.is_array() || value.size() != 3)
      throw std::invalid_argument("expected a three-component vector");
    const auto v = value.get<std::array<float, 3>>();
    return Vec3{v[0], v[1], v[2]};
  }
  case react::InputType::String:
    return value.get<std::string>();
  case react::InputType::Boolean:
    return Value(value.get<bool>());
  case react::InputType::Term:
    if (value.is_string())
      return value.get<std::string>();
    return Value(value.get<double>());
  case react::InputType::Integer:
    if (value.is_number_integer())
      return Value(value.get<std::int64_t>());
    return Value(value.get<double>());
  case react::InputType::Number:
    return Value(value.get<double>());
  }
  std::terminate();
}
} // namespace
EventPayload decode_arguments(const react::Term &action, const nlohmann::json &arguments,
                              Handle body) {
  if (!arguments.is_object())
    throw std::invalid_argument("action args must be an input record");
  std::vector<EventField> fields;
  fields.reserve(arguments.size());
  for (const auto &[key, value] : arguments.items()) {
    const auto input = std::find_if(action.inputs.begin(), action.inputs.end(),
                                    [&](const auto &input) { return input.name == key; });
    if (input == action.inputs.end())
      throw std::invalid_argument("unknown action input: " + key);
    try {
      fields.push_back({key, atom(*input, value, body)});
    } catch (const std::exception &error) {
      throw std::invalid_argument(key + ": " + error.what());
    }
  }
  return fields;
}
control::Reply queue_control(void *context, const control::Invocation &call) {
  auto &binding = *static_cast<QueuedControl *>(context);
  drive::Input input;
  input.operation = call.request.operation;
  input.caller = call.principal.player;
  input.body = call.principal.body;
  input.session = call.session;
  input.sequence = call.sequence;
  input.request = call.request.id;
  input.fields = call.request.fields;
  const auto work = drive::enqueue_input(binding.inputs, std::move(input));
  step::append_inputs(binding.queues, {work});
  return control::Reply::pending();
}
void consume_input(void *context, step::InputWork work) {
  auto &delivery = *static_cast<InputDelivery *>(context);
  const auto completed = drive::invoke_input(delivery.inputs, delivery.operations, work);
  if (completed)
    delivery.sessions.complete(completed->session, completed->sequence, completed->fields);
}
drive::InputReply do_input(void *context, const drive::Input &input) {
  auto &binding = *static_cast<DoInput *>(context);
  auto &turns = binding.actions;
  nlohmann::json reply;
  const auto name = input.fields.at("action").get<std::string>();
  const auto args = input.fields.value("args", nlohmann::json::object());
  ActionRequest request;
  request.caller = input.caller;
  TermId action = 0;
  try {
    const auto found = turns.handlers.values.terms.by_name.find(name);
    if (found == turns.handlers.values.terms.by_name.end())
      throw std::invalid_argument("unknown registered action: " + name);
    action = found->second;
    request.action = found->second;
    if (input.fields.contains("target"))
      request.target = handle(input.fields.at("target"), input.body);
    if (!turns.declarations.rows.contains(request.action))
      request.args =
          decode_arguments(turns.handlers.values.terms.rows[request.action], args, input.body);
  } catch (const std::exception &error) {
    reply = {{"ok", false}, {"error", "bad_input"}, {"problem", error.what()}};
    if (action && turns.handlers.values.terms.rows[action].kind == react::Kind::Action)
      reply["signature"] =
          command::action_signature(turns.handlers.values.terms, turns.declarations, action);
    record::Event event;
    event.step = turns.handlers.clock.step;
    event.kind = record::Kind::Action;
    event.actor = record::Actor::player(std::get<PlayerId>(input.caller));
    event.name = name;
    event.result = record::Result::Denied;
    event.detail = {{"args", args}, {"agent", true}, {"response", reply}};
    if (input.fields.contains("target"))
      event.detail["raw_target"] = input.fields.at("target");
    turns.handlers.journal.append(std::move(event), input.origin);
    viewing::append_heard(reply, binding.hearing, input.session,
                          input.fields.value("events_limit", binding.events_per_response));
    return {false, std::move(reply)};
  }
  auto completed =
      perform_action(turns, std::move(request), args, input.origin,
                     drive::InputOrder{input.id, input.session, input.sequence, input.request});
  if (completed.deferred)
    return {true, std::move(completed.reply), std::move(completed.origin)};
  reply = std::move(completed.reply);
  viewing::append_heard(reply, binding.hearing, input.session,
                        input.fields.value("events_limit", binding.events_per_response));
  return {false, std::move(reply)};
}
drive::InputReply cancel_input(void *context, const drive::Input &input) {
  auto &binding = *static_cast<CancelInput *>(context);
  const auto ending = step::cancel_move(binding.intents, input.body);
  if (ending)
    binding.endings.push_back(*ending);
  record::Event event;
  event.step = binding.clock.step;
  event.kind = record::Kind::Action;
  event.actor = record::Actor::player(std::get<PlayerId>(input.caller));
  event.name = "cancel";
  event.result = record::Result::NoEffect;
  event.things = {input.body};
  binding.journal.append(std::move(event), input.origin);
  nlohmann::json reply = {
      {"ok", true}, {"effect", "none"}, {"reason", ending ? "cancelled" : "no_intent"}};
  viewing::append_heard(reply, binding.hearing, input.session,
                        input.fields.value("events_limit", binding.events_per_response));
  return {false, std::move(reply)};
}
} // namespace amber::bindings
