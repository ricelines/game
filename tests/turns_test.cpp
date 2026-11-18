#include "runtime_fixture.hpp"
#include "space_fixture.hpp"
#include "step/action_private.hpp"
#include "step/run_private.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <tuple>

using namespace amber;
using test_runtime::require;
namespace {
// One real invocation segment. Nested actions are exercised by game_actions.py's
// native process, rather than a second implementation of the execution stack.
react::CompletedInvocation invoke(test_runtime::Fixture &f, const react::PlannedInvocation &plan,
                                  record::Journal &journal) {
  react::Invocation invocation;
  react::CompletedInvocation completed;
  react::start_invocation(f.react_state, f.objects_state, 7, f.catalog_state, journal, plan.handler,
                          plan.input, invocation, completed);
  require(completed.outcome.status == react::RunOutcome::Status::Ok,
          "single-segment test must complete actual Lua without an action continuation");
  react::finish_invocation(identity(f.objects), attribution(f.objects), f.models, f.scripts,
                           journal, invocation, completed);
  return completed;
}
void a_watch_is_acknowledged_only_when_its_handler_starts() {
  test_runtime::Fixture f(250);
  const auto script = f.add(R"(return {kind='model',watches={'novel.heat'},on={
    ['novel.heat']=function(self) self.state.runs=(self.state.runs or 0)+1 end}})");
  f.models.rows[1].script = script;
  react::project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  f.state(f.a, 1, 0);
  f.baseline(f.a);
  f.state(f.a, 1, 1);
  const auto plan =
      react::evaluate_watch(f.react_state, f.values(), f.scripts, {{f.a, 1}, {}}, 0, 20);
  require(plan.invocations.size() == 1, "changed watch plans one run");
  f.scripts.rows[script].declaration.active = false;
  react::project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  auto &objects_state = f.objects_state;

  record::Journal journal(8, {});
  react::Invocation invocation;
  react::CompletedInvocation completed;
  const auto &candidate = plan.invocations.front();
  require(!react::start_invocation(f.react_state, objects_state, 7, f.catalog_state, journal,
                                   candidate.handler, candidate.input, invocation, completed),
          "disabled planned handler does not start");
  require(!f.states.contains({f.a, script}) && journal.take().empty(),
          "skipping a planned handler changes no script state or journal");
  f.scripts.rows[script].declaration.active = true;
  react::project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  const auto retried =
      react::evaluate_watch(f.react_state, f.values(), f.scripts, {{f.a, 1}, {}}, 1, 20);
  require(retried.invocations.size() == 1, "unstarted watch change remains pending");
  invoke(f, retried.invocations.front(), journal);
  require(std::get<double>(f.states.at({f.a, script}).values.at(std::string("runs"))) == 1 &&
              react::evaluate_watch(f.react_state, f.values(), f.scripts, {{f.a, 1}, {}}, 1, 20)
                  .invocations.empty(),
          "the watch fires once and is consumed only by its actual run");
}
void distinct_thresholds_wake_their_current_handlers() {
  test_runtime::Fixture f(250);
  const auto first = f.add(R"(return {kind='instance',watches={{term='novel.heat',
    thresholds={{predicate='above',value=1}}}},on={['novel.heat']=function(self)
      self.state.count=(self.state.count or 0)+1 end}})");
  const auto second = f.add(R"(return {kind='instance',watches={{term='novel.heat',
    thresholds={{predicate='above',value=2}}}},on={['novel.heat']=function(self)
      self.state.count=(self.state.count or 0)+1 end}})");
  const std::array scripts{first, second};
  react::set_instances(f.installed, f.a, scripts, f.scripts);
  react::project_instance(f.triggers, f.installed, f.scripts, f.terms, f.a);
  f.state(f.a, 1, 0);
  react::set_rate(f.react_state, f.terms.rows[1], f.a, 1, 0, 3, 0, 20, f.effects);
  f.baseline(f.a);
  for (const auto &[now, due, script] :
       {std::tuple{Step{0}, Step{21}, first}, std::tuple{Step{21}, Step{41}, second}}) {
    const auto timer =
        react::threshold_timer(f.react_state, f.values(), f.scripts, {f.a, 1}, now, 20, {});
    require(timer && timer->due == due, "the next timer is the earliest current consumer crossing");
    auto plans = f.changed({f.a, 1}, due);
    require(plans.size() == 1 && plans[0].handler.script == script,
            "only the handler whose own threshold crossed is selected");
    f.run(plans, due);
  }
  for (auto script : scripts)
    require(std::get<double>(f.states.at({f.a, script}).values.at(std::string("count"))) == 1,
            "each distinct threshold invokes its handler exactly once");
  require(!react::threshold_timer(f.react_state, f.values(), f.scripts, {f.a, 1}, 41, 20, {}),
          "no process timer remains after all reachable predicates have crossed");
}
void selector_changes_refresh_a_running_process() {
  test_runtime::Fixture f(250);
  const auto rule = f.add(R"(return {kind='rule',applies_to={flag='novel.ready'},
    watches={{term='novel.heat',thresholds={{predicate='above',value=2}}}},on={
    ['novel.heat']=function(self,_,value)
      self.state.count=(self.state.count or 0)+1;self.state.last=value end},
    tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=0}}}})");
  react::add_rule(f.rules, f.scripts, f.terms, rule);
  react::project_rules(f.triggers, f.rules);
  f.state(f.a, 1, 0);
  f.state(f.a, 5, 0);
  react::set_rate(f.react_state, f.terms.rows[1], f.a, 1, 0, 4, 0, 20, f.effects);
  step::State step_state;
  record::Journal journal(8, {});
  const auto drain = [&](Step now) {
    step::promote_due(step_state.timers, now, step_state.queues);
    while (auto item = step::pop_foreground(step_state.queues)) {
      auto plans = step::prepare_reactive(f.react_state, f.values(), f.scripts, step_state,
                                          std::move(*item), now, 20);
      if (!plans)
        continue;
      for (auto &plan : *plans) {
        auto completed = invoke(f, plan, journal);
        step::publish_invocation(journal, step_state.queues, step_state.timers,
                                 std::move(completed), plan.input.origin, 1000);
      }
    }
  };
  const auto publish = [&](Step now) {
    step::publish_effects(std::move(f.effects), {}, now, step_state.queues, step_state.timers,
                          journal, 1000);
    f.effects = {};
    drain(now);
  };
  publish(0);
  require(step::timers(step_state.timers).empty() && !f.states.contains({f.a, rule}),
          "a false selector leaves the running process unwatched");
  f.state(f.a, 5, 1, 10);
  publish(10);
  require(step::timers(step_state.timers).size() == 1 && !f.states.contains({f.a, rule}),
          "enabling the selector arms its existing process without invoking the quantity handler");
  f.state(f.a, 5, 0, 20);
  publish(20);
  require(step::timers(step_state.timers).empty() && !f.states.contains({f.a, rule}),
          "disabling the last interested rule cancels its crossing");
  f.state(f.a, 5, 1, 30);
  publish(30);
  drain(40);
  require(!f.states.contains({f.a, rule}), "the strict threshold is silent at equality");
  drain(41);
  require(
      std::get<double>(f.states.at({f.a, rule}).values.at(std::string("count"))) == 1 &&
          std::abs(std::get<double>(f.states.at({f.a, rule}).values.at(std::string("last"))) -
                   2.05) < 1e-9 &&
          step::timers(step_state.timers).empty(),
      "reenabling the rule delivers the actual process crossing once without another heat write");
}
void removing_an_installed_watch_cancels_its_crossing() {
  test_runtime::Fixture f(250);
  const auto script = f.add(R"(return {kind='instance',watches={{term='novel.heat',
    thresholds={{predicate='above',value=1}}}},on={['novel.heat']=function(self)
      self.state.count=(self.state.count or 0)+1 end}})");
  react::set_instances(f.installed, f.a, std::span(&script, 1), f.scripts);
  react::project_instance(f.triggers, f.installed, f.scripts, f.terms, f.a);
  f.state(f.a, 1, 0);
  react::set_rate(f.react_state, f.terms.rows[1], f.a, 1, 0, 3, 0, 20, f.effects);
  f.effects = {};
  f.baseline(f.a);
  step::State step_state;
  record::Journal journal(8, {});
  step::publish_effects(std::move(f.effects), {}, 0, step_state.queues, step_state.timers, journal,
                        1000);
  require(step::timers(step_state.timers).size() == 1,
          "the installed watch has a pending process crossing before removal");
  perm::State permissions;
  const step::NativeCall call{
      {PlayerId{1}, 0, f.a, {}}, {{"script", "s:" + std::to_string(script)}}, {}, 10, 20, {}};
  auto removed = step::editing::uninstall_script(call, f.react_state, f.values(), f.catalog_state,
                                                 permissions, 6);
  require(removed.changed, "the ordinary uninstall action removes the installed behavior");
  step::publish_effects(std::move(removed.effects), {}, 10, step_state.queues, step_state.timers,
                        journal, 1000);
  while (auto item = step::pop_foreground(step_state.queues)) {
    const auto plans = step::prepare_reactive(f.react_state, f.values(), f.scripts, step_state,
                                              std::move(*item), 10, 20);
    require(!plans || plans->empty(), "watch removal must not invoke the retired handler");
  }
  require(step::timers(step_state.timers).empty(),
          "removing the last installed watch cancels its pending process timer");
  step::promote_due(step_state.timers, 21, step_state.queues);
  require(!step::pop_foreground(step_state.queues) && !f.states.contains({f.a, script}),
          "the removed crossing cannot recreate retired script state");
}
void stored_objects_drop_work_issued_before_placement() {
  test_runtime::Fixture f(250);
  const auto script = f.add(R"(return {kind='instance',on={['novel.cue']=function(self)end}})");
  react::set_instances(f.installed, f.a, std::span(&script, 1), f.scripts);
  react::project_instance(f.triggers, f.installed, f.scripts, f.terms, f.a);
  step::State step_state;
  const auto runs = [&](Step issued) {
    react::EventWork work{f.a, {}, 6, {}, {}};
    work.issued = issued;
    const auto plans = step::prepare_reactive(f.react_state, f.values(), f.scripts, step_state,
                                              {work, {}}, 30, 20);
    return plans && !plans->empty();
  };
  require(runs(10), "a placed object's event runs its handler");
  f.objects.flags[f.a.slot()] |= Held;
  set_plot(f.objects, f.a.slot(), 0);
  require(!runs(20), "a stored object is data and runs nothing");
  f.objects.flags[f.a.slot()] &= ~Held;
  f.react_state.placed[f.a.bits] = 25;
  require(!runs(20), "work issued before the object was placed again is dropped");
  require(runs(25), "work issued when the object was placed runs");
}
void chosen_actions_and_typed_inputs() {
  test_runtime::Fixture f(250);
  f.objects.owner[f.a.slot()] = 1;
  f.objects.owner[f.b.slot()] = 1;
  f.objects.owner[f.c.slot()] = 2;
  f.objects.model[f.b.slot()] = 2;
  f.objects.model[f.c.slot()] = 2;
  f.models.rows.resize(3);
  f.models.rows[2].id = 2;
  f.terms.rows[1].dimensions[1] = 1;
  f.terms.rows[7].inputs = {{"amount", react::InputType::Number, 1, false},
                            {"note", react::InputType::String, 0, true}};
  const auto script = f.add(R"(return {kind='model',actions={['novel.touch']={right='use'}},on={
    ['novel.touch']=function(self,_,args)
      self.state.calls=(self.state.calls or 0)+1
      self.state.amount=args.amount;self.state.note=args.note or 'omitted'
    end}})",
                            3);
  f.models.rows[1].script = script;
  react::project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  amber::perm::State perm_state;
  perm::Players &players = perm_state.players;
  players.rows.resize(3);
  perm::Grants &grants = perm_state.grants;
  const auto holder = perm::Holder::script(script, f.a);
  const auto scope = perm::Scope::object(f.a);
  const auto grant =
      perm::create_grant(grants, players, perm::Holder::player(1),
                         {1, holder, scope, perm::bit(perm::Verb::Use), 0, 0, 1000},
                         {scope, 1, perm::all_rights, 0, false}, {holder, 1, true}, 0);
  require(bool(grant), "installed script requires its explicit object Use grant");
  net::Profiles profiles;
  profiles.rows.resize(3);
  profiles.rows[1].id = 1;
  profiles.rows[1].body = f.b;
  profiles.rows[2].id = 2;
  profiles.rows[2].body = f.c;

  step::Intents intents;
  step::WorkQueues queues;
  step::TimerWheel timers;
  record::Journal journal(8, {});
  space::State space_state{testspace::settings(), 8};
  auto &space = space_state.space;
  const auto deadline = RunDeadline::after(2000);
  auto root = step::begin_action(
      f.catalog_state, f.react_state, profiles, perm_state, f.objects_state, space_state, intents,
      journal, 2,
      {{PlayerId{1}, 7, f.a, Value{}}, {{"amount", 2.5}}, {}, 12, 20, {}, {}, deadline});
  require(!root.outcome && root.handlers.size() == 1 && root.call.body == f.b,
          "current player body and actual installed receiver determine the action plan");
  auto completed = invoke(f, root.handlers.front(), journal);
  step::accept_handler(root, completed);
  const auto handler_origin = completed.origin;
  step::publish_invocation(journal, queues, timers, std::move(completed), root.call.origin, 1000);
  std::vector<step::GapDraft> gaps;
  const auto result = step::finish_action(root, intents, space, identity(f.objects),
                                          transforms(f.objects), journal, gaps);
  require(result.reply["effect"] == "applied" && gaps.empty(),
          "actual stored handler effects determine the completed action");
  auto records = journal.take();
  require(records.size() == 2 && records[0]["kind"] == "action" && records[0]["actor"] == "p:1" &&
              records[0]["agent"] == true && records[0]["cause"].is_null() &&
              records[0]["root"] == records[0]["id"] && records[0]["result"] == "applied" &&
              records[1]["kind"] == "handler" && records[1]["cause"] == records[0]["id"],
          "only the authenticated player action is chosen; its real handler keeps the cause");
  require(std::get<double>(f.states.at({f.a, script}).values.at(std::string("amount"))) == 2.5 &&
              std::get<std::string>(f.states.at({f.a, script}).values.at(std::string("note"))) ==
                  "omitted",
          "raw typed inputs reach actual Lua with optional inputs absent");

  auto child = step::begin_action(f.catalog_state, f.react_state, profiles, perm_state,
                                  f.objects_state, space_state, intents, journal, 2,
                                  {{ScriptHolder{script, f.a}, 7, f.a, Value{}},
                                   {{"amount", 3}},
                                   {},
                                   12,
                                   20,
                                   handler_origin,
                                   {},
                                   deadline});
  require(!child.outcome && child.call.body == Handle{} && child.handlers.size() == 1 &&
              !child.handlers.front().input.action_body,
          "ScriptHolder calls have no inherited player body or authority");
  completed = invoke(f, child.handlers.front(), journal);
  step::accept_handler(child, completed);
  step::publish_invocation(journal, queues, timers, std::move(completed), child.call.origin, 1000);
  step::finish_action(child, intents, space, identity(f.objects), transforms(f.objects), journal,
                      gaps);
  records = journal.take();
  require(records.size() == 2 && records[0]["agent"] == false &&
              records[0]["actor"] == "s:" + std::to_string(script) &&
              records[0]["cause"] == handler_origin.cause &&
              records[0]["root"] == result.origin.root && records[0]["result"] == "applied" &&
              records[1]["cause"] == records[0]["id"],
          "script action retains actual actor/cause/root while remaining nonchosen");
  perm::revoke(grants, players, grant.grant);
  auto denied = step::begin_action(f.catalog_state, f.react_state, profiles, perm_state,
                                   f.objects_state, space_state, intents, journal, 2,
                                   {{ScriptHolder{script, f.a}, 7, f.a, Value{}},
                                    {{"amount", 4}},
                                    {},
                                    12,
                                    20,
                                    handler_origin,
                                    {},
                                    deadline});
  auto foreign = step::begin_action(
      f.catalog_state, f.react_state, profiles, perm_state, f.objects_state, space_state, intents,
      journal, 2, {{PlayerId{2}, 7, f.a, Value{}}, {{"amount", 4}}, {}, 12, 20, {}, {}, deadline});
  require(denied.outcome && foreign.outcome && denied.handlers.empty() && foreign.handlers.empty(),
          "revocation and foreign-owner denial happen before handler invocation");
  for (const auto &outcome : {denied.outcome, foreign.outcome})
    for (const auto *field : {"need", "on", "why", "fix"})
      require(outcome->reply.at("denied").contains(field),
              "S5 denial includes its actionable fields");
  records = journal.take();
  require(records.size() == 2 && records[0]["agent"] == false && records[1]["agent"] == true &&
              records[0]["result"] == "denied" && records[1]["result"] == "denied" &&
              std::get<double>(f.states.at({f.a, script}).values.at(std::string("calls"))) == 2,
          "real denial retains chosen classification and leaves script state unchanged");

  auto &native = f.react_state.native_declarations;
  const std::string signature = "novel.touch(amount: number [kg], note?: string)";
  step::Input input;
  input.caller = PlayerId{1};
  input.body = f.b;
  for (const auto &fields : std::vector<nlohmann::json>{
           {{"action", "novel.unknown"}, {"target", "not a handle"}, {"args", {{"x", 2}}}},
           {{"action", "novel.touch"}, {"target", "not a handle"}, {"args", {{"amount", 3}}}}}) {
    input.fields = fields;
    const auto parsed = step::parse_action_input(input, f.terms, native, journal, 12, 20);
    require(std::holds_alternative<step::InputReply>(parsed),
            "accepted unknown action or malformed target fails during actual input parsing");
    const auto &reply = std::get<step::InputReply>(parsed).fields;
    records = journal.take();
    require(reply["error"] == "bad_input" && records.size() == 1 &&
                records[0]["name"] == fields["action"] && records[0]["args"] == fields["args"] &&
                records[0]["raw_target"] == fields["target"] && records[0]["things"].empty() &&
                records[0]["agent"] == true && records[0]["changed"] == false &&
                records[0]["result"] == "denied" && records[0]["response"] == reply,
            "parse failure publishes one chosen denial with exact raw diagnostics and no handles");
    require(reply.contains("signature") == (fields["action"] == "novel.touch"),
            "only known registered actions advertise a truthful signature");
  }
  for (const auto &arguments :
       std::vector<nlohmann::json>{{{"amount", "not a number"}}, {{"unknown", 3}}}) {
    input.fields = {{"action", "novel.touch"}, {"target", to_string(f.a)}, {"args", arguments}};
    const auto parsed = step::parse_action_input(input, f.terms, native, journal, 12, 20);
    require(std::holds_alternative<step::InputReply>(parsed),
            "wrong input types and unknown fields fail during actual typed decoding");
    const auto &reply = std::get<step::InputReply>(parsed).fields;
    records = journal.take();
    require(reply["error"] == "bad_input" && reply["signature"] == signature &&
                records.size() == 1 && records[0]["result"] == "denied" &&
                records[0]["args"] == arguments && records[0]["raw_target"] == to_string(f.a) &&
                records[0]["things"].empty() && records[0]["agent"] == true &&
                records[0]["changed"] == false && records[0]["response"] == reply,
            "typed decoding publishes one chosen denial with exact raw diagnostics and signature");
  }
  input.fields = {
      {"action", "novel.touch"}, {"target", to_string(f.a)}, {"args", nlohmann::json::object()}};
  const auto parsed = step::parse_action_input(input, f.terms, native, journal, 12, 20);
  require(std::holds_alternative<step::NativeCall>(parsed),
          "an absent required field reaches action declaration validation");
  auto malformed_call = std::get<step::NativeCall>(parsed);
  malformed_call.origin = {};
  malformed_call.deadline = deadline;
  const auto malformed =
      step::begin_action(f.catalog_state, f.react_state, profiles, perm_state, f.objects_state,
                         space_state, intents, journal, 2, std::move(malformed_call));
  require(malformed.outcome && malformed.outcome->reply["error"] == "bad_input" &&
              malformed.outcome->reply["signature"] == signature && malformed.handlers.empty(),
          "missing required input fails before Lua and includes the actual signature");
  records = journal.take();
  require(records.size() == 1 && records[0]["result"] == "denied" && records[0]["args"].empty() &&
              records[0]["things"] == nlohmann::json::array({to_string(f.a)}) &&
              records[0]["agent"] == true && records[0]["changed"] == false &&
              records[0]["response"] == malformed.outcome->reply &&
              std::get<double>(f.states.at({f.a, script}).values.at(std::string("calls"))) == 2,
          "declaration validation records one chosen denial without invoking the handler");
}
void canonical_results_and_actual_bounds() {
  test_runtime::Fixture f(250);
  f.objects.owner[f.a.slot()] = 1;
  f.objects.owner[f.b.slot()] = 1;
  f.objects.owner[f.c.slot()] = 2;
  f.objects.flags[f.a.slot()] = Body;
  f.objects.model[f.b.slot()] = 2;
  f.objects.model[f.c.slot()] = 2;
  f.objects.position[f.b.slot()] = {0, 1, 0};
  f.models.rows.resize(3);
  f.models.rows[2].id = 2;
  const auto script =
      f.add(R"(return {kind='model',actions={['novel.touch']={right='use'}},on={}})");
  f.models.rows[1].script = script;
  react::project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  space::State space_state{testspace::settings(), 8};
  auto &space = space_state.space;
  testspace::Box shape({.6f, .6f, 1.8f});
  const auto geometry = space::import_geometry(space, shape.source());
  space::BodySpec body{
      f.a, geometry.geometry, space::Motion::Character, {.6f, 0, 0}, 70, false, false, {}};
  body.character =
      space::CharacterSpec{.3f, 1.8f, 70, 100, 1.8f, 8, .24f, std::numbers::pi_v<float> / 4, .3f};
  require(space::add(space, identity(f.objects), transforms(f.objects), body).status ==
              space::Status::Ok,
          "real CharacterVirtual supplies the actual configured movement speed");
  body.object = f.b;
  body.motion = space::Motion::Static;
  body.character.reset();
  require(space::add(space, identity(f.objects), transforms(f.objects), body).status ==
              space::Status::Ok,
          "actual target bounds are queryable for ordinary fallback reach");
  net::Profiles profiles;
  profiles.rows.resize(3);
  profiles.rows[1].id = 1;
  profiles.rows[1].body = f.a;
  profiles.rows[2].id = 2;
  profiles.rows[2].body = f.c;
  amber::perm::State perm_state;
  perm::Players &players = perm_state.players;
  players.rows.resize(3);

  step::Intents intents;
  record::Journal journal(8, {});
  std::vector<step::GapDraft> gaps;
  const auto deadline = RunDeadline::after(2000);
  auto declared = step::begin_action(
      f.catalog_state, f.react_state, profiles, perm_state, f.objects_state, space_state, intents,
      journal, 2,
      {{PlayerId{1}, 7, {}, Value{}}, nlohmann::json::object(), {}, 0, 20, {}, {}, deadline});
  require(!declared.outcome && declared.handlers.empty(),
          "declared action has no fabricated handler");
  auto result = step::finish_action(declared, intents, space, identity(f.objects),
                                    transforms(f.objects), journal, gaps);
  require(result.reply["effect"] == "none" && result.reply["reason"] == "no_handler" &&
              gaps.empty(),
          "declared action without a handler is distinct from an unmodeled action");
  f.models.rows[1].script = 0;
  react::project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  auto fallback = step::begin_action(
      f.catalog_state, f.react_state, profiles, perm_state, f.objects_state, space_state, intents,
      journal, 2,
      {{PlayerId{1}, 7, f.b, Value{}}, nlohmann::json::object(), {}, 0, 20, {}, {}, deadline});
  require(!fallback.outcome && fallback.resolution.route == react::Route::Fallback,
          "nearby undeclared action remains a real open-use attempt");
  result = step::finish_action(fallback, intents, space, identity(f.objects), transforms(f.objects),
                               journal, gaps);
  require(result.reply["reason"] == "not_modeled" && gaps.size() == 1 && gaps[0].target == f.b,
          "actual fallback completion preserves the unmodeled target and source attempt");
  require(space::set_pose(space, identity(f.objects), transforms(f.objects), f.b, {{0, 20, 0}, {}})
                  .status == space::Status::Ok,
          "move actual Jolt target beyond reach");
  auto remote = step::begin_action(
      f.catalog_state, f.react_state, profiles, perm_state, f.objects_state, space_state, intents,
      journal, 2,
      {{PlayerId{1}, 7, f.b, Value{}}, nlohmann::json::object(), {}, 0, 20, {}, {}, deadline});
  require(remote.outcome && remote.outcome->reply["denied"]["need"] == "reach" && gaps.size() == 1,
          "fallback cannot create remote gap captures outside actual target bounds");
  require(step::in_reach({0, 0, 0}, {{-.5f, -1, -1}, {100, 1, 1}}, 2),
          "closest actual bounds permit large geometry whose center is farther away");

  // Finishing uses the actual newly started intent and CharacterVirtual speed cap.
  f.models.rows[1].script = script;
  react::project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  auto moving = step::begin_action(
      f.catalog_state, f.react_state, profiles, perm_state, f.objects_state, space_state, intents,
      journal, 2,
      {{PlayerId{1}, 7, {}, Value{}}, nlohmann::json::object(), {}, 0, 20, {}, {}, deadline});
  step::start_move(intents, f.a, Vec3{9, 0, 0}, moving.call.origin, 9);
  result = step::finish_action(moving, intents, space, identity(f.objects), transforms(f.objects),
                               journal, gaps);
  require(result.reply["effect"] == "started" &&
              std::abs(result.reply["eta_s"].get<double>() - 5) < 1e-5,
          "movement estimate clamps requested speed to the actual character configuration");
  auto slower = step::begin_action(
      f.catalog_state, f.react_state, profiles, perm_state, f.objects_state, space_state, intents,
      journal, 2,
      {{PlayerId{1}, 7, {}, Value{}}, nlohmann::json::object(), {}, 0, 20, {}, {}, deadline});
  step::start_move(intents, f.a, Vec3{9, 0, 0}, slower.call.origin, .3f);
  result = step::finish_action(slower, intents, space, identity(f.objects), transforms(f.objects),
                               journal, gaps);
  require(result.reply["effect"] == "started" &&
              std::abs(result.reply["eta_s"].get<double>() - 30) < 1e-5,
          "movement estimate retains a slower actual authored intent");
}
void threshold_plans_use_current_declarations(bool benchmark) {
  test_runtime::Fixture f(250);
  f.state(f.a, 1, 0);
  react::set_rate(f.react_state, f.terms.rows[1], f.a, 1, 0, 20, 0, 20, f.effects);
  const auto direct = f.add(R"(return {kind='instance',watches={{term='novel.heat',thresholds={
    {predicate='above',value=1},{predicate='above',value=2},{predicate='above',value=3}}}},on={
    ['novel.heat']=function(self,source,value)
      assert(source==self.handle and value==self:get('novel.heat'))
      self.state.count=(self.state.count or 0)+1;self.state.last=value
    end,['novel.cue']=function(self,_,rate) self:rate('novel.heat',rate,0,20) end}})");
  const auto sibling = f.add(R"(return {kind='instance',watches={{term='novel.heat',
    thresholds={{predicate='above',value=1},{predicate='above',value=2},
    {predicate='above',value=3}}}},on={['novel.heat']=function(self,source,value)
      assert(source==self.handle and type(value)=='number' and value>1)
      self.state.count=(self.state.count or 0)+1;self.state.last=value
    end}})");
  const auto bare = f.add(R"(return {kind='model',on={['novel.heat']=function(self)
    error('unwatched quantity handler must not consume threshold work')end}})");
  f.models.rows[1].script = bare;
  react::project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  const std::array<ScriptId, 2> consumers{direct, sibling};
  react::set_instances(f.installed, f.a, consumers, f.scripts);
  react::project_instance(f.triggers, f.installed, f.scripts, f.terms, f.a);
  f.baseline(f.a);
  step::State step_state;
  auto &queues = step_state.queues;
  auto &timers = step_state.timers;

  record::Journal journal(8, {});
  journal.restore_next_id(10);
  const amber::Origin origin{9, 3, {1, 2}, 1.5, false};
  auto due = react::threshold_timer(f.react_state, f.values(), f.scripts, {f.a, 1}, 0, 20, origin);
  require(due && due->due == 21, "both watches share the actual process crossing timer");
  const auto obsolete = *due;
  step::schedule(timers, std::move(*due));
  const auto crossing = [&](Step now) {
    step::promote_due(timers, now, queues);
    while (auto item = step::pop_foreground(queues)) {
      auto plans = step::prepare_reactive(f.react_state, f.values(), f.scripts, step_state,
                                          std::move(*item), now, 20);
      if (!plans || plans->empty())
        continue;
      require(plans->size() == 2, "one watched fact delivers the two current explicit handlers");
      for (auto &plan : *plans) {
        plan.input.deadline = RunDeadline::after(2000);
        auto completed = invoke(f, plan, journal);
        step::publish_invocation(journal, queues, timers, std::move(completed), plan.input.origin,
                                 1000);
      }
    }
  };
  crossing(21);
  for (auto key : std::array<ScriptKey, 2>{{{f.a, direct}, {f.a, sibling}}})
    require(std::get<double>(f.states.at(key).values.at(std::string("count"))) == 1 &&
                std::abs(std::get<double>(f.states.at(key).values.at(std::string("last"))) - 1.05) <
                    1e-9,
            "both handlers receive the actual sampled source scalar once");
  require(!f.states.contains({f.a, bare}) && !f.states.contains({f.b, bare}),
          "bare quantity siblings never consume ordinary threshold work");
  require(std::abs(std::get<react::Answer>(f.watch_answers.at({f.a, 1})).value.numeric() - 1.05) <
              1e-9,
          "actual handler startup saves the delivered answer");
  auto next = step::timers(timers);
  require(next.size() == 1 && next[0].due == 41,
          "source evaluation rearms the next threshold for its fact");
  crossing(41);
  require(std::get<double>(f.states.at({f.a, direct}).values.at(std::string("count"))) == 2 &&
              std::get<double>(f.states.at({f.a, sibling}).values.at(std::string("count"))) == 2 &&
              step::timers(timers).size() == 1 &&
              !step::prepare_reactive(f.react_state, f.values(), f.scripts, step_state,
                                      {obsolete.event, obsolete.threshold}, 41, 20),
          "a replacement timer makes an already queued older crossing harmless");
  snap::Buffer bytes;
  snap::Output out{bytes};
  react::write_script_states(out, f.states, f.watch_answers, react::Placements{});
  out.adapter().flush();
  bytes.resize(out.adapter().writtenBytesCount());
  snap::Input in{bytes.data(), bytes.size()};
  {
    react::Placements placed;
    react::read_script_states(in, f.states, f.watch_answers, placed);
  }
  snap::check_complete(in);
  const auto restored_due = step::timers(timers).front().due;
  require(f.changed({f.a, 1}, restored_due).size() == 2,
          "restored answers preserve the next crossing for both handlers");
  auto records = journal.take();
  require(records.size() == 4 && records[0]["cause"] == 9 && records[0]["root"] == 3,
          "threshold handlers retain timer provenance without fake chosen input actions");

  const std::size_t rearms = benchmark ? 10000 : 1000;
  const auto started = step::Monotonic::now();
  for (std::size_t i = 0; i < rearms; ++i) {
    auto plans = step::plan_event(f.triggers, f.values(),
                                  {f.a, {}, 6, Value(i % 2 ? .000001 : .000002), origin}, 41, 20);
    require(plans.size() == 1, "only the installed rate handler receives its event");
    plans.front().input.deadline = RunDeadline::after(2000);
    auto completed = invoke(f, plans.front(), journal);
    auto changed = react::evaluate_watch(f.react_state, f.values(), f.scripts,
                                         {{f.a, 1}, completed.origin}, 41, 20);
    require(changed.invocations.empty(),
            "rate changes preserving the sampled value do not wake Lua");
    step::apply_thresholds(timers, std::move(changed.thresholds));
    step::publish_invocation(journal, queues, timers, std::move(completed), origin, 1000);
    // The explicit source evaluation above already consumed this handler's dirty fact.
    while (auto item = step::pop_foreground(queues))
      require(std::holds_alternative<react::WatchWork>(item->work),
              "rate-only handler produces dirty source work, never extra action callbacks");
    require(timers.thresholds.size() == 1,
            "actual repeated Lua rates retain one pending timer for the watched fact");
  }
  next = step::timers(timers);
  records = journal.take();
  require(next.size() == 1 && next.front().due > 1000000 && records.size() == rearms &&
              next.back().event.origin.cause == records.back()["id"] &&
              std::get<double>(f.states.at({f.a, direct}).values.at(std::string("count"))) == 2,
          "far-future replacements retain latest real origin without invented threshold callbacks");
  auto plans = step::plan_event(f.triggers, f.values(), {f.a, {}, 6, Value(-.5), origin}, 41, 20);
  plans.front().input.deadline = RunDeadline::after(2000);
  auto completed = invoke(f, plans.front(), journal);
  auto changed = react::evaluate_watch(f.react_state, f.values(), f.scripts,
                                       {{f.a, 1}, completed.origin}, 41, 20);
  step::apply_thresholds(timers, std::move(changed.thresholds));
  step::publish_invocation(journal, queues, timers, std::move(completed), origin, 1000);
  while (step::pop_foreground(queues)) {
  }
  crossing(step::timers(timers).front().due);
  require(std::get<double>(f.states.at({f.a, direct}).values.at(std::string("count"))) == 3 &&
              std::get<double>(f.states.at({f.a, sibling}).values.at(std::string("count"))) == 3 &&
              step::timers(timers).size() == 1,
          "next real crossing still reaches both scripts exactly once after replacements");
  // A stopped real process has no future crossing. Its old far-future tokens
  // must disappear, while unrelated ordinary timers remain independent.
  step::schedule(timers, {1000000000, {f.c, {}, 6, {}, origin}, {}});
  const auto stopped_token = step::timers(timers).front();
  const auto stopped_at = timers.now;
  plans = step::plan_event(f.triggers, f.values(), {f.a, {}, 6, Value(0.), origin}, stopped_at, 20);
  plans.front().input.deadline = RunDeadline::after(2000);
  completed = invoke(f, plans.front(), journal);
  require(f.processes[1].get(f.a.slot())->rate == 0 && timers.thresholds.size() == 1,
          "actual Lua stopping a process exposes its pending old crossing");
  auto stopped = react::evaluate_watch(f.react_state, f.values(), f.scripts,
                                       {{f.a, 1}, completed.origin}, stopped_at, 20);
  step::apply_thresholds(timers, std::move(stopped.thresholds));
  require(stopped.invocations.empty() && timers.thresholds.empty() &&
              step::timers(timers).size() == 1 && step::timers(timers).front().due == 1000000000,
          "zero rate cancels the fact crossing and preserves unrelated ordinary timers");
  auto stale = step::prepare_reactive(f.react_state, f.values(), f.scripts, step_state,
                                      {stopped_token.event, stopped_token.threshold}, 10000, 20);
  require(stale && stale->empty(), "a stopped process crossing only requests a fact evaluation");
  auto work = step::pop_foreground(queues);
  require(work.has_value(), "the due fact is checked through the ordinary watch queue");
  auto stopped_delivery = step::prepare_reactive(f.react_state, f.values(), f.scripts, step_state,
                                                 std::move(*work), 10000, 20);
  require(stopped_delivery && stopped_delivery->empty() && !step::pop_foreground(queues),
          "a queued crossing after stopping the process invokes no handler");
  step::publish_invocation(journal, queues, timers, std::move(completed), origin, 1000);
  while (auto item = step::pop_foreground(queues))
    require(std::holds_alternative<react::WatchWork>(item->work) &&
                std::get<react::WatchWork>(item->work).source == FactKey{f.a, 1},
            "stopping a process publishes only its actual dirty source");
  if (benchmark)
    std::cout
        << "actual Lua rate + watch planning + Journal + timer replacement: rearms=" << rearms
        << " pending=" << timers.thresholds.size() << " mean us/op="
        << std::chrono::duration<double, std::micro>(step::Monotonic::now() - started).count() /
               rearms
        << '\n';
}
} // namespace
int main(int argc, char **argv) {
  try {
    const bool benchmark = argc == 2 && std::string_view(argv[1]) == "--bench";
    require(argc == 1 || benchmark, "usage: turns_test [--bench]");
    a_watch_is_acknowledged_only_when_its_handler_starts();
    distinct_thresholds_wake_their_current_handlers();
    selector_changes_refresh_a_running_process();
    removing_an_installed_watch_cancels_its_crossing();
    stored_objects_drop_work_issued_before_placement();
    chosen_actions_and_typed_inputs();
    canonical_results_and_actual_bounds();
    threshold_plans_use_current_declarations(benchmark);
    std::cout
        << "Actual action planning, attribution, typed inputs, threshold plans and Lua pass.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
