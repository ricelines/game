#include "catalog/catalog.hpp"
#include "physical_fixture.hpp"
#include "space_fixture.hpp"
#include "step/step.hpp"

using namespace testspace;
namespace bi = amber::step;
namespace re = amber::react;
namespace pe = amber::perm;
namespace {
struct Fixture : test_physical::Fixture {
  Handle body = f.a, object = f.b, wall;
  sp::StepResult last;
  explicit Fixture(sp::Settings config = settings(), sp::Motion mode = sp::Motion::Dynamic)
      : test_physical::Fixture(config, mode) {
    sp::set_pose(space, identity(f.objects), transforms(f.objects), body, {{-3, 0, 0}, {}});
    sp::set_pose(space, identity(f.objects), transforms(f.objects), object, {{0, 0, 1}, {}});
    wall = box({.4f, 3, 3}, {3, 0, 0});
    if (mode == sp::Motion::Kinematic) {
      parts.out.set(body.slot(), std::vector<Handle>{object});
      parts.in.set(object.slot(), std::vector<Handle>{body});
    }
    load(R"(return {kind='model',actions={['novel.touch']={right='use'}},on={
      ['novel.touch']=function(self,object,a)
        if a.mode=='route' then self:navigate(vector.create(2,0,0))
        elseif a.mode=='reroute' then self:navigate(vector.create(1,1,0))
        elseif a.mode=='cancel' then self:cancel_motion()
        elseif a.mode=='read_only' then local p=self:object(object).position
        elseif a.mode=='slide' or a.mode=='partial' then
          self:sweep(object,vector.create(0.2,0,0))
          if a.mode=='partial' then error('after completed operation') end
        end
      end}})",
         2);
    grant(object, pe::bit(pe::Verb::Take) | pe::bit(pe::Verb::Move));
    reach_m = 10;
  }
  HandlerResult run(const char *name, const amber::Origin &parent = {}, bool expect_error = false) {
    auto result = invoke(std::vector<EventField>{{"mode", std::string(name)}}, object, parent);
    require((result.outcome.status == re::RunOutcome::Status::Error) == expect_error,
            "actual Lua primitive handler status");
    return result;
  }
  void tick(std::span<const sp::MoveRequest> requests = {}) {
    sp::install_ready_navigation(space);
    for (const auto &request : requests)
      if (sp::needs_plan(space_state, identity(f.objects), request))
        sp::plan_move(space_state, identity(f.objects), request, {});
    sp::step(space, f.objects_state, physical, ++clock.step, .05f, requests, last);
    bi::publish_physical(step_state, journal, {14, {20, 21, 22, 23}}, 10000, last,
                         amber::step::Clock{.step = clock.step, .hz = 20});
    bi::finish_motion(step_state, last.intents);
  }
};
void final_navigation_and_immediate_relations() {
  Fixture f;
  auto moved = f.run("route");
  require(!moved.changed, "own navigation remains outside changed-world numerator");
  auto &intent = f.intents.moving.at(f.body.bits);
  require(intent.origin.cause == moved.origin.cause && intent.origin.root == moved.origin.root,
          "navigation receives final handler origin, not its empty incoming origin");
  const auto old = intent.id;
  auto replacement = f.run("reroute");
  require(f.endings.size() == 1 && f.endings[0].intent.id == old &&
              f.endings[0].intent.origin.cause == replacement.origin.cause,
          "replacement ending receives the exact replacing handler origin");
  auto cancelled = f.run("cancel");
  require(f.intents.moving.empty() && f.endings.back().intent.origin.root == cancelled.origin.root,
          "cancellation ending receives the cancelling handler origin");
  auto held = f.grasp(f.object, {{"local_pose", {{"position", {3, 0, 1}}}}}, moved.origin);
  require(held.reply.at("effect") == "applied" && f.f.objects.parent[f.object.slot()] == f.body,
          "real grasp updates native attachment and authoritative parent");
  bool found = false;
  while (auto item = step::pop_foreground(f.queues))
    if (auto *watch = std::get_if<react::WatchWork>(&item->work)) {
      require(watch->origin.root == held.origin.root && watch->origin.cause == held.origin.cause,
              "immediate attachment relation watches carry the accepted native action origin");
      found |= watch->source.subject == f.object && watch->source.term == 22;
    }
  require(found, "actual attachment relation produced its fact watch");
  auto released = f.release(f.object, {{"linear", {6, 0, 0}}}, held.origin);
  require(released.reply.at("effect") == "applied" && !f.f.objects.parent[f.object.slot()],
          "real release detaches");
  bool contact = false;
  for (int step = 0; step < 20 && !contact; ++step) {
    f.tick();
    for (const auto &row : f.last.contacts)
      if (row.started && ((row.first == f.object && row.second == f.wall) ||
                          (row.first == f.wall && row.second == f.object))) {
        contact = true;
      }
    while (auto item = step::pop_foreground(f.queues))
      if (auto *event = std::get_if<react::EventWork>(&item->work);
          event && event->receiver == f.wall && event->sender == f.object)
        require(event->origin == amber::Origin{},
                "a released body's later impact has no active navigation origin");
  }
  require(contact, "released real Jolt rigid body hits real wall");
}
void script_impulse_and_partial_lua_failure() {
  auto config = settings();
  config.gravity_m_s2 = {};
  Fixture f(config);
  const auto refused = f.begin_as(PlayerId{1}, "engine.impulse", f.object,
                                  {{"momentum", {60, 0, 0}}, {"point", {0, 0, 1}}});
  require(refused.outcome && refused.outcome->reply["error"] == "bad_input" &&
              refused.outcome->reply["problem"] ==
                  "engine.grasp, release, attach, impulse and transfer are called by a body script",
          "player impulse is refused before applying motion");
  const auto first_script =
      f.f.add("return {kind='instance',on={['novel.touch']=function(self)end}}", 1);
  const std::array installed{first_script};
  re::set_instances(f.f.installed, f.f.a, installed, f.f.scripts);
  re::project_instance(f.f.triggers, f.f.installed, f.f.scripts, f.f.terms, f.f.a);
  f.f.objects.preset[f.object.slot()] |= pe::bit(pe::Verb::Move);
  auto admitted = f.begin_as(ScriptHolder{first_script, f.f.a}, "engine.impulse", f.object,
                             {{"momentum", {60, 0, 0}}, {"point", {0, 0, 1}}});
  require(!admitted.outcome, "actual installed-script impulse is admitted");
  const auto first =
      f.finish(admitted, bi::impulse_action(f.space, identity(f.f.objects), admitted.call));
  require(first.reply.at("effect") == "applied", "script impulse applies motion");
  bool hit = false;
  for (int step = 0; step < 20 && !hit; ++step) {
    f.tick();
    while (auto item = step::pop_foreground(f.queues))
      if (auto *event = std::get_if<react::EventWork>(&item->work);
          event && event->receiver == f.wall && event->sender == f.object) {
        hit = true;
        require(event->origin == amber::Origin{}, "rigid impacts have an empty navigation origin");
      }
  }
  require(hit, "the admitted impulse makes the body hit the wall");
  Fixture partial(config, sp::Motion::Kinematic);
  const auto before = partial.f.objects.position[partial.object.slot()];
  const auto failed = partial.run("partial", {}, true);
  require(failed.changed && partial.f.objects.position[partial.object.slot()].x > before.x,
          "a later Lua error retains the same handler's accepted real shape sweep");
}
void handler_credits_include_foreign_reads() {
  auto config = settings();
  config.gravity_m_s2 = {};
  Fixture f(config, sp::Motion::Kinematic);
  f.f.objects.maker[f.f.c.slot()] = 77;
  f.f.objects.created_step[f.f.c.slot()] = 100;
  f.load(R"(return {kind='model',actions={['novel.touch']={right='use'}},on={
    ['novel.touch']=function(self,object,args)
      self:sweep(object,vector.create(.2,0,0))
      local other = self:object(args.target)
      assert(other.owner=='p:2')
    end}})",
         2);
  const auto reply = f.invoke(std::vector<EventField>{{"target", Value(f.f.c)}}, f.object);
  if (reply.outcome.status != re::RunOutcome::Status::Ok)
    throw std::runtime_error(reply.outcome.error);
  bool later_credit = false;
  for (const auto &row : f.journal.take())
    if (row["kind"] == "handler")
      later_credit |= std::ranges::find(row["authors"], "p:77") != row["authors"].end();
  require(later_credit, "the handler itself still records the actual later foreign read");
}
void incoming_rigid_character_contacts() {
  auto config = settings();
  config.gravity_m_s2 = {};
  // The inner rigid body also reports fast incoming CCD impacts during Update,
  // after the virtual controller has run. Those genuine callbacks must survive
  // duplicate reduction rather than being discarded with all inner-body events.
  Scene incoming(config);
  auto avatar = incoming.character({3, 0, 0});
  auto projectile = incoming.box({.4f, .4f, .4f}, {0, 0, .4f}, sp::Motion::Dynamic, 0, false);
  sp::impulse(incoming.space, identity(incoming.objects), projectile, {500, 0, 0}, {});
  bool impact = false;
  for (int i = 0; i < 4; ++i) {
    incoming.tick();
    for (const auto &contact : incoming.result.contacts)
      if (contact.started && contact.first == avatar && contact.second == projectile) {
        impact = true;
      }
  }
  require(impact, "incoming real rigid CCD impact reaches stationary Character inner body");
}
void character_contacts_and_fact_reduction() {
  Scene scene;
  scene.box({12, 6, .2f}, {0, 0, -.2f});
  const auto body = scene.character({0, 0, 0});
  scene.box({.4f, 3, 2}, {1, 0, 0}, sp::Motion::Static, 0, false);
  scene.wait_navigation();
  amber::step::State state;
  const amber::Origin origin{91, 90, {2}, 8.5, false};
  const Vec3 goal{3, 0, 0};
  const auto started = bi::start_move(state.intents, body, goal, origin);
  sp::MoveRequest move{body, started.id, sp::PointGoal{goal, .1f}, {}};
  record::Journal journal(3, {});
  bool contact = false;
  for (int i = 0; i < 80 && !contact; ++i) {
    scene.tick({&move, 1});
    bi::publish_physical(state, journal, {14, {10, 11, 12, 13}}, 100, scene.result,
                         {.step = scene.clock, .hz = 20});
    while (auto item = step::pop_foreground(state.queues))
      if (auto *event = std::get_if<react::EventWork>(&item->work);
          event && event->receiver == body &&
          std::abs(std::get<Vec3>(std::get<std::vector<EventField>>(event->payload)[1].value).x) >
              .5f) {
        contact = true;
        require(event->origin == origin, "controller contact carries the active move origin");
      }
  }
  require(contact, "CharacterVirtual reports the actual wall contact");
  sp::StepResult physical;
  physical.relations = {{sp::RelationKind::Contains, body, {999}, true},
                        {sp::RelationKind::Contains, body, {998}, false}};
  bi::publish_physical(state, journal, {14, {10, 11, 12, 13}}, 100, physical,
                       {.step = scene.clock, .hz = 20});
  int watches = 0;
  while (auto item = step::pop_foreground(state.queues))
    if (auto *watch = std::get_if<react::WatchWork>(&item->work);
        watch && watch->source.subject == body) {
      ++watches;
      require(watch->origin == origin, "relation change carries the moving body's origin");
    }
  require(watches == 1, "multiple relation changes coalesce into one fact notification");
  bi::finish_move(state.intents, body, bi::IntentEnd::Cancelled);
  bi::publish_physical(state, journal, {14, {10, 11, 12, 13}}, 100, physical,
                       {.step = scene.clock, .hz = 20});
  while (auto item = step::pop_foreground(state.queues))
    require(std::get<react::WatchWork>(item->work).origin == amber::Origin{},
            "idle physics has no previous navigation origin");
}
void arrival_keeps_origin_until_publication() {
  amber::step::State state;
  record::Journal journal(8, {});
  const auto body = Handle::make(1, 1), zone = Handle::make(2, 1);
  const amber::Origin origin{7, 6, {2}, 1., false};
  bi::start_move(state.intents, body, Vec3{1, 0, 0}, origin);
  sp::StepResult physical;
  physical.relations = {{sp::RelationKind::InZone, body, zone, true}};
  physical.intents = {{body, 1, sp::IntentState::Arrived, sp::PathStatus::Ready}};
  bi::publish_physical(state, journal, {14, {10, 11, 12, 13}}, 100, physical,
                       {.step = 7, .hz = 20});
  bi::finish_motion(state, physical.intents);
  require(state.intents.moving.empty() && state.intent_endings.size() == 1,
          "arrival finishes the active movement");
  while (auto item = step::pop_foreground(state.queues))
    require(std::get<react::WatchWork>(item->work).origin == origin,
            "arrival-step zone entry retains the move origin");
}
void contact_overflow_is_recorded() {
  amber::step::State step_state;
  step::WorkQueues &queues = step_state.queues;
  record::Journal journal(8, {});
  record::Event action;
  action.kind = record::Kind::Action;
  action.actor = record::Actor::player(2);
  const auto origin = journal.append(std::move(action));
  journal.take();

  sp::StepResult physical;
  const auto first = Handle::make(1, 1), second = Handle::make(2, 1);
  bi::start_move(step_state.intents, first, Vec3{1, 0, 0}, origin);
  physical.contacts.push_back({first, second, {}, {1, 0, 0}, true});
  bi::publish_physical(step_state, journal, {14, {10, 11, 12, 13}}, 1, physical,
                       amber::step::Clock{.step = 7, .hz = 20});
  const auto records = journal.take();
  require(queues.emitted_events == 1 && records.size() == 1 && records[0]["kind"] == "system" &&
              records[0]["name"] == "overflow" && records[0]["queue"] == "events" &&
              records[0]["limit"] == 1 && records[0]["dropped_term"] == 14 &&
              records[0]["root"] == origin.root && records[0]["cause"] == origin.cause,
          "a dropped physical contact must retain its protective limit and causal record");
}
} // namespace
int main() {
  try {
    final_navigation_and_immediate_relations();
    script_impulse_and_partial_lua_failure();
    handler_credits_include_foreign_reads();
    incoming_rigid_character_contacts();
    character_contacts_and_fact_reduction();
    arrival_keeps_origin_until_publication();
    contact_overflow_is_recorded();
    std::cout << "physical causality tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
