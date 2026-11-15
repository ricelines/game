#include "action_private.hpp"

#include <algorithm>

namespace amber::step {
namespace {
void include(std::vector<Handle> &things, Handle object) {
  if (object && std::ranges::find(things, object) == things.end())
    things.push_back(object);
}
record::Event action_record(const NativeCall &call, const react::Terms &terms,
                            const catalog::Scripts &scripts) {
  record::Event event;
  event.step = call.step;
  event.kind = record::Kind::Action;
  if (const auto *player = std::get_if<PlayerId>(&call.request.caller))
    event.actor = record::Actor::player(*player);
  else {
    const auto script = std::get<ScriptHolder>(call.request.caller).script;
    event.actor = record::Actor::script(script);
    event.script_author = scripts.rows.at(script).author;
  }
  if (call.request.action && call.request.action < terms.rows.size())
    event.name = terms.rows[call.request.action].name;
  include(event.things, call.request.target);
  event.detail = {{"args", call.arguments},
                  {"agent", std::holds_alternative<PlayerId>(call.request.caller)}};
  return event;
}
bool installed_caller(const catalog::Models &models, const catalog::Scripts &scripts,
                      const react::InstanceScripts &installations, const react::RuleVersions &rules,
                      const react::ValueRead &values, Step now, double hz, ScriptHolder caller) {
  if (!values.identity.valid(caller.object) || !caller.script ||
      caller.script >= scripts.rows.size() || !scripts.rows[caller.script].declaration.active)
    return false;
  auto installed = react::installed_scripts(models, installations, values.identity, caller.object);
  if (std::ranges::find(installed, caller.script) != installed.end())
    return true;
  installed = react::rule_scripts(rules, values, caller.object, now, hz);
  return std::ranges::find(installed, caller.script) != installed.end();
}
} // namespace
ActionRun begin_action(const react::ValueRead &values, const catalog::Models &models,
                       const catalog::Scripts &scripts, const react::InstanceScripts &installations,
                       const react::RuleVersions &rules, const react::Triggers &triggers,
                       const react::NativeActions &native, const net::Profiles &profiles,
                       const perm::Players &players, const perm::Grants &grants,
                       const perm::OwnerLimits &limits, AuthorityRead authority,
                       AttributionRead attribution, TransformRead poses, const space::Space &space,
                       const Intents &intents, record::Journal &journal, float reach_m,
                       NativeCall call, const record::Origin &parent,
                       Monotonic::time_point deadline) {
  ActionRun run;
  run.call = std::move(call);
  run.deadline = deadline;
  auto &request = run.call.request;
  auto event = action_record(run.call, values.terms, scripts);
  const auto deny = [&](nlohmann::json reply) {
    if (reply.value("error", "") == "bad_input" && request.action &&
        request.action < values.terms.rows.size() &&
        values.terms.rows[request.action].kind == react::Kind::Action)
      reply["signature"] = react::action_signature(values.terms, native, request.action);
    event.result = record::Result::Denied;
    event.detail["response"] = reply;
    event.credits = record::credits(values.identity, attribution, models, scripts, event.things, 0,
                                    run.call.hz);
    run.outcome = ActionOutcome{std::move(reply), journal.append(std::move(event), parent)};
  };
  if (const auto *player = std::get_if<PlayerId>(&request.caller)) {
    if (!*player || *player >= profiles.rows.size() || profiles.rows[*player].id != *player) {
      deny({{"ok", false}, {"error", "unknown_player"}});
      return run;
    }
    run.call.body = profiles.rows[*player].body;
  } else {
    run.call.body = {};
    run.call.order.reset();
    if (!installed_caller(models, scripts, installations, rules, values, run.call.step, run.call.hz,
                          std::get<ScriptHolder>(request.caller))) {
      deny({{"ok", false}, {"error", "inactive_script_holder"}});
      return run;
    }
  }
  run.resolution = react::resolve(native, values.terms, models, scripts, installations,
                                  values.identity, request, run.call.body);
  if (!run.resolution) {
    constexpr const char *failures[]{"none", "unknown_action", "stale_target", "missing_target"};
    deny({{"ok", false},
          {"error", "bad_input"},
          {"problem", failures[static_cast<unsigned>(run.resolution.failure)]}});
    return run;
  }
  const auto &resolution = run.resolution;
  if (resolution.route == react::Route::Native &&
      (std::holds_alternative<react::RegistryAction>(resolution.native_operation) ||
       (std::get_if<react::ElaborationAction>(&resolution.native_operation) &&
        std::get<react::ElaborationAction>(resolution.native_operation) ==
            react::ElaborationAction::RuleActivation)))
    event.kind = record::Kind::Registry;
  if (std::holds_alternative<PlayerId>(request.caller) &&
      resolution.route == react::Route::Native &&
      std::holds_alternative<react::MechanicAction>(resolution.native_operation)) {
    deny({{"ok", false},
          {"error", "bad_input"},
          {"problem",
           "engine.grasp, release, attach, impulse and transfer are called by a body script"}});
    return run;
  }
  try {
    if (!run.call.arguments.is_object())
      throw std::invalid_argument("action args must be an input record");
    if (resolution.route != react::Route::Native) {
      if (const auto *value = std::get_if<Value>(&request.args); value && !value->present())
        request.args = decode_arguments(values.terms.rows[request.action], run.call.arguments,
                                        std::holds_alternative<ScriptHolder>(request.caller)
                                            ? std::get<ScriptHolder>(request.caller).object
                                            : run.call.body);
      const auto errors =
          react::validate_arguments(values.terms, values.terms.rows[request.action], request.args);
      if (!errors.empty()) {
        std::string problem;
        for (const auto &error : errors) {
          if (!problem.empty())
            problem += "; ";
          problem += error.input + ": " + error.problem;
        }
        throw std::invalid_argument(problem);
      }
    }
  } catch (const std::exception &error) {
    deny({{"ok", false}, {"error", "bad_input"}, {"problem", error.what()}});
    return run;
  }
  if (!resolution.right.empty()) {
    const auto target =
        perm::target(values.identity, authority, limits, resolution.permission_target);
    const auto gate =
        perm::check_right(resolution.right, request.caller, target, players, grants, run.call.step);
    if (!gate.allowed) {
      const auto need = std::string(perm::required_right(resolution.right, request.caller, target));
      auto owner = "p:" + std::to_string(target.owner);
      if (target.owner < profiles.rows.size() && !profiles.rows[target.owner].name.empty())
        owner += " (" + profiles.rows[target.owner].name + ")";
      deny({{"ok", false},
            {"denied",
             {{"need", need},
              {"on", to_string(resolution.permission_target)},
              {"why", gate.reason},
              {"fix", "ask " + owner + " for " + need + " on this object"}}}});
      return run;
    }
  }
  if (resolution.reach) {
    const auto actor = std::holds_alternative<PlayerId>(request.caller)
                           ? run.call.body
                           : std::get<ScriptHolder>(request.caller).object;
    const auto target = request.target ? request.target : resolution.receiver;
    const auto bounds = space::bounds(space, values.identity, target);
    if (!values.identity.valid(actor) || !bounds ||
        !in_reach(poses.position[actor.slot()], {bounds->min, bounds->max}, reach_m)) {
      deny({{"ok", false},
            {"denied",
             {{"need", "reach"},
              {"on", to_string(target)},
              {"why", "out_of_reach"},
              {"fix", "move closer"}}}});
      return run;
    }
  }
  include(run.things, request.target);
  run.credits =
      record::credits(values.identity, attribution, models, scripts, run.things, 0, run.call.hz);
  run.call.origin = journal.append(std::move(event), parent, run.credits);
  if (const auto found = intents.moving.find(run.call.body.bits); found != intents.moving.end())
    run.previous_intent = found->second.id;
  run.handlers = plan_action(triggers, values, resolution, request, run.call.body, run.call.origin,
                             run.call.step, run.call.hz);
  for (auto &handler : run.handlers)
    handler.input.deadline = deadline;
  return run;
}
void accept_handler(ActionRun &run, const react::CompletedInvocation &handler) {
  run.handled += handler.outcome.status != react::RunOutcome::Status::Missing;
  run.changed |= handler.effects.changed;
  run.credits.insert(run.credits.end(), handler.object_credits.begin(),
                     handler.object_credits.end());
  for (auto thing : handler.things)
    include(run.things, thing);
  if (!handler.outcome.error.empty())
    run.errors.push_back(handler.outcome.error);
}
ActionOutcome finish_action(ActionRun &run, const Intents &intents, const space::Space &space,
                            IdentityRead identity, TransformRead poses, record::Journal &journal,
                            std::vector<GapDraft> &gaps) {
  nlohmann::json reply{{"ok", true}, {"effect", run.changed ? "applied" : "none"}};
  auto result = run.changed ? record::Result::Applied : record::Result::NoEffect;
  if (!run.handled) {
    if (run.resolution.route == react::Route::Fallback) {
      result = record::Result::NotModeled;
      reply["reason"] = "not_modeled";
      reply["say"] = "Nothing happens. This is not modeled yet.";
      const auto *player = std::get_if<PlayerId>(&run.call.request.caller);
      gaps.push_back({run.resolution.receiver, run.call.request.action, run.call.request.action,
                      player ? *player : PlayerId{0}, run.call.origin});
    } else {
      result = record::Result::NoHandler;
      reply["reason"] = "no_handler";
    }
  } else if (!run.changed)
    reply["reason"] = "no_effect";
  if (!run.errors.empty())
    reply["script_errors"] = run.errors;
  if (!run.created.empty()) {
    reply["created"] = nlohmann::json::array();
    for (auto object : run.created) {
      reply["created"].push_back(to_string(object));
      include(run.things, object);
    }
  }
  if (const auto found = intents.moving.find(run.call.body.bits);
      found != intents.moving.end() && found->second.id != run.previous_intent) {
    reply["effect"] = "started";
    reply["intent"] = "move_to";
    reply["intent_id"] = found->second.id;
    std::optional<Vec3> goal;
    if (const auto *point = std::get_if<Vec3>(&found->second.goal))
      goal = *point;
    else if (const auto object = std::get<Handle>(found->second.goal); identity.valid(object))
      goal = poses.position[object.slot()];
    const auto speed =
        space::movement_speed(space, identity, run.call.body, found->second.speed_m_s);
    reply["eta_s"] =
        goal && speed && *speed > 0
            ? nlohmann::json((*goal - poses.position[run.call.body.slot()]).length() / *speed)
            : nlohmann::json(nullptr);
  }
  auto things = nlohmann::json::array();
  for (auto object : run.things)
    things.push_back(to_string(object));
  auto origin = journal.finish(run.call.origin, result, run.changed,
                               {{"response", reply}, {"things", std::move(things)}}, run.credits);
  return {std::move(reply), origin};
}
ActionOutcome finish_native(ActionRun &run, const NativeCompletion &completion, IdentityRead ids,
                            AttributionRead attribution, const catalog::Models &models,
                            const catalog::Scripts &scripts, record::Journal &journal) {
  for (const auto &event : completion.records) {
    for (auto object : event.things)
      include(run.things, object);
    auto credits = record::credits(ids, attribution, models, scripts, event.things, 0, run.call.hz);
    run.credits.insert(run.credits.end(), credits.begin(), credits.end());
    run.credits.insert(run.credits.end(), event.credits.begin(), event.credits.end());
  }
  auto things = nlohmann::json::array();
  for (auto object : run.things)
    things.push_back(to_string(object));
  const bool changed = completion.changed || completion.effects.changed;
  const auto result = changed && completion.result != record::Result::Denied
                          ? record::Result::Applied
                          : completion.result;
  auto origin =
      journal.finish(run.call.origin, result, changed,
                     {{"response", completion.reply}, {"things", std::move(things)}}, run.credits);
  return {completion.reply, origin, completion.deferred};
}
} // namespace amber::step
