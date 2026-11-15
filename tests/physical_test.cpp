#include "physical_fixture.hpp"

using namespace amber;
using test_runtime::require;
namespace {
void physical_primitives_are_script_only() {
  test_physical::Fixture f;
  for (const auto *name :
       {"engine.grasp", "engine.release", "engine.attach", "engine.impulse", "engine.transfer"}) {
    const auto run = f.begin_as(PlayerId{1}, name, f.f.b, nlohmann::json::object());
    require(
        run.outcome && run.outcome->reply["ok"] == false &&
            run.outcome->reply["error"] == "bad_input" &&
            run.outcome->reply["problem"] ==
                "engine.grasp, release, attach, impulse and transfer are called by a body script",
        "every physical primitive rejects player callers before permission or argument checks");
  }
  step::declare_commerce(f.f.terms, f.declarations);
  step::declare_admin(f.f.terms, f.declarations);
  const auto listed = net::player_action_ids(f.declarations, f.f.values());
  for (const auto id : listed)
    require(!std::holds_alternative<react::MechanicAction>(f.declarations.rows.at(id).operation),
            "player discovery includes no physical primitives");
  for (const auto *name : {"engine.give", "engine.offer", "engine.grant"})
    require(std::ranges::find(listed, f.f.terms.by_name.at(name)) != listed.end(),
            "ordinary native player actions remain discoverable");
}
void ordinary_transfer_and_live_processes() {
  test_physical::Fixture f;
  f.load("return {kind='model',on={['novel.touch']=function(self)end}}");
  f.f.properties[1].set(f.f.b.slot(), Value(5.));
  f.f.properties[1].set(f.f.c.slot(), Value(1.));
  const auto args = nlohmann::json{{"to", to_string(f.f.c)}, {"term", "novel.heat"}, {"amount", 2}};
  auto result = f.transfer(f.f.b, args);
  require(result.reply.contains("denied") && f.f.properties[1].get(f.f.b.slot())->numeric() == 5,
          "a body-installed script cannot borrow its owner's source rights");
  f.grant(f.f.b, perm::bit(perm::Verb::Move));
  result = f.transfer(f.f.b, args);
  require(result.reply.contains("denied") && f.f.properties[1].get(f.f.b.slot())->numeric() == 5,
          "receiver Use is checked before moving the quantity");
  f.f.objects.preset[f.f.c.slot()] = perm::bit(perm::Verb::Use);
  result = f.transfer(f.f.b, args);
  require(result.reply.at("ok") && result.reply.at("amount") == 2 &&
              f.f.properties[1].get(f.f.b.slot())->numeric() == 3 &&
              f.f.properties[1].get(f.f.c.slot())->numeric() == 3,
          "ordinary action transfers the exact conserved quantity");
  f.f.processes[1].set(f.f.b.slot(), react::Process{5, 0, -1, 0, 10});
  f.f.processes[1].set(f.f.c.slot(), react::Process{1, 0, 1, 0, 10});
  f.clock.step = 20;
  result = f.transfer(f.f.b, args);
  require(
      result.reply.at("ok") &&
          react::read_state(f.f.properties, f.f.processes, f.f.b, 1, 20, 20).value.numeric() == 2 &&
          react::read_state(f.f.properties, f.f.processes, f.f.c, 1, 20, 20).value.numeric() == 4 &&
          f.f.processes[1].get(f.f.b.slot())->rate == -1 &&
          f.f.processes[1].get(f.f.c.slot())->rate == 1,
      "transfer settles sampled values and preserves both rates");
  f.f.processes[1].set(f.f.c.slot(), react::Process{9, 20, 1, 0, 10});
  result = f.transfer(f.f.b, args);
  require(result.reply.at("ok") && result.reply.at("amount") == 1 &&
              react::read_state(f.f.properties, f.f.processes, f.f.b, 1, 20, 20).value.numeric() ==
                  1 &&
              react::read_state(f.f.properties, f.f.processes, f.f.c, 1, 20, 20).value.numeric() ==
                  10 &&
              f.f.processes[1].get(f.f.b.slot())->rate == -1 &&
              f.f.processes[1].get(f.f.c.slot())->rate == 1,
          "destination process ceiling clamps actual transfer without losing quantity or rates");
  result = f.transfer(f.f.b, args);
  require(result.reply.at("ok") && result.reply.at("amount") == 0 &&
              react::read_state(f.f.properties, f.f.processes, f.f.b, 1, 20, 20).value.numeric() ==
                  1,
          "a full destination leaves the source unchanged");
  f.f.processes[1].erase(f.f.b.slot());
  f.f.processes[1].erase(f.f.c.slot());
  f.f.properties[1].set(f.f.b.slot(), Value(5.));
  f.f.properties[1].set(f.f.c.slot(), Value(1.));
  auto bounded = args;
  bounded["amount"] = 10;
  bounded["to_max"] = 2;
  result = f.transfer(f.f.b, bounded);
  require(result.reply.at("amount") == 1 && f.f.properties[1].get(f.f.b.slot())->numeric() == 4 &&
              f.f.properties[1].get(f.f.c.slot())->numeric() == 2,
          "explicit receiver bound uses the conserved term's units and returns actual debit");
  bounded["to_max"] = 100;
  result = f.transfer(f.f.b, bounded);
  require(result.reply.at("amount") == 4 && f.f.properties[1].get(f.f.b.slot())->numeric() == 0 &&
              f.f.properties[1].get(f.f.c.slot())->numeric() == 6,
          "source availability caps a larger request and preserves the exact total");
  f.f.processes[1].set(f.f.b.slot(), react::Process{3, 20, -1, 2, 10});
  f.f.properties[1].set(f.f.c.slot(), Value(0.));
  result = f.transfer(f.f.b, bounded);
  require(result.reply.at("amount") == 1 &&
              react::read_state(f.f.properties, f.f.processes, f.f.b, 1, 20, 20).value.numeric() ==
                  2 &&
              f.f.properties[1].get(f.f.c.slot())->numeric() == 1,
          "source process floor remains in its own reservoir");
  f.f.objects.preset[f.f.c.slot()] = 0;
  result = f.transfer(f.f.b, bounded);
  require(result.reply.contains("denied") &&
              react::read_state(f.f.properties, f.f.processes, f.f.b, 1, 20, 20).value.numeric() ==
                  2 &&
              f.f.properties[1].get(f.f.c.slot())->numeric() == 1,
          "both endpoints stay unchanged when destination authority is revoked");
}
void ordinary_grasp_release_seat_and_flight() {
  test_physical::Fixture f;
  f.load("return {kind='model',on={['novel.touch']=function(self)end}}");
  auto result = f.grasp(f.f.b, {{"local_pose", {{"position", {0, 1, 1}}}}});
  require(!result.reply.at("ok").get<bool>() && !f.f.objects.parent[f.f.b.slot()],
          "ordinary admission denies Take without inheriting the owner's rights");
  const auto take = f.grant(f.f.b, perm::bit(perm::Verb::Take));
  result = f.grasp(f.f.b, {{"local_pose", {{"position", {0, 1, 1}}}}});
  require(result.reply.at("effect") == "applied" && f.f.objects.parent[f.f.b.slot()] == f.f.a,
          "explicit Take admits actual Jolt grasp");
  perm::revoke(f.grants, f.players, take);
  result = f.release(f.f.b, {{"linear", {3, 0, 2}}});
  require(result.reply.at("effect") == "applied" && !(f.f.objects.flags[f.f.b.slot()] & Held),
          "releasing an item held here clears real held state without owner fallback");
  const auto before = f.f.objects.position[f.f.b.slot()];
  for (int i = 0; i < 5; ++i)
    f.tick();
  require(f.f.objects.position[f.f.b.slot()].x > before.x + .3f,
          "released item follows actual dynamic velocity");
  f.f.objects.preset[f.f.c.slot()] = perm::bit(perm::Verb::Use);
  const auto holder = perm::Holder::script(f.script, f.f.a);
  const auto scope = perm::Scope::avatar(f.f.a);
  const auto consent = perm::create_grant(f.grants, f.players, perm::Holder::player(1),
                                          {1, holder, scope, 1, 0, 0, 10000},
                                          {scope, 1, 3, 0, false}, {holder, 1, true}, f.clock.step);
  require(bool(consent), "the avatar owner explicitly consents to animation");
  result =
      f.attach(f.f.a, {{"parent", to_string(f.f.c)}, {"local_pose", {{"position", {0, 0, .6}}}}});
  require(result.reply.at("ok") && f.f.objects.parent[f.f.a.slot()] == f.f.c,
          "ordinary Attach seats the actual body after Avatar and parent Use checks");
  result = f.release(f.f.a);
  require(result.reply.at("ok") && !f.f.objects.parent[f.f.a.slot()] &&
              space::body_motion(f.space, identity(f.f.objects), f.f.a)->motion ==
                  space::Motion::Character,
          "detaching a body restores its character controller");
}
void own_motion_queries_and_speech() {
  test_physical::Fixture f;
  f.instance_texts.rows[f.f.a.bits].name = "Public speaker";
  f.listeners = {{1, f.f.a, {0, 0, 1}}};
  const auto script = f.load(R"(return {kind='model',actions={['novel.touch']={right='use'}},on={
    ['novel.touch']=function(self,_,args)
      if args.mode=='walk' then self:navigate(vector.create(4,0,0),{speed_m_s=0.3})
      elseif args.mode=='stop' then self:cancel_motion()
      elseif args.mode=='speech' then self:sound('hello',5,nil,{kind='speech'})
      else self.state.said=true;self:sound('hello',5,nil,{kind='speech'}) end
    end}})");
  auto result = f.invoke(std::vector<EventField>{{"mode", std::string("walk")}});
  require(result.outcome.status == react::RunOutcome::Status::Ok && !result.changed &&
              !f.intents.moving.empty() && f.intents.moving.at(f.f.a.bits).speed_m_s == .3f,
          "own navigation starts an intent with authored speed");
  result = f.invoke(std::vector<EventField>{{"mode", std::string("stop")}});
  require(!result.changed && f.intents.moving.empty(),
          "own intent control is not a stored world mutation");
  f.journal.take();
  result = f.invoke(std::vector<EventField>{{"mode", std::string("speech")}});
  require(!result.changed, "speech alone is not a stored world change");
  auto logs = f.journal.take();
  require(logs.back().at("kind") == "speech" && logs.back().at("actor") == "p:1" &&
              !logs.back().at("changed").get<bool>() && !logs.back().at("agent").get<bool>(),
          "explicit speech preserves player display and nonchosen event classification");
  std::size_t omitted = 0;
  auto heard = observe::take_heard(f.hearing, 1, 10, omitted);
  require(heard.size() == 1 && heard[0].sound.speech && heard[0].sound.name == "Public speaker",
          "heard speech carries canonical public speaker metadata");
  result = f.invoke(std::vector<EventField>{{"mode", std::string("stored")}});
  require(result.changed && std::get<bool>(f.f.states.at({f.f.a, script}).at(std::string("said"))),
          "speech preserves an independent earlier state write");
}
void declared_part_batch_and_foreign_rejection(bool benchmark = false) {
  test_physical::Fixture f;
  std::vector<Handle> parts;
  for (int index = 0; index < 300; ++index) {
    auto h = f.box({.03f, .08f, .03f}, {0, 0, 1}, space::Motion::Kinematic, false);
    parts.push_back(h);
    f.parts.in.set(h.slot(), std::vector<Handle>{f.f.a});
  }
  f.parts.out.set(f.f.a.slot(), parts);
  const auto script = f.load(R"(return {kind='model',actions={['novel.touch']={right='use'}},on={
    ['novel.touch']=function(self,selected,args)
      if args.mode=='foreign' then self:move_part(selected,vector.create(10,0,0),{x=0,y=0,z=0,w=1});return end
      self.state.phase=(self.state.phase or 0)+.1
      local rows={}
      for index,part in self:parts() do
        rows[index]={object=part,position=vector.create(index%20*.05,index//20*.05,1+math.sin(self.state.phase)*.2)}
      end
      self.state.count=self:move_part(rows)
    end}})");
  auto result = f.invoke(std::vector<EventField>{{"mode", std::string("batch")}});
  require(result.outcome.status == react::RunOutcome::Status::Ok && result.changed &&
              std::get<double>(f.f.states.at({f.f.a, script}).at(std::string("count"))) == 300,
          "one script batches all actual declared part poses without foreign Move grants");
  const auto before = f.f.objects.position[f.f.c.slot()];
  result = f.invoke(std::vector<EventField>{{"mode", std::string("foreign")}}, f.f.c);
  require(result.outcome.status == react::RunOutcome::Status::Error &&
              f.f.objects.position[f.f.c.slot()] == before,
          "acquisition does not confer own-part mutation authority");
  std::vector<double> costs;
  for (int index = 0; index < (benchmark ? 100 : 1); ++index) {
    auto start = std::chrono::steady_clock::now();
    result = f.invoke(std::vector<EventField>{{"mode", std::string("batch")}});
    require(result.outcome.status == react::RunOutcome::Status::Ok, "actual batch completes");
    costs.push_back(
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
            .count());
    f.journal.take();
    f.tick();
  }
  if (benchmark) {
    std::sort(costs.begin(), costs.end());
    std::cout << "ordinary_own_parts_300_us p50=" << costs[50] << " p95=" << costs[95]
              << " max=" << costs.back() << '\n';
  }
}
void exact_queries_do_not_grant_event_authority() {
  test_physical::Fixture f;
  f.load(R"(return {kind='model',actions={['novel.touch']={right='use'}},on={
    ['novel.touch']=function(self,sender,args)
      if args.mode=='related' then
        self:emit(self:related('novel.link','out')[1],'novel.cue',true)
      elseif args.mode=='event' then self:emit(sender,'novel.cue',true)
      else
        local nearby=world.near(self,5)
        assert(#nearby.objects>=2)
        local hit=world.raycast(vector.create(0,3,.2),vector.create(0,.5,.2))
        assert(hit.object~=nil)
        self.state.hit=hit.object
        self.state.world_readonly=not pcall(function() world.near=nil end)
        if args.mode=='emit_query' then self:emit(hit.object,'novel.cue',true) end
      end
    end}})");
  const auto read = f.invoke(std::vector<EventField>{{"mode", std::string("query")}});
  require(read.outcome.status == react::RunOutcome::Status::Ok &&
              std::get<Handle>(f.f.states.at({f.f.a, f.script}).at(std::string("hit"))) == f.f.b &&
              std::get<bool>(f.f.states.at({f.f.a, f.script}).at(std::string("world_readonly"))),
          "readonly world queries return actual geometry handles and readonly query methods");
  const auto denied = f.invoke(std::vector<EventField>{{"mode", std::string("emit_query")}});
  require(denied.outcome.status == react::RunOutcome::Status::Error && f.queues.causal.empty(),
          "an exact query grants no event mutation authority on an unrelated object");
  react::set_relation(f.f.relations.at(3), 3, f.f.a, f.f.b, true, f.f.effects);
  const auto related = f.invoke(std::vector<EventField>{{"mode", std::string("related")}});
  require(related.outcome.status == react::RunOutcome::Status::Ok && !f.queues.causal.empty(),
          "an actual related endpoint remains eligible for emitted events");
  while (step::pop_foreground(f.queues)) {
  }
  const auto event = f.invoke(std::vector<EventField>{{"mode", std::string("event")}}, f.f.c);
  require(event.outcome.status == react::RunOutcome::Status::Ok && !f.queues.causal.empty(),
          "the current event sender remains eligible for emitted events");
}
void native_query_deadline_keeps_effects_and_step_continues() {
  test_physical::Fixture f;
  f.load(R"(return {kind='model',on={['novel.touch']=function(self,_,args)
    self:set('novel.result', 7)
    if args and args.loop then while true do world.near(self, 10) end end
  end}})");
  const auto result = f.invoke(std::vector<EventField>{{"loop", Value(true)}});
  require(result.outcome.status == react::RunOutcome::Status::Cutoff &&
              f.f.properties[2].get(f.f.a.slot())->numeric() == 7,
          "native query loop cuts off and keeps prior effects");
  const auto records = f.journal.take();
  require(std::ranges::count_if(records,
                                [](const auto &row) {
                                  return row["kind"] == "system" && row["name"] == "cutoff";
                                }) == 1,
          "query cutoff is recorded exactly once");
  f.tick();
  require(f.invoke().outcome.status == react::RunOutcome::Status::Ok,
          "the next step can run a handler after a query cutoff");
}
void foreign_avatar_motion_requires_its_own_scope() {
  test_physical::Fixture f;
  f.load("return {kind='model',on={['novel.touch']=function(self)end}}");
  f.f.objects.flags[f.f.c.slot()] |= Body;
  f.f.objects.preset[f.f.c.slot()] = perm::bit(perm::Verb::Move);
  f.grant(f.f.c, perm::bit(perm::Verb::Move));
  const auto args = nlohmann::json{{"momentum", {4, 0, 0}}, {"point", {1, 0, .2}}};
  auto result = f.impulse(f.f.c, args);
  require(!result.reply.at("ok").get<bool>() && result.reply.at("denied").at("need") == "animate",
          "public and object Move cannot replace explicit foreign Avatar authority");
  const auto holder = perm::Holder::script(f.script, f.f.a);
  const auto scope = perm::Scope::avatar(f.f.c);
  const auto grant = perm::create_grant(f.grants, f.players, perm::Holder::player(2),
                                        {2, holder, scope, 1, 0, 0, 10000}, {scope, 2, 3, 0, false},
                                        {holder, 1, true}, 0);
  require(bool(grant), "avatar owner explicitly grants animation to the installed script");
  result = f.impulse(f.f.c, args);
  require(result.reply.at("ok") && result.reply.at("effect") == "applied",
          "the exact Avatar grant admits actual native motion");
  perm::revoke(f.grants, f.players, grant.grant);
  require(!f.impulse(f.f.c, args).reply.at("ok").get<bool>(),
          "revoking Avatar authority affects the next ordinary action");
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--bench") {
      declared_part_batch_and_foreign_rejection(true);
      return 0;
    }
    physical_primitives_are_script_only();
    ordinary_transfer_and_live_processes();
    ordinary_grasp_release_seat_and_flight();
    own_motion_queries_and_speech();
    declared_part_batch_and_foreign_rejection();
    exact_queries_do_not_grant_event_authority();
    native_query_deadline_keeps_effects_and_step_continues();
    foreign_avatar_motion_requires_its_own_scope();
    std::cout << "Ordinary physical actions and own-part Lua primitives passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
