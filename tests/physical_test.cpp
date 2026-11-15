#include "checked_actions_fixture.hpp"

using namespace amber;
using test_runtime::require;
namespace {
void ordinary_transfer_and_live_processes() {
  test_actions::Fixture f;
  f.load("return {kind='model',on={['novel.touch']=function(self)end}}");
  f.f.properties[1].set(f.f.b.slot(), Value(5.));
  f.f.properties[1].set(f.f.c.slot(), Value(1.));
  const auto args = nlohmann::json{{"to", to_string(f.f.c)}, {"term", "novel.heat"}, {"amount", 2}};
  auto result = f.act("engine.transfer", f.f.b, args);
  require(result.reply.contains("denied") && f.f.properties[1].get(f.f.b.slot())->numeric() == 5,
          "a body-installed script cannot borrow its owner's source rights");
  f.grant(f.f.b, perm::bit(perm::Verb::Move));
  result = f.act("engine.transfer", f.f.b, args);
  require(result.reply.contains("denied") && f.f.properties[1].get(f.f.b.slot())->numeric() == 5,
          "receiver Use is checked before moving the quantity");
  f.f.objects.preset[f.f.c.slot()] = perm::bit(perm::Verb::Use);
  result = f.act("engine.transfer", f.f.b, args);
  require(result.reply.at("ok") && result.reply.at("amount") == 2 &&
              f.f.properties[1].get(f.f.b.slot())->numeric() == 3 &&
              f.f.properties[1].get(f.f.c.slot())->numeric() == 3,
          "ordinary action transfers the exact conserved quantity");
  f.f.processes[1].set(f.f.b.slot(), react::Process{5, 0, -1, 0, 10, 1});
  f.f.processes[1].set(f.f.c.slot(), react::Process{1, 0, 1, 0, 10, 1});
  f.clock.step = 20;
  result = f.act("engine.transfer", f.f.b, args);
  require(
      result.reply.at("ok") &&
          react::read_state(f.f.properties, f.f.processes, f.f.b, 1, 20, 20).value.numeric() == 2 &&
          react::read_state(f.f.properties, f.f.processes, f.f.c, 1, 20, 20).value.numeric() == 4 &&
          f.f.processes[1].get(f.f.b.slot())->rate == -1 &&
          f.f.processes[1].get(f.f.c.slot())->rate == 1,
      "transfer settles sampled values and preserves both rates");
  f.f.processes[1].set(f.f.c.slot(), react::Process{9, 20, 1, 0, 10, 3});
  result = f.act("engine.transfer", f.f.b, args);
  require(
      !result.reply.at("ok").get<bool>() &&
          react::read_state(f.f.properties, f.f.processes, f.f.b, 1, 20, 20).value.numeric() == 2 &&
          react::read_state(f.f.properties, f.f.processes, f.f.c, 1, 20, 20).value.numeric() == 9,
      "a numerical bound violation leaves both endpoints unchanged");
}
void actual_body_script_nested_actions_and_flight() {
  test_actions::Fixture f;
  const auto script = f.load(R"(return {kind='model',actions={['novel.touch']={right='use'}},on={
    ['novel.touch']=function(self,selected,args)
      if args.mode=='hold' then
        local r=self:act('engine.grasp',selected,{local_pose={position=vector.create(0,1,1)}})
        self.state.accepted=r.ok
      elseif args.mode=='drop' then
        local r=self:act('engine.release',selected,{linear=vector.create(3,0,2)})
        assert(r.ok);self.state.released=true
      elseif args.mode=='seat' then
        assert(self:act('engine.attach',self.handle,{parent=selected,
          local_pose={position=vector.create(0,0,0.6)}}).ok)
      else assert(self:act('engine.release',self.handle).ok) end
    end}})");
  auto result = f.player(f.f.b, {{"mode", "hold"}});
  require(!f.f.objects.parent[f.f.b.slot()] &&
              !std::get<bool>(f.f.states.at({f.f.a, script}).at(std::string("accepted"))),
          "nested denied Take is returned to Lua before it continues");
  const auto take = f.grant(f.f.b, perm::bit(perm::Verb::Take));
  result = f.player(f.f.b, {{"mode", "hold"}});
  require(result.reply.at("effect") == "applied" && f.f.objects.parent[f.f.b.slot()] == f.f.a,
          "ordinary granted grasp changes actual Jolt attachment before Lua resumes");
  perm::revoke(f.grants, f.players, take);
  result = f.player(f.f.b, {{"mode", "drop"}});
  require(result.reply.at("effect") == "applied" && !(f.f.objects.flags[f.f.b.slot()] & Held),
          "ordinary release clears actual held state");
  auto before = f.f.objects.position[f.f.b.slot()];
  for (int step = 0; step < 5; ++step)
    f.tick();
  require(f.f.objects.position[f.f.b.slot()].x > before.x + .3f,
          "released object follows actual dynamic velocity");
  f.f.objects.preset[f.f.c.slot()] = perm::bit(perm::Verb::Use);
  result = f.player(f.f.c, {{"mode", "seat"}});
  require(f.f.objects.parent[f.f.a.slot()] == f.f.c, "ordinary attach seats the physical body");
  result = f.player({}, {{"mode", "stand"}});
  require(!f.f.objects.parent[f.f.a.slot()] &&
              space::body_motion(f.space, identity(f.f.objects), f.f.a)->motion ==
                  space::Motion::Character,
          "detaching a Body restores its character controller");
}
void own_motion_queries_and_speech() {
  test_actions::Fixture f;
  f.instance_texts.rows[f.f.a.bits].name = "Public speaker";
  f.listeners = {{1, f.f.a, {0, 0, 1}}};
  const auto script = f.load(R"(return {kind='model',actions={['novel.touch']={right='use'}},on={
    ['novel.touch']=function(self,_,args)
      if args.mode=='walk' then self:navigate(vector.create(4,0,0),{speed_m_s=0.3})
      elseif args.mode=='stop' then self:cancel_motion()
      elseif args.mode=='speech' then self:sound('hello',5,nil,{kind='speech'})
      else self.state.said=true;self:sound('hello',5,nil,{kind='speech'}) end
    end}})");
  auto result = f.player({}, {{"mode", "walk"}});
  require(result.reply.at("effect") == "started" && !f.intents.moving.empty() &&
              f.intents.moving.at(f.f.a.bits).speed_m_s == .3f,
          "own navigation starts an intent with authored speed");
  result = f.player({}, {{"mode", "stop"}});
  require(result.reply.at("effect") == "none" && f.intents.moving.empty(),
          "own intent control is not a stored world mutation");
  f.journal.take();
  result = f.player({}, {{"mode", "speech"}});
  require(result.reply.at("effect") == "none", "speech alone is not a stored world change");
  auto logs = f.journal.take();
  require(logs.back().at("kind") == "speech" && logs.back().at("actor") == "p:1" &&
              !logs.back().at("changed").get<bool>() && !logs.back().at("agent").get<bool>(),
          "explicit speech preserves player display and nonchosen event classification");
  std::size_t omitted = 0;
  auto heard = observe::take_heard(f.hearing, 1, 10, omitted);
  require(heard.size() == 1 && heard[0].sound.speech && heard[0].sound.name == "Public speaker",
          "heard speech carries canonical public speaker metadata");
  result = f.player({}, {{"mode", "stored"}});
  require(result.reply.at("effect") == "applied" &&
              std::get<bool>(f.f.states.at({f.f.a, script}).at(std::string("said"))),
          "speech preserves an independent earlier state write");
}
void declared_part_batch_and_foreign_rejection(bool benchmark = false) {
  test_actions::Fixture f;
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
  auto result = f.player();
  if (result.reply.contains("script_errors"))
    throw std::runtime_error(result.reply.dump());
  require(result.reply.at("effect") == "applied" &&
              std::get<double>(f.f.states.at({f.f.a, script}).at(std::string("count"))) == 300,
          "one script batches all actual declared part poses without foreign Move grants");
  const auto before = f.f.objects.position[f.f.c.slot()];
  result = f.player(f.f.c, {{"mode", "foreign"}});
  require(result.reply.contains("script_errors") && f.f.objects.position[f.f.c.slot()] == before,
          "acquisition does not confer own-part mutation authority");
  std::vector<double> costs;
  for (int index = 0; index < (benchmark ? 100 : 1); ++index) {
    auto start = std::chrono::steady_clock::now();
    result = f.player();
    require(!result.reply.contains("script_errors"), "actual batch completes");
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
  test_actions::Fixture f;
  f.f.properties[1].set(f.f.b.slot(), Value(7.));
  f.load(R"(return {kind='model',actions={['novel.touch']={right='use'}},on={
    ['novel.touch']=function(self,sender,args)
      if args.mode=='related' then
        self:emit(self:related('novel.link','out')[1],'novel.cue',true)
      elseif args.mode=='event' then self:emit(sender,'novel.cue',true)
      else
        local nearby=world.near(self,5)
        assert(nearby.complete and #nearby.objects>=2)
        local hit=world.raycast(vector.create(0,3,.2),vector.create(0,.5,.2))
        assert(hit.complete and hit.object~=nil)
        self.state.read=self:read(hit.object,'novel.heat')
        self.state.world_readonly=not pcall(function() world.near=nil end)
        if args.mode=='emit_query' then self:emit(hit.object,'novel.cue',true) end
      end
    end}})");
  const auto read = f.player({}, {{"mode", "read"}});
  require(!read.reply.contains("script_errors") &&
              std::get<double>(f.f.states.at({f.f.a, f.script}).at(std::string("read"))) == 7 &&
              std::get<bool>(f.f.states.at({f.f.a, f.script}).at(std::string("world_readonly"))),
          "readonly world queries return actual handles usable for readonly facts");
  const auto denied = f.player({}, {{"mode", "emit_query"}});
  require(denied.reply.contains("script_errors") && f.queues.causal.empty(),
          "an exact query grants no event mutation authority on an unrelated object");
  react::set_relation(f.f.relations.at(3), 3, f.f.a, f.f.b, true, f.f.effects);
  const auto related = f.player({}, {{"mode", "related"}});
  require(!related.reply.contains("script_errors") && !f.queues.causal.empty(),
          "an actual related endpoint remains eligible for emitted events");
  while (step::pop_foreground(f.queues)) {
  }
  const auto event = f.player(f.f.c, {{"mode", "event"}});
  require(!event.reply.contains("script_errors") && !f.queues.causal.empty(),
          "the current event sender remains eligible for emitted events");
}
void foreign_avatar_motion_requires_its_own_scope() {
  test_actions::Fixture f;
  f.load("return {kind='model',on={['novel.touch']=function(self)end}}");
  f.f.objects.flags[f.f.c.slot()] |= Body;
  f.f.objects.preset[f.f.c.slot()] = perm::bit(perm::Verb::Move);
  f.grant(f.f.c, perm::bit(perm::Verb::Move));
  const auto args = nlohmann::json{{"momentum", {4, 0, 0}}, {"point", {1, 0, .2}}};
  auto result = f.act("engine.impulse", f.f.c, args);
  require(!result.reply.at("ok").get<bool>() && result.reply.at("denied").at("need") == "animate",
          "public and object Move cannot replace explicit foreign Avatar authority");
  const auto holder = perm::Holder::script(f.script, f.f.a);
  const auto scope = perm::Scope::avatar(f.f.c);
  const auto grant = perm::create_grant(f.grants, f.players, perm::Holder::player(2),
                                        {2, holder, scope, 1, 0, 0, 10000}, {scope, 2, 3, 0, false},
                                        {holder, 1, true}, 0);
  require(bool(grant), "avatar owner explicitly grants animation to the installed script");
  result = f.act("engine.impulse", f.f.c, args);
  require(result.reply.at("ok") && result.reply.at("effect") == "applied",
          "the exact Avatar grant admits actual native motion");
  perm::revoke(f.grants, f.players, grant.grant);
  require(!f.act("engine.impulse", f.f.c, args).reply.at("ok").get<bool>(),
          "revoking Avatar authority affects the next ordinary action");
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--bench") {
      declared_part_batch_and_foreign_rejection(true);
      return 0;
    }
    ordinary_transfer_and_live_processes();
    actual_body_script_nested_actions_and_flight();
    own_motion_queries_and_speech();
    declared_part_batch_and_foreign_rejection();
    exact_queries_do_not_grant_event_authority();
    foreign_avatar_motion_requires_its_own_scope();
    std::cout << "Ordinary physical actions and own-part Lua primitives passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
