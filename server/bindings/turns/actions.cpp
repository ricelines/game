#include "bindings/controls/controls.hpp"
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
                             std::optional<drive::InputOrder> order,
                             std::chrono::steady_clock::time_point deadline) {
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
    if (const auto *value = std::get_if<Value>(&request.args);
        value && !value->present() && native_arguments.is_object()) {
      try {
        request.args =
            decode_arguments(handlers.values.terms.rows[request.action], native_arguments,
                             std::holds_alternative<ScriptHolder>(request.caller)
                                 ? std::get<ScriptHolder>(request.caller).object
                                 : body);
      } catch (const std::exception &error) {
        return deny({{"ok", false}, {"error", "bad_input"}, {"problem", error.what()}});
      }
    }
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
    if (!gate.allowed) {
      const auto need =
          std::string(drive::required_right(resolution.right, request.caller, target));
      std::string owner = "p:" + std::to_string(target.owner);
      if (target.owner < turns.profiles.rows.size()) {
        const auto &profile = turns.profiles.rows[target.owner];
        if (profile.id == target.owner && !profile.name.empty())
          owner += " (" + profile.name + ")";
      }
      return deny({{"ok", false},
                   {"denied",
                    {{"need", need},
                     {"on", to_string(resolution.permission_target)},
                     {"why", gate.reason},
                     {"fix", "ask " + owner + " for " + need + " on this object"}}}});
    }
  }
  if (resolution.reach) {
    const auto actor = std::holds_alternative<PlayerId>(request.caller)
                           ? body
                           : std::get<ScriptHolder>(request.caller).object;
    const auto target = request.target ? request.target : resolution.receiver;
    const auto bounds = space::bounds(turns.space, identity, target);
    if (!identity.valid(actor) || !bounds ||
        !drive::in_reach(turns.transforms.position[actor.slot()], {bounds->min, bounds->max},
                         turns.reach_m))
      return deny({{"ok", false},
                   {"denied",
                    {{"need", "reach"},
                     {"on", to_string(target)},
                     {"why", "out_of_reach"},
                     {"fix", "move closer"}}}});
  }
  auto event = action_record(turns, request);
  const auto touched = credits(turns, request.target);
  if (resolution.route == command::Route::Native) {
    event.detail["args"] = native_arguments;
    auto origin = handlers.journal.append(std::move(event), parent, touched);
    auto completed =
        drive::invoke_native(turns.native, {request, std::move(native_arguments), body,
                                            handlers.clock.step, handlers.hz, origin, order});
    signature(completed.reply);
    reconcile_effects(handlers, completed.effects, origin);
    auto reply = completed.reply;
    const bool deferred = completed.deferred;
    std::vector<Handle> things;
    if (request.target)
      things.push_back(request.target);
    const auto &resolver = handlers.execution.credits;
    for (auto &record : completed.records) {
      for (auto object : record.things)
        if (std::find(things.begin(), things.end(), object) == things.end())
          things.push_back(object);
      if (resolver.resolve) {
        const auto object_credits =
            resolver.resolve(resolver.context, record.things, 0, handlers.hz);
        record.credits.insert(record.credits.end(), object_credits.begin(), object_credits.end());
      }
    }
    const auto actual_credits =
        resolver.resolve ? resolver.resolve(resolver.context, things, 0, handlers.hz) : touched;
    auto encoded_things = nlohmann::json::array();
    for (auto h : things)
      encoded_things.push_back(to_string(h));
    const bool changed = completed.changed || completed.effects.changed;
    const auto result = changed && completed.result != record::Result::Denied
                            ? record::Result::Applied
                            : completed.result;
    origin = handlers.journal.finish(origin, result, changed,
                                     {{"response", reply}, {"things", std::move(encoded_things)}},
                                     actual_credits);
    for (auto &record : completed.records)
      handlers.journal.append(std::move(record), origin);
    react::publish_effects(std::move(completed.effects), origin, handlers.clock.step,
                           handlers.queues, handlers.timers, handlers.journal,
                           handlers.event_limit);
    return {std::move(reply), origin, deferred};
  }

  auto origin = handlers.journal.append(std::move(event), parent, touched);
  std::vector<record::Credit> actual_credits;
  std::vector<Handle> consulted;
  if (request.target)
    consulted.push_back(request.target);
  std::vector<Handle> created;
  std::vector<std::string> errors;
  std::size_t count = 0;
  bool changed = false;
  std::uint64_t previous_intent = 0;
  if (const auto found = turns.intents.moving.find(body.bits); found != turns.intents.moving.end())
    previous_intent = found->second.id;
  for (auto plan : drive::plan_action(handlers.triggers, handlers.values, resolution, request, body,
                                      origin, handlers.clock.step, handlers.hz)) {
    plan.input.deadline = deadline;
    auto completed = run_invocation(turns, plan, &created);
    count += completed.outcome.status != react::RunOutcome::Status::Missing;
    changed |= completed.effects.changed;
    actual_credits.insert(actual_credits.end(), completed.object_credits.begin(),
                          completed.object_credits.end());
    for (auto h : completed.things)
      if (std::ranges::find(consulted, h) == consulted.end())
        consulted.push_back(h);
    if (!completed.outcome.error.empty())
      errors.push_back(completed.outcome.error);
    publish_invocation(handlers.journal, handlers.queues, handlers.timers, std::move(completed),
                       origin, handlers.event_limit);
  }
  nlohmann::json reply = {{"ok", true}, {"effect", changed ? "applied" : "none"}};
  auto result = changed ? record::Result::Applied : record::Result::NoEffect;
  if (!count) {
    if (resolution.route == command::Route::Fallback) {
      result = record::Result::NotModeled;
      reply["reason"] = "not_modeled";
      reply["say"] = "Nothing happens. This is not modeled yet.";
      const auto *player = std::get_if<PlayerId>(&request.caller);
      turns.gaps.push_back({resolution.receiver, request.action, request.action,
                            player ? *player : PlayerId{0}, origin});
    } else {
      result = record::Result::NoHandler;
      reply["reason"] = "no_handler";
    }
  } else if (!changed)
    reply["reason"] = "no_effect";
  if (!errors.empty())
    reply["script_errors"] = errors;
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
    std::optional<Vec3> goal;
    if (const auto *point = std::get_if<Vec3>(&found->second.goal))
      goal = *point;
    else if (const auto target = std::get<Handle>(found->second.goal); identity.valid(target))
      goal = turns.transforms.position[target.slot()];
    const auto speed = space::movement_speed(turns.space, identity, body, found->second.speed_m_s);
    reply["eta_s"] =
        goal && speed && *speed > 0
            ? nlohmann::json((*goal - turns.transforms.position[body.slot()]).length() / *speed)
            : nlohmann::json(nullptr);
  }
  for (auto h : created)
    if (std::ranges::find(consulted, h) == consulted.end())
      consulted.push_back(h);
  auto things = nlohmann::json::array();
  for (auto h : consulted)
    things.push_back(to_string(h));
  origin =
      handlers.journal.finish(origin, result, changed,
                              {{"response", reply}, {"things", std::move(things)}}, actual_credits);
  return {std::move(reply), origin};
}
} // namespace amber::bindings
