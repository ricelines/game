#include "runtime_fixture.hpp"
#include "space_fixture.hpp"
#include "step/action_private.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>

using namespace amber;
using test_runtime::require;
namespace {
// One real invocation segment. Nested actions are exercised by game_actions.py's
// native process, rather than a second implementation of the execution stack.
react::CompletedInvocation invoke(test_runtime::Fixture &f, const step::PlannedInvocation &plan,
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
  f.bind(script, f.a);
  f.state(f.a, 1, 1);
  const auto plan = step::plan_watch(f.subscriptions, f.values(), f.allocations, f.triggers,
                                     f.scripts, {{f.a, 1}, {}}, 0, 20);
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
  const auto retried = step::plan_watch(f.subscriptions, f.values(), f.allocations, f.triggers,
                                        f.scripts, {{f.a, 1}, {}}, 1, 20);
  require(retried.invocations.size() == 1, "unstarted watch change remains pending");
  invoke(f, retried.invocations.front(), journal);
  require(std::get<double>(f.states.at({f.a, script}).at(std::string("runs"))) == 1 &&
              step::plan_watch(f.subscriptions, f.values(), f.allocations, f.triggers, f.scripts,
                               {{f.a, 1}, {}}, 1, 20)
                  .invocations.empty(),
          "the watch fires once and is consumed only by its actual run");
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
  const auto deadline = step::Monotonic::now() + std::chrono::seconds(2);
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
  require(std::get<double>(f.states.at({f.a, script}).at(std::string("amount"))) == 2.5 &&
              std::get<std::string>(f.states.at({f.a, script}).at(std::string("note"))) ==
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
              std::get<double>(f.states.at({f.a, script}).at(std::string("calls"))) == 2,
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
              std::get<double>(f.states.at({f.a, script}).at(std::string("calls"))) == 2,
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
  const auto deadline = step::Monotonic::now() + std::chrono::seconds(2);
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
void threshold_plans_use_current_subscriptions(bool benchmark) {
  test_runtime::Fixture f(250);
  f.state(f.a, 1, 0);
  react::set_rate(f.react_state, f.terms.rows[1], f.a, 1, 0, 20,
                  amber::step::Clock{.step = 0, .hz = 20}, f.effects);
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
  f.bind(direct, f.a);
  f.bind(sibling, f.a);
  step::WorkQueues queues;
  step::TimerWheel timers;

  record::Journal journal(8, {});
  journal.restore_next_id(10);
  const record::Origin origin{9, 3, {1, 2}, 1.5, false};
  auto due = react::threshold_timers(f.subscriptions, f.processes, {f.a, 1}, 0, 20, origin);
  require(due.size() == 2 && due[0].due == 21 && due[1].due == 21,
          "both subscriptions arm the actual process crossing");
  const auto obsolete = due[0];
  for (auto &timer : due)
    step::schedule(timers, std::move(timer));
  const auto crossing = [&](Step now) {
    step::promote_due(timers, now, queues);
    while (auto item = step::pop_foreground(queues)) {
      if (auto *event = std::get_if<step::EventWork>(&item->work)) {
        const step::EventTurn turn{*event, item->threshold};
        if (step::valid_event(turn, identity(f.objects), f.processes, f.subscriptions, now, 20)) {
          require(turn.threshold.has_value(), "crossing fixture produces threshold events only");
          step::enqueue_watch(queues, {turn.threshold->source, turn.event.origin});
        }
      } else {
        const auto &watch = std::get<step::WatchWork>(item->work);
        step::cancel_source_thresholds(timers, f.subscriptions, watch.source);
        auto planned = step::plan_watch(f.subscriptions, f.values(), f.allocations, f.triggers,
                                        f.scripts, watch, now, 20);
        require(planned.modeled && !planned.shared_gap && planned.invocations.size() == 2,
                "one source watch plan delivers exactly the two current explicit subscriptions");
        for (auto &timer : planned.changes.timers)
          step::schedule(timers, std::move(timer));
        for (auto &plan : planned.invocations) {
          plan.input.deadline = step::Monotonic::now() + std::chrono::seconds(2);
          auto completed = invoke(f, plan, journal);
          step::publish_invocation(journal, queues, timers, std::move(completed), watch.origin,
                                   1000);
        }
      }
    }
  };
  crossing(21);
  for (auto key : std::array<ScriptKey, 2>{{{f.a, direct}, {f.a, sibling}}})
    require(std::get<double>(f.states.at(key).at(std::string("count"))) == 1 &&
                std::abs(std::get<double>(f.states.at(key).at(std::string("last"))) - 1.05) < 1e-9,
            "both handlers receive the actual sampled source scalar once");
  require(!f.states.contains({f.a, bare}) && !f.states.contains({f.b, bare}),
          "bare quantity siblings never consume ordinary threshold work");
  for (const auto &binding : f.subscriptions.rows.at({f.a, 1}))
    require(std::abs(std::get<react::Answer>(binding.last).value.numeric() - 1.05) < 1e-9,
            "actual watch planning updates the saved delivered answer");
  auto next = step::timers(timers);
  require(next.size() == 2 && next[0].due == 41 && next[1].due == 41,
          "coalesced source evaluation rearms one future token per consumer");
  crossing(41);
  require(std::get<double>(f.states.at({f.a, direct}).at(std::string("count"))) == 2 &&
              std::get<double>(f.states.at({f.a, sibling}).at(std::string("count"))) == 2 &&
              step::timers(timers).size() == 2 &&
              !step::valid_event({obsolete.event, obsolete.threshold}, identity(f.objects),
                                 f.processes, f.subscriptions, 41, 20),
          "current predicate/last answer reject a stale token without version machinery");
  snap::Buffer bytes;
  snap::Output out{bytes};
  react::write_script_states(out, f.states, f.subscriptions);
  out.adapter().flush();
  bytes.resize(out.adapter().writtenBytesCount());
  react::ScriptStates states;
  react::Subscriptions subscriptions;
  snap::Input in{bytes.data(), bytes.size()};
  react::read_script_states(in, states, subscriptions);
  snap::check_complete(in);
  for (const auto &timer : step::timers(timers))
    require(react::valid_threshold(*timer.threshold, identity(f.objects), f.processes,
                                   subscriptions, timer.due, 20),
            "restored actual subscriptions retain future crossing semantics");
  auto records = journal.take();
  require(records.size() == 4 && records[0]["cause"] == 9 && records[0]["root"] == 3,
          "threshold handlers retain timer provenance without fake chosen input actions");

  const std::size_t rearms = benchmark ? 10000 : 1000;
  const auto started = step::Monotonic::now();
  for (std::size_t i = 0; i < rearms; ++i) {
    auto plans = step::plan_event(f.triggers, f.values(),
                                  {f.a, {}, 6, Value(i % 2 ? .000001 : .000002), origin}, 41, 20);
    require(plans.size() == 1, "only the installed rate handler receives its event");
    plans.front().input.deadline = step::Monotonic::now() + std::chrono::seconds(2);
    auto completed = invoke(f, plans.front(), journal);
    step::cancel_source_thresholds(timers, f.subscriptions, {f.a, 1});
    const auto changed = step::plan_watch(f.subscriptions, f.values(), f.allocations, f.triggers,
                                          f.scripts, {{f.a, 1}, completed.origin}, 41, 20);
    require(changed.invocations.empty(),
            "rate changes preserving the sampled value do not wake Lua");
    for (const auto &timer : changed.changes.timers)
      completed.effects.ordered.emplace_back(timer);
    step::publish_invocation(journal, queues, timers, std::move(completed), origin, 1000);
    // The explicit source evaluation above already consumed this handler's dirty fact.
    while (auto item = step::pop_foreground(queues))
      require(std::holds_alternative<step::WatchWork>(item->work),
              "rate-only handler produces dirty source work, never extra action callbacks");
    require(timers.thresholds.size() == 2,
            "actual repeated Lua rates retain one pending token per subscribed script");
  }
  next = step::timers(timers);
  records = journal.take();
  require(next.size() == 2 && next.front().due > 1000000 && records.size() == rearms &&
              next.back().event.origin.cause == records.back()["id"] &&
              std::get<double>(f.states.at({f.a, direct}).at(std::string("count"))) == 2,
          "far-future replacements retain latest real origin without invented threshold callbacks");
  auto plans = step::plan_event(f.triggers, f.values(), {f.a, {}, 6, Value(-.5), origin}, 41, 20);
  plans.front().input.deadline = step::Monotonic::now() + std::chrono::seconds(2);
  auto completed = invoke(f, plans.front(), journal);
  step::cancel_source_thresholds(timers, f.subscriptions, {f.a, 1});
  const auto changed = step::plan_watch(f.subscriptions, f.values(), f.allocations, f.triggers,
                                        f.scripts, {{f.a, 1}, completed.origin}, 41, 20);
  for (const auto &timer : changed.changes.timers)
    completed.effects.ordered.emplace_back(timer);
  step::publish_invocation(journal, queues, timers, std::move(completed), origin, 1000);
  while (step::pop_foreground(queues)) {
  }
  crossing(step::timers(timers).front().due);
  require(std::get<double>(f.states.at({f.a, direct}).at(std::string("count"))) == 3 &&
              std::get<double>(f.states.at({f.a, sibling}).at(std::string("count"))) == 3 &&
              step::timers(timers).size() == 2,
          "next real crossing still reaches both scripts exactly once after replacements");
  // A stopped real process has no future crossing. Its old far-future tokens
  // must disappear, while unrelated ordinary timers remain independent.
  step::schedule(timers, {1000000000, {f.c, {}, 6, {}, origin}, {}});
  const auto stopped_token = step::timers(timers).front();
  const auto stopped_at = timers.now;
  plans = step::plan_event(f.triggers, f.values(), {f.a, {}, 6, Value(0.), origin}, stopped_at, 20);
  plans.front().input.deadline = step::Monotonic::now() + std::chrono::seconds(2);
  completed = invoke(f, plans.front(), journal);
  require(f.processes[1].get(f.a.slot())->rate == 0 && timers.thresholds.size() == 2,
          "actual Lua stopping a process exposes the two pending old crossings");
  step::cancel_source_thresholds(timers, f.subscriptions, {f.a, 1});
  const auto stopped = step::plan_watch(f.subscriptions, f.values(), f.allocations, f.triggers,
                                        f.scripts, {{f.a, 1}, completed.origin}, stopped_at, 20);
  require(stopped.changes.timers.empty() && stopped.invocations.empty() &&
              timers.thresholds.empty() && step::timers(timers).size() == 1 &&
              step::timers(timers).front().due == 1000000000 &&
              !step::valid_event({stopped_token.event, stopped_token.threshold},
                                 identity(f.objects), f.processes, f.subscriptions, 10000, 20),
          "zero rate cancels current bindings' tokens and preserves unrelated ordinary timers");
  step::publish_invocation(journal, queues, timers, std::move(completed), origin, 1000);
  while (auto item = step::pop_foreground(queues))
    require(std::holds_alternative<step::WatchWork>(item->work) &&
                std::get<step::WatchWork>(item->work).source == FactKey{f.a, 1},
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
    chosen_actions_and_typed_inputs();
    canonical_results_and_actual_bounds();
    threshold_plans_use_current_subscriptions(benchmark);
    std::cout
        << "Actual action planning, attribution, typed inputs, threshold plans and Lua pass.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
