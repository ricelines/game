#include "space_fixture.hpp"

#include "bindings/controls/controls.hpp"
#include "bindings/turns/turns.hpp"
#include "react/snapshot.hpp"
#include "runtime_fixture.hpp"

#include <chrono>
#include <iostream>

using namespace amber;
using test_runtime::require;
namespace {
void only_actual_player_actions_are_chosen_events() {
  test_runtime::Fixture f;
  f.objects.owner[f.a.slot()] = 1;
  f.objects.owner[f.b.slot()] = 1;
  f.objects.owner[f.c.slot()] = 2;
  f.objects.model[f.b.slot()] = 2;
  f.models.rows.resize(3);
  f.models.rows[2].id = 2;
  const auto script = f.add(R"(return {kind='model',actions={['novel.touch']={right='use'}},
    on={['novel.touch']=function(self)
      self.state.calls=(self.state.calls or 0)+1
      if self.state.calls==1 then self:act('novel.touch',self.handle) end
    end}})",
                            3);
  f.models.rows[1].script = script;
  install::project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  perm::Players players;
  players.rows.resize(3);
  perm::Grants grants;
  const auto holder = perm::Holder::script(script, f.a);
  const auto scope = perm::Scope::object(f.a);
  const auto grant = perm::create_grant(
      grants, players, {1, holder, scope, perm::bit(perm::Verb::Use), 0, 0, 1000},
      {scope, 1, perm::all_rights, 0, false}, {holder, 1, true}, 0);
  require(bool(grant), "real script action requires its explicit object grant");
  control::Profiles profiles;
  profiles.rows.resize(3);
  profiles.rows[1].id = 1;
  profiles.rows[1].body = f.b;
  profiles.rows[2].id = 2;
  profiles.rows[2].body = f.c;
  Relation parts, grasped;
  bindings::Execution execution{f.vm, f.states, f.random, identity(f.objects), parts, 7};
  drive::StructuralOperations structural;
  drive::Clock clock;
  clock.step = 12;
  record::Journal journal(8, {});
  step::WorkQueues queues;
  step::TimerWheel timers;
  std::vector<FactKey> mutated;
  std::vector<bindings::GapDraft> gaps;
  react::MeterRows meters;
  react::PriorityLinks priorities;
  bindings::AllocationReconciliation allocation{f.allocations, meters, priorities};
  bindings::HandlerTurns handlers{execution,
                                  structural,
                                  f.triggers,
                                  *f.projection,
                                  {f.subscriptions, f.links, f.scripts, f.rules},
                                  journal,
                                  queues,
                                  timers,
                                  mutated,
                                  clock,
                                  20,
                                  1000,
                                  allocation};
  command::NativeActions declarations;
  drive::NativeOperations native;
  perm::OwnerLimits limits;
  step::Intents intents;
  space::Space physical{testspace::settings()};
  bindings::ActionTurns actions{handlers,
                                declarations,
                                native,
                                f.models,
                                f.scripts,
                                f.installed,
                                f.rules,
                                profiles,
                                authority(f.objects),
                                transforms(f.objects),
                                limits,
                                players,
                                grants,
                                physical,
                                grasped,
                                intents,
                                gaps,
                                2};
  const auto root = bindings::perform_action(actions, {PlayerId{1}, 7, f.a, Value{}}, {}, {});
  require(root.reply.value("effect", "") == "applied" && queues.causal.size() == 1,
          "actual chosen player action executes Lua and queues a real descendant action");
  auto queued = step::pop_foreground(queues);
  require(queued && std::holds_alternative<step::ActionWork>(queued->work),
          "Lua self:act emits typed action work");
  auto descendant = std::get<step::ActionWork>(std::move(queued->work));
  const auto parent = descendant.origin;
  require(std::get<ScriptHolder>(descendant.request.caller) == ScriptHolder{script, f.a},
          "queued action retains the actual installed script holder");
  bindings::consume_action(&actions, std::move(descendant));
  auto records = journal.take();
  require(records.size() == 4 && records[0]["kind"] == "action" && records[0]["actor"] == "p:1" &&
              records[0]["agent"] == true && records[0]["cause"].is_null() &&
              records[0]["root"] == records[0]["id"] && records[0]["result"] == "applied",
          "player input alone is chosen, with its actual root actor and applied result");
  require(
      records[2]["kind"] == "action" && records[2]["actor"] == "s:" + std::to_string(script) &&
          records[2]["agent"] == false && records[2]["cause"] == records[1]["id"] &&
          records[2]["cause"] == parent.cause && records[2]["root"] == records[0]["id"] &&
          records[2]["result"] == "applied" && records[3]["cause"] == records[2]["id"] &&
          std::get<double>(f.states.at({f.a, script}).at(std::string("calls"))) == 2 &&
          queues.causal.empty(),
      "real nested script action is nonchosen while actor, handler cause, root and effect survive");
  perm::revoke(grants, players, grant.grant);
  const auto denied =
      bindings::perform_action(actions, {ScriptHolder{script, f.a}, 7, f.a, Value{}}, {}, parent);
  require(denied.reply.contains("denied"), "revoked script grant denies the real action pipeline");
  const auto player_denied =
      bindings::perform_action(actions, {PlayerId{2}, 7, f.a, Value{}}, {}, {});
  require(player_denied.reply.contains("denied"), "foreign player without Use is denied");
  records = journal.take();
  require(records.size() == 2 && records[0]["agent"] == false &&
              records[0]["actor"] == "s:" + std::to_string(script) &&
              records[0]["cause"] == parent.cause && records[0]["root"] == parent.root &&
              records[0]["result"] == "denied" && records[1]["agent"] == true &&
              records[1]["actor"] == "p:2" && records[1]["cause"].is_null() &&
              records[1]["result"] == "denied" &&
              std::get<double>(f.states.at({f.a, script}).at(std::string("calls"))) == 2,
          "denial preserves chosen classification and causal identity without invoking handlers");

  f.terms.rows[1].dimensions[1] = 1;
  f.terms.rows[7].inputs = {{"amount", react::InputType::Number, 1, false},
                            {"note", react::InputType::String, 0, true}};
  const auto typed = f.add(R"(return {kind='model',actions={['novel.touch']={right='use'}},on={
    ['novel.touch']=function(self,_,args)
      self.state.amount=args.amount;self.state.note=args.note or 'omitted'
    end,
    ['novel.cue']=function(self) self:act('novel.touch',self.handle,{amount='wrong'}) end,
  }})");
  f.models.rows[1].script = typed;
  install::project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  const std::string signature = "novel.touch(amount: number [kg], note?: string)";
  const auto missing = bindings::perform_action(actions, {PlayerId{1}, 7, f.a, Value{}}, {}, {});
  require(missing.reply["error"] == "bad_input" && missing.reply["signature"] == signature &&
              missing.reply["problem"].get<std::string>().find("amount") != std::string::npos &&
              !f.states.contains({f.a, typed}),
          "typed action validation must return its real signature before calling Lua");
  const auto accepted = bindings::perform_action(
      actions, {PlayerId{1}, 7, f.a, std::vector<EventField>{{"amount", Value(2.5)}}}, {}, {});
  require(accepted.reply["effect"] == "applied" &&
              std::get<double>(f.states.at({f.a, typed}).at(std::string("amount"))) == 2.5 &&
              std::get<std::string>(f.states.at({f.a, typed}).at(std::string("note"))) == "omitted",
          "the advertised typed signature must invoke actual Lua with its optional field absent");

  bindings::consume_event(&handlers, {step::EventWork{f.a, {}, 6, {}, {}}, {}});
  auto malformed = step::pop_foreground(queues);
  require(malformed && std::holds_alternative<step::ActionWork>(malformed->work),
          "actual Lua action must reach the normal action queue");
  auto bad_action = std::get<step::ActionWork>(std::move(malformed->work));
  const auto rejected = bindings::perform_action(
      actions, bad_action.request, observe::event_payload_json(bad_action.request.args),
      bad_action.origin);
  require(
      rejected.reply["error"] == "bad_input" && rejected.reply["signature"] == signature &&
          std::get<double>(f.states.at({f.a, typed}).at(std::string("amount"))) == 2.5,
      "malformed actual Lua self:act must report the same signature without invoking its target");

  observe::Hearing hearing;
  bindings::DoInput input_binding{actions, hearing, 40};
  drive::Input input;
  input.caller = PlayerId{1};
  input.body = f.b;
  journal.take();
  input.fields = {{"action", "novel.touch"},
                  {"target", to_string(f.a)},
                  {"args", {{"amount", "not a number"}}}};
  const auto action_response = [](nlohmann::json reply) {
    reply.erase("events");
    reply.erase("events_omitted");
    return reply;
  };
  const auto decode_error = bindings::do_input(&input_binding, input).fields;
  require(decode_error["error"] == "bad_input" && decode_error["signature"] == signature &&
              decode_error["problem"].get<std::string>().find("amount") != std::string::npos,
          "raw JSON decode failure must include the actual action signature and input name");
  records = journal.take();
  require(records.size() == 1 && records[0]["kind"] == "action" && records[0]["actor"] == "p:1" &&
              records[0]["agent"] == true && records[0]["name"] == "novel.touch" &&
              records[0]["changed"] == false && records[0]["result"] == "denied" &&
              records[0]["cause"].is_null() && records[0]["root"] == records[0]["id"] &&
              records[0]["args"] == input.fields["args"] &&
              records[0]["raw_target"] == to_string(f.a) && records[0]["things"].empty() &&
              records[0]["response"] == action_response(decode_error),
          "accepted malformed DO must record exactly one chosen denial with raw diagnostics");
  for (const auto &fields : std::vector<nlohmann::json>{
           {{"action", "novel.unknown"}, {"target", "not a handle"}, {"args", {{"x", 2}}}},
           {{"action", "novel.touch"}, {"target", "not a handle"}, {"args", {{"amount", 3}}}},
           {{"action", "novel.touch"}, {"target", "self"}, {"args", {{"unknown", 3}}}}}) {
    input.fields = fields;
    const auto invalid = bindings::do_input(&input_binding, input).fields;
    records = journal.take();
    require(invalid["error"] == "bad_input" && records.size() == 1 &&
                records[0]["name"] == fields["action"] && records[0]["args"] == fields["args"] &&
                records[0]["raw_target"] == fields["target"] && records[0]["agent"] == true &&
                records[0]["result"] == "denied" && records[0]["changed"] == false &&
                records[0]["things"].empty() &&
                records[0]["response"] == action_response(invalid) &&
                std::get<double>(f.states.at({f.a, typed}).at(std::string("amount"))) == 2.5,
            "unknown action, malformed target or input must retain the exact accepted choice");
    require(invalid.contains("signature") == (fields["action"] == "novel.touch"),
            "only known action terms have a truthful signature");
  }
  input.fields = {
      {"action", "novel.touch"}, {"target", to_string(f.a)}, {"args", nlohmann::json::object()}};
  const auto required_error = bindings::do_input(&input_binding, input).fields;
  records = journal.take();
  require(required_error["error"] == "bad_input" && records.size() == 1 &&
              records[0]["result"] == "denied" &&
              records[0]["response"] == action_response(required_error) &&
              records[0]["things"] == nlohmann::json::array({to_string(f.a)}),
          "post-decode action validation must publish its existing denial only once");
  input.fields["args"] = {{"amount", 4}};
  const auto valid_input = bindings::do_input(&input_binding, input).fields;
  records = journal.take();
  require(valid_input["effect"] == "applied" && records.size() == 2 &&
              records[0]["kind"] == "action" && records[0]["agent"] == true &&
              records[0]["result"] == "applied" && records[1]["kind"] == "handler" &&
              records[1]["cause"] == records[0]["id"] &&
              std::get<double>(f.states.at({f.a, typed}).at(std::string("amount"))) == 4,
          "valid accepted DO must execute the actual VM and retain its normal action/handler pair");

  const auto native_id = command::declare_native(
      declarations, f.terms, "engine.typed_example", "Native descriptor regression", "", false,
      {{"type", "object"},
       {"required", {"amount"}},
       {"properties", {{"amount", {{"type", "integer"}}}}}});
  drive::register_native(native, native_id,
                         {[](const nlohmann::json &) { return std::string("amount is missing"); },
                          [](void *, const drive::NativeCall &) -> drive::NativeCompletion {
                            throw std::runtime_error(
                                "native mutation must not run for invalid arguments");
                          },
                          nullptr});
  const auto native_error =
      bindings::perform_action(actions, {PlayerId{1}, native_id, {}, Value{}}, {}, {});
  require(native_error.reply["error"] == "bad_input" &&
              native_error.reply["signature"] == "engine.typed_example(amount: integer)" &&
              native_error.reply["problem"] == "amount is missing",
          "native descriptor validation must return the signature before its mutation adapter");
}
void threshold_turn_uses_current_subscriptions_and_one_rearm(bool benchmark = false) {
  test_runtime::Fixture f;
  f.state(f.a, 1, 0);
  react::set_rate(f.properties, f.processes, f.terms.rows[1], f.a, 1, 0, 20, 0, 20, f.effects);
  auto direct = f.add(R"(return {kind='instance',watches={{term='novel.heat',thresholds={
    {predicate='above',value=1},{predicate='above',value=2},{predicate='above',value=3}}}},
    on={['novel.heat']=function(self,source,value)
      assert(type(value)=='number' and value>1)
      self.state.count=(self.state.count or 0)+1
      self.state.last=value;self.state.read=self:read(source,'novel.heat')
    end,['novel.cue']=function(self,_,rate)
      self:rate('novel.heat',rate,0,20)
    end}})");
  auto along =
      f.add(R"(return {kind='instance',watches={{term='novel.heat',along='novel.link',thresholds={
    {predicate='above',value=1},{predicate='above',value=2},{predicate='above',value=3}}}},
    on={['novel.heat']=function(self,source,value)
      assert(source~=self.handle and type(value)=='number' and value>1)
      self.state.count=(self.state.count or 0)+1
      self.state.last=value;self.state.read=self:read(source,'novel.heat')
    end}})");
  auto bare = f.add(R"(return {kind='model',on={['novel.heat']=function(self)
    error('bare quantity handler is not a threshold subscriber')end}})");
  f.models.rows[1].script = bare;
  install::project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  const std::array<ScriptId, 1> own{direct}, followed{along};
  install::set_instances(f.installed, f.a, own, f.scripts);
  install::set_instances(f.installed, f.b, followed, f.scripts);
  install::project_instance(f.triggers, f.installed, f.scripts, f.terms, f.a);
  install::project_instance(f.triggers, f.installed, f.scripts, f.terms, f.b);
  react::set_relation(f.relations.at(3), 3, f.b, f.a, true, f.effects);
  f.bind(direct, f.a);
  f.bind(along, f.b);
  step::WorkQueues queues;
  step::TimerWheel timers;
  const record::Origin origin{9, 3, {1, 2}, 1.5, false};
  auto due = react::threshold_timers(f.subscriptions, f.processes, {f.a, 1}, 0, 20, origin);
  require(due.size() == 2 && due[0].due == 21 && due[1].due == 21,
          "two distinct real bindings arm the same source crossing");
  const auto obsolete = due[0];
  for (auto &timer : due)
    step::schedule(timers, std::move(timer));
  Relation parts;
  bindings::Execution execution{f.vm, f.states, f.random, identity(f.objects), parts, 7};
  drive::StructuralOperations structural;
  drive::Clock clock;
  record::Journal journal(8, {});
  journal.restore_next_id(10);
  std::vector<FactKey> mutated;
  std::vector<bindings::GapDraft> gaps;
  react::MeterRows meters;
  react::PriorityLinks priorities;
  bindings::AllocationReconciliation allocation{f.allocations, meters, priorities};
  bindings::HandlerTurns handlers{execution,
                                  structural,
                                  f.triggers,
                                  *f.projection,
                                  {f.subscriptions, f.links, f.scripts, f.rules},
                                  journal,
                                  queues,
                                  timers,
                                  mutated,
                                  clock,
                                  20,
                                  1000,
                                  allocation};
  bindings::WatchTurns watches{handlers, gaps};
  auto advance = [&](Step step) {
    clock.step = step;
    step::promote_due(timers, step, queues);
    drive::Frame frame;
    frame.handler_deadline = drive::Monotonic::now() + std::chrono::seconds(2);
    drive::drain_foreground(queues, frame, {&handlers, bindings::consume_event},
                            {&watches, bindings::consume_watch}, {}, {});
  };
  advance(21);
  for (auto key : std::array<ScriptKey, 2>{{{f.a, direct}, {f.b, along}}}) {
    const auto &state = f.states.at(key);
    require(
        std::get<double>(state.at(std::string("count"))) == 1 &&
            std::abs(std::get<double>(state.at(std::string("last"))) - 1.05) < 1e-9 &&
            std::get<double>(state.at(std::string("last"))) ==
                std::get<double>(state.at(std::string("read"))),
        "threshold uses the changed scalar payload and acquired followed-source read in real Lua");
  }
  require(!f.states.contains({f.a, bare}) && !f.states.contains({f.b, bare}) && gaps.empty(),
          "timer routing cannot dispatch bare quantity siblings or report a false shared gap");
  for (const auto &binding : f.subscriptions.rows.at({f.a, 1}))
    require(std::abs(std::get<react::Answer>(binding.last).value.numeric() - 1.05) < 1e-9,
            "threshold path updates authoritative last-delivered answers");
  auto next = step::timers(timers);
  require(next.size() == 2 && next[0].due == 41 && next[1].due == 41,
          "sibling due tokens rearm exactly one next timer per binding");
  advance(41);
  require(std::get<double>(f.states.at({f.a, direct}).at(std::string("count"))) == 2 &&
              std::get<double>(f.states.at({f.b, along}).at(std::string("count"))) == 2 &&
              step::timers(timers).size() == 2,
          "later crossings retain one actual callback and bounded timer count per consumer");
  bindings::consume_event(&handlers, {obsolete.event, obsolete.threshold});
  require(queues.causal.empty(), "obsolete observed-answer token cannot enqueue another watch");
  auto binding = f.subscriptions.rows.at({f.a, 1})[0];
  const auto previous_high_water = f.subscriptions.next_revision;
  react::unbind_watch(f.subscriptions, binding.consumer, binding.index);
  react::bind_watch(f.subscriptions, {f.a, 1}, binding);
  const auto &rebound = f.subscriptions.rows.at({f.a, 1}).back();
  require(rebound.revision >= previous_high_water &&
              f.subscriptions.next_revision > rebound.revision,
          "answer epochs and source rebinding share one monotonic revision high water");
  snap::Buffer bytes;
  snap::Output out{bytes};
  react::write_script_states(out, f.states, f.subscriptions);
  out.adapter().flush();
  bytes.resize(out.adapter().writtenBytesCount());
  react::ScriptStates loaded_states;
  react::Subscriptions loaded_subscriptions;
  snap::Input in{bytes.data(), bytes.size()};
  react::read_script_states(in, loaded_states, loaded_subscriptions);
  snap::check_complete(in);
  auto rearmed = react::threshold_timers(f.subscriptions, f.processes, {f.a, 1}, 41, 20, origin);
  require(loaded_subscriptions.next_revision == f.subscriptions.next_revision &&
              std::all_of(rearmed.begin(), rearmed.end(),
                          [&](const auto &timer) {
                            return react::valid_threshold(*timer.threshold, identity(f.objects),
                                                          f.processes, loaded_subscriptions);
                          }),
          "restored observation epochs preserve future timer validity without revision reuse");
  auto records = journal.take();
  require(records.size() == 4 && records[0]["cause"] == 9 && records[0]["root"] == 3,
          "threshold-driven handler records retain timer provenance without fake input actions");
  const std::size_t rearms = benchmark ? 10000 : 1000;
  std::vector<double> samples;
  samples.reserve(rearms / 100);
  auto sample_start = drive::Monotonic::now();
  for (std::size_t i = 0; i < rearms; ++i) {
    bindings::consume_event(
        &handlers, {step::EventWork{f.a, {}, 6, Value(i % 2 ? .000001 : .000002), origin}, {}});
    advance(41);
    require(timers.thresholds.size() == 2,
            "actual repeated Lua rate changes retain one pending threshold per own/along watch");
    if ((i + 1) % 100 == 0) {
      const auto now = drive::Monotonic::now();
      samples.push_back(std::chrono::duration<double, std::micro>(now - sample_start).count() /
                        100);
      sample_start = now;
    }
  }
  next = step::timers(timers);
  records = journal.take();
  require(next.size() == 2 && next.front().due > 1000000 && records.size() == rearms &&
              next.back().event.origin.cause == records.back()["id"] &&
              std::get<double>(f.states.at({f.a, direct}).at(std::string("count"))) == 2,
          "far-future replacement keeps exact latest Origin without inventing threshold callbacks");
  bindings::consume_event(&handlers, {step::EventWork{f.a, {}, 6, Value(-.5), origin}, {}});
  advance(41);
  const auto crossing = step::timers(timers).front().due;
  advance(crossing);
  require(std::get<double>(f.states.at({f.a, direct}).at(std::string("count"))) == 3 &&
              std::get<double>(f.states.at({f.b, along}).at(std::string("count"))) == 3 &&
              step::timers(timers).size() == 2,
          "replacement still delivers the next genuine own/along threshold through real Lua once");
  if (benchmark) {
    std::sort(samples.begin(), samples.end());
    std::cout << "actual Lua rate + watch + Journal + threshold replacement: rearms=" << rearms
              << " pending=" << timers.thresholds.size()
              << " us/op p50=" << samples[samples.size() / 2]
              << " p95=" << samples[samples.size() * 95 / 100] << " max=" << samples.back()
              << " (100-operation batch means)\n";
  }
}
} // namespace
int main(int argc, char **argv) {
  try {
    only_actual_player_actions_are_chosen_events();
    const bool benchmark = argc == 2 && std::string_view(argv[1]) == "--bench";
    require(argc == 1 || benchmark, "usage: turns_test [--bench]");
    threshold_turn_uses_current_subscriptions_and_one_rearm(benchmark);
    std::cout
        << "Actual action attribution, threshold turn, followed-source and rearm checks passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
