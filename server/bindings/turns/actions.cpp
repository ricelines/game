#include "observe/observe.hpp"
#include "turns.hpp"

#include <algorithm>

namespace amber::bindings {
namespace {
std::vector<record::Credit> credits(const ActionTurns &, Handle);
record::Event action_record(const ActionTurns &turns, const ActionRequest &request) {
  const auto &handlers = turns.handlers;
  record::Event event;
  event.step = handlers.clock.step;
  event.kind = record::Kind::Action;
  if (const auto *player = std::get_if<PlayerId>(&request.caller))
    event.actor = record::Actor::player(*player);
  else {
    const auto script = std::get<ScriptHolder>(request.caller).script;
    event.actor = record::Actor::script(script);
    if (script && script < turns.scripts.rows.size())
      event.script_author = turns.scripts.rows[script].author;
  }
  if (request.action && request.action < handlers.values.terms.rows.size())
    event.name = handlers.values.terms.rows[request.action].name;
  if (request.target)
    event.things.push_back(request.target);
  event.detail = {{"args", observe::event_payload_json(request.args)},
                  {"agent", std::holds_alternative<PlayerId>(request.caller)}};
  return event;
}
ActionOutcome reject(ActionTurns &turns, const ActionRequest &request, const record::Origin &parent,
                     nlohmann::json reply, const nlohmann::json &arguments) {
  auto event = action_record(turns, request);
  event.detail["args"] = arguments;
  event.credits = credits(turns, request.target);
  event.result = record::Result::Denied;
  event.detail["response"] = reply;
  const auto origin = turns.handlers.journal.append(std::move(event), parent);
  return {std::move(reply), origin};
}
Handle held(const ActionTurns &turns, Handle body) {
  if (const auto *attached = turns.grasped.in.get(body.slot()))
    for (auto object : *attached)
      if (turns.handlers.values.identity.valid(object) &&
          (turns.handlers.values.identity.flags[object.slot()] & Held) &&
          !(turns.handlers.values.identity.flags[object.slot()] & Body))
        return object;
  return {};
}
std::vector<record::Credit> credits(const ActionTurns &turns, Handle target) {
  std::vector<record::Credit> result;
  if (turns.handlers.values.identity.valid(target)) {
    const auto &resolver = turns.handlers.execution.credits;
    if (resolver.resolve)
      return resolver.resolve(resolver.context, std::span{&target, 1}, 0, turns.handlers.hz);
    const auto model = turns.handlers.values.identity.model[target.slot()];
    result.push_back({turns.models.rows[model].author, turns.models.rows[model].created_s, false});
  }
  return result;
}
bool installed_caller(const ActionTurns &turns, ScriptHolder caller) {
  if (!turns.handlers.values.identity.valid(caller.object) || !caller.script ||
      caller.script >= turns.scripts.rows.size() ||
      !turns.scripts.rows[caller.script].declaration.active)
    return false;
  auto scripts = install::installed_scripts(turns.models, turns.instances,
                                            turns.handlers.values.identity, caller.object);
  if (std::find(scripts.begin(), scripts.end(), caller.script) != scripts.end())
    return true;
  scripts = install::rule_scripts(turns.rules, turns.handlers.values, caller.object,
                                  turns.handlers.clock.step, turns.handlers.hz);
  return std::find(scripts.begin(), scripts.end(), caller.script) != scripts.end();
}
} // namespace
ActionOutcome perform_action(ActionTurns &turns, ActionRequest request,
                             nlohmann::json native_arguments, const record::Origin &parent,
                             std::optional<drive::InputOrder> order) {
  auto &handlers = turns.handlers;
  const auto identity = handlers.values.identity;
  Handle body;
  const auto signature = [&](nlohmann::json &reply) {
    if (reply.value("error", "") == "bad_input" && request.action &&
        request.action < handlers.values.terms.rows.size() &&
        handlers.values.terms.rows[request.action].kind == react::Kind::Action)
      reply["signature"] =
          command::action_signature(handlers.values.terms, turns.declarations, request.action);
  };
  const auto deny = [&](nlohmann::json reply) {
    signature(reply);
    return reject(turns, request, parent, std::move(reply), native_arguments);
  };
  if (const auto *player = std::get_if<PlayerId>(&request.caller)) {
    if (!*player || *player >= turns.profiles.rows.size() ||
        turns.profiles.rows[*player].id != *player)
      return deny({{"ok", false}, {"error", "unknown_player"}});
    body = turns.profiles.rows[*player].body;
  } else if (!installed_caller(turns, std::get<ScriptHolder>(request.caller)))
    return deny({{"ok", false}, {"error", "inactive_script_holder"}});
  const auto resolution = command::resolve(turns.declarations, handlers.values.terms, turns.models,
                                           turns.scripts, turns.instances, identity, request, body);
  if (!resolution) {
    constexpr const char *failures[]{"none", "unknown_action", "stale_target", "missing_target"};
    return deny({{"ok", false},
                 {"error", "bad_input"},
                 {"problem", failures[static_cast<unsigned>(resolution.failure)]}});
  }
  if (resolution.route != command::Route::Native) {
    const auto errors = command::validate_arguments(
        handlers.values.terms, handlers.values.terms.rows[request.action], request.args);
    if (!errors.empty()) {
      std::string problem;
      for (const auto &error : errors) {
        if (!problem.empty())
          problem += "; ";
        problem += error.input + ": " + error.problem;
      }
      return deny({{"ok", false}, {"error", "bad_input"}, {"problem", std::move(problem)}});
    }
  }
  if (!resolution.right.empty()) {
    const auto target =
        perm::target(identity, turns.authority, turns.limits, resolution.permission_target);
    const auto gate = drive::check_right(resolution.right, request.caller, target, turns.players,
                                         turns.grants, handlers.clock.step);
    if (!gate.allowed)
      return deny(
          {{"ok", false},
           {"denied",
            {{"need", resolution.right},
             {"on", to_string(resolution.permission_target)},
             {"why", gate.reason},
             {"fix", "ask p:" + std::to_string(target.owner) + " for " + resolution.right}}}});
  }
  if (resolution.reach) {
    const auto actor = std::holds_alternative<PlayerId>(request.caller)
                           ? body
                           : std::get<ScriptHolder>(request.caller).object;
    const auto bounds = space::bounds(turns.space, identity, resolution.permission_target);
    if (!identity.valid(actor) || !bounds ||
        !drive::in_reach(turns.transforms.position[actor.slot()], {bounds->min, bounds->max},
                         turns.reach_m))
      return deny({{"ok", false},
                   {"denied",
                    {{"need", "reach"},
                     {"on", to_string(resolution.permission_target)},
                     {"why", "out_of_reach"},
                     {"fix", "move closer"}}}});
  }
  auto event = action_record(turns, request);
  const auto touched = credits(turns, request.target);
  if (resolution.route == command::Route::Native) {
    event.detail["args"] = native_arguments;
    auto completed =
        drive::invoke_native(turns.native, {request, std::move(native_arguments), body,
                                            handlers.clock.step, handlers.hz, parent, order});
    signature(completed.reply);
    reconcile_effects(handlers, completed.effects, parent);
    auto reply = completed.reply;
    const bool deferred = completed.deferred;
    const auto &resolver = handlers.execution.credits;
    for (auto &record : completed.records) {
      for (auto object : record.things)
        if (std::find(event.things.begin(), event.things.end(), object) == event.things.end())
          event.things.push_back(object);
      if (resolver.resolve)
        record.credits = resolver.resolve(resolver.context, record.things, 0, handlers.hz);
    }
    const auto actual_credits =
        resolver.resolve ? resolver.resolve(resolver.context, event.things, 0, handlers.hz)
                         : touched;
    const auto origin = drive::publish_native(
        handlers.journal, handlers.queues, handlers.timers, std::move(event), std::move(completed),
        parent, actual_credits, handlers.clock.step, handlers.event_limit);
    return {std::move(reply), origin, deferred};
  }
  std::vector<CompletedInvocation> completed;
  std::vector<Handle> created;
  std::uint64_t previous_intent = 0;
  if (const auto found = turns.intents.moving.find(body.bits); found != turns.intents.moving.end())
    previous_intent = found->second.id;
  for (const auto &plan :
       drive::plan_action(handlers.triggers, handlers.values, resolution, request, body,
                          held(turns, body), parent, handlers.clock.step, handlers.hz))
    completed.push_back(run_invocation(handlers, plan, &created));
  auto result = publish_action(handlers.journal, handlers.queues, handlers.timers, std::move(event),
                               std::move(completed), parent, touched, handlers.event_limit);
  nlohmann::json reply = {{"ok", true}, {"effect", result.changed ? "applied" : "none"}};
  if (!result.handlers) {
    reply["reason"] = "not_modeled";
    reply["say"] = "Nothing happens. This is not modeled yet.";
    const auto *player = std::get_if<PlayerId>(&request.caller);
    turns.gaps.push_back({resolution.receiver, request.action, request.action,
                          player ? *player : PlayerId{0}, result.origin});
  } else if (!result.changed)
    reply["reason"] = "no_effect";
  if (!result.errors.empty())
    reply["script_errors"] = result.errors;
  if (!created.empty()) {
    reply["created"] = nlohmann::json::array();
    for (auto object : created)
      reply["created"].push_back(to_string(object));
  }
  if (const auto found = turns.intents.moving.find(body.bits);
      found != turns.intents.moving.end() && found->second.id != previous_intent) {
    reply["effect"] = "started";
    reply["intent"] = "move_to";
    reply["intent_id"] = found->second.id;
  }
  return {std::move(reply), result.origin};
}
void consume_action(void *context, step::ActionWork work) {
  auto &turns = *static_cast<ActionTurns *>(context);
  auto arguments = observe::event_payload_json(work.request.args);
  if (arguments.is_null())
    arguments = nlohmann::json::object();
  (void)perform_action(turns, std::move(work.request), std::move(arguments), work.origin);
}
} // namespace amber::bindings
