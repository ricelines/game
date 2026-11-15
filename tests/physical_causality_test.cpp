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
  HandlerResult run(const char *name, const record::Origin &parent = {},
                    bool expect_error = false) {
    auto result = invoke(std::vector<EventField>{{"mode", std::string(name)}}, object, parent);
    require((result.outcome.status == re::RunOutcome::Status::Error) == expect_error,
            "actual Lua primitive handler status");
    return result;
  }
  bi::ActionOutcome push(const record::Origin &parent = {}, double momentum = 60) {
    const auto p = f.objects.position[object.slot()];
    return impulse(object, {{"momentum", {momentum, 0, 0}}, {"point", {p.x, p.y, p.z}}}, parent);
  }
  void tick(std::span<const sp::MoveRequest> requests = {}) {
    sp::step(space, f.objects_state, physical, ++clock.step, .05f, requests, last);
    bi::publish_physical(step_state, journal, {14, {20, 21, 22, 23}}, 10000, 20, f.objects_state,
                         f.catalog_state, last, clock.step);
    bi::collect_physical_causes(causes, space);
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
    if (auto *watch = std::get_if<step::WatchWork>(&item->work)) {
      require(watch->origin.root == held.origin.root && watch->origin.cause == held.origin.cause,
              "immediate attachment relation watches carry the accepted native action origin");
      found |= watch->source.subject == f.object && watch->source.term == 22;
    }
  require(found, "actual attachment relation produced its fact watch");
  f.f.objects.maker[f.wall.slot()] = 77;
  f.f.objects.created_step[f.wall.slot()] = 40;
  auto released = f.release(f.object, {{"linear", {6, 0, 0}}}, held.origin);
  require(released.reply.at("effect") == "applied" && !f.f.objects.parent[f.object.slot()],
          "real release detaches");
  bool contact = false;
  for (int step = 0; step < 20 && !contact; ++step) {
    f.tick();
    for (const auto &row : f.last.contacts)
      if (row.started && ((row.first == f.object && row.second == f.wall) ||
                          (row.first == f.wall && row.second == f.object))) {
        // A callback owns its token until publication; expiry may already have removed the live
        // tag.
        contact = true;
      }
    while (auto item = step::pop_foreground(f.queues))
      if (auto *event = std::get_if<step::EventWork>(&item->work);
          event && event->receiver == f.wall && event->sender == f.object)
        require(event->origin.root == released.origin.root &&
                    event->origin.cause == released.origin.cause &&
                    std::ranges::find(event->origin.authors, PlayerId{77}) !=
                        event->origin.authors.end(),
                "delayed release impact retains the exact accepted release cause");
  }
  require(contact, "released real Jolt rigid body hits real wall");
}
void impulse_ambiguity_and_partial_lua_failure() {
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
  const auto first = f.finish(
      admitted, bi::impulse_action(f.space, identity(f.f.objects), f.causes, admitted.call));
  const auto second = f.push({}, 1);
  const auto token = sp::motion_cause(f.space, identity(f.f.objects), f.object);
  auto summary = f.causes.lookup(token);
  require(summary.ambiguous && !summary.origin.root && !summary.origin.cause &&
              summary.origin.authors == std::vector<PlayerId>({1, 2}),
          "additive impulses from distinct roots retain bounded explicit ambiguity");
  require(first.origin.root != second.origin.root, "test uses genuinely independent action roots");
  bool hit = false, anchor = false;
  for (int step = 0; step < 20 && !hit; ++step) {
    f.tick();
    for (const auto &row : f.last.contacts)
      hit |= row.started && (row.first == f.object || row.second == f.object) &&
             (row.first == f.wall || row.second == f.wall);
  }
  for (const auto &row : f.journal.take())
    if (row["name"] == "physical_interaction") {
      anchor = true;
      require(row["causal_ambiguity"] == true && row["cause"].is_null() &&
                  row["root"] == row["id"] && row["result"] == "no_effect" &&
                  row["changed"] == false && row["authors"].size() == 2,
              "ambiguous impact makes a new accurate System provenance anchor");
    }
  require(hit && anchor, "real delayed collision publishes ambiguous additive impulse lineage");
  // Long impulse sequences stay flat/bounded, with explicit overflow rather than a history graph.
  for (PlayerId author = 3; author < 100; ++author) {
    record::Event event;
    event.kind = record::Kind::Action;
    event.actor = record::Actor::player(author);
    const auto origin = f.journal.append(std::move(event));
    const auto pending =
        f.causes.intern(origin, sp::motion_cause(f.space, identity(f.f.objects), f.object));
    require(
        sp::impulse(f.space, identity(f.f.objects), f.object, {.01f, 0, 0}, {}, pending).status ==
            sp::Status::Ok,
        "bounded actual impulse");
    bi::collect_physical_causes(f.causes, f.space);
  }
  summary = f.causes.lookup(sp::motion_cause(f.space, identity(f.f.objects), f.object));
  require(summary.ambiguous && summary.origin.more && summary.origin.authors.size() == 3,
          "many actual impulses keep AUTHORS_MAX and overflow marker");
  Fixture partial(config, sp::Motion::Kinematic);
  const auto before = partial.f.objects.position[partial.object.slot()];
  const auto failed = partial.run("partial", {}, true);
  require(failed.changed && partial.f.objects.position[partial.object.slot()].x > before.x,
          "a later Lua error retains the same handler's accepted real shape sweep");
  require(
      partial.causes
              .lookup(sp::motion_cause(partial.space, identity(partial.f.objects), partial.object))
              .origin.cause == failed.origin.cause,
      "partial failure preserves the operation-time handler cause");
}
void operation_time_credits_do_not_look_ahead() {
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
  const auto summary = f.causes.lookup(sp::motion_cause(f.space, identity(f.f.objects), f.object));
  require(summary.origin.cause == reply.origin.cause &&
              std::ranges::find(summary.origin.authors, PlayerId{77}) ==
                  summary.origin.authors.end(),
          "a later read cannot retroactively credit an earlier accepted own-part sweep");
  bool later_credit = false;
  for (const auto &row : f.journal.take())
    if (row["kind"] == "handler")
      later_credit |= std::ranges::find(row["authors"], "p:77") != row["authors"].end();
  require(later_credit, "the handler itself still records the actual later foreign read");
}
void pose_sleep_lifecycle_and_capture() {
  auto config = settings();
  config.gravity_m_s2 = {};
  Fixture f(config, sp::Motion::Kinematic);
  f.run("read_only");
  require(!sp::motion_cause(f.space, identity(f.f.objects), f.object),
          "read consultation does not confer mechanical cause");
  f.run("slide");
  require(sp::motion_cause(f.space, identity(f.f.objects), f.object) != 0,
          "actual accepted sweep stores a step cause");
  // A pose callback captured before expiry survives through its result publication.
  f.tick();
  require(!sp::motion_cause(f.space, identity(f.f.objects), f.object),
          "stationary scripted pose cannot pollute a later unrelated walker");

  require(sp::attach(f.space, f.f.objects_state, f.space_state,
                     {f.object, f.body, {{3, 0, 1}, {}}, true})
                      .status == sp::Status::Ok &&
              sp::detach(f.space, f.f.objects_state, f.space_state,
                         {f.object, sp::Motion::Dynamic, {}, {}})
                      .status == sp::Status::Ok,
          "actual grasp/release creates dynamic motion");
  f.push();
  std::vector<bi::BodyCauseRow> rows;
  bi::capture_motion_causes(f.causes, f.space, identity(f.f.objects), {}, rows);
  require(rows.empty(), "empty selected capture means empty, never whole world");
  const std::array<Handle, 1> selected{f.object};
  bi::capture_motion_causes(f.causes, f.space, identity(f.f.objects), selected, rows);
  require(rows.size() == 1 && rows[0].object == f.object, "selected in-flight cause captured");
  rows[0].summary.origin.oldest = 12.5;
  rows[0].summary.origin.more = true;
  snap::Buffer bytes;
  snap::Output out{bytes};
  bi::write_motion_causes(out, rows);
  out.adapter().flush();
  bytes.resize(out.adapter().writtenBytesCount());
  std::vector<bi::BodyCauseRow> restored;
  snap::Input in{bytes.data(), bytes.size()};
  bi::read_motion_causes(in, restored);
  snap::check_complete(in);
  bi::MechanicalCauses fresh(3);
  bi::restore_motion_causes(fresh, f.space, identity(f.f.objects), restored);
  const auto saved = fresh.lookup(sp::motion_cause(f.space, identity(f.f.objects), f.object));
  require(saved.origin == rows[0].summary.origin && saved.ambiguous == rows[0].summary.ambiguous,
          "MOVE codec restores exact root, authors, oldest and overflow using fresh native token");
  Fixture restarted(config);
  restarted.f.objects.created_step[restarted.object.slot()] = 400;
  restarted.f.objects.maker[restarted.wall.slot()] = 77;
  restarted.f.objects.created_step[restarted.wall.slot()] = 40;
  restarted.f.models.rows[1].created_s = 20;
  auto impact_origin = rows[0].summary.origin;
  impact_origin.authors.push_back(77);
  impact_origin.oldest = 2;
  std::vector<sp::MotionRow> velocities;
  sp::capture_motion(f.space, identity(f.f.objects), selected, velocities);
  sp::restore_motion(restarted.space, identity(restarted.f.objects), velocities);
  bi::restore_motion_causes(restarted.causes, restarted.space, identity(restarted.f.objects),
                            restored);
  bool resumed = false;
  for (int i = 0; i < 20 && !resumed; ++i) {
    restarted.tick();
    while (auto item = step::pop_foreground(restarted.queues))
      if (auto *event = std::get_if<step::EventWork>(&item->work);
          event && event->receiver == restarted.wall && event->sender == restarted.object) {
        resumed = true;
        require(event->origin == impact_origin,
                "restored in-flight collision retains its cause and credits the actual wall");
      }
  }
  require(resumed, "restored moving body produces actual future impact");
  // Restore back into the active fixture's table before its next publication/collection.
  bi::restore_motion_causes(f.causes, f.space, identity(f.f.objects), restored);
  std::vector<sp::RelationChange> changes;
  require(sp::custody(f.space, f.f.objects_state, f.object, f.body, f.space_state).status ==
              sp::Status::Ok,
          "uninstrumented real custody extracts the body");
  require(!sp::motion_cause(f.space, identity(f.f.objects), f.object),
          "custody cannot borrow earlier impulse cause");
  sp::remove(f.space, identity(f.f.objects), f.object, parents(f.f.objects),
             space_flags(f.f.objects), f.physical, changes);
  destroy(f.f.objects, f.object);
  auto replacement = f.box({.4f, .4f, .4f}, {0, 0, 1}, sp::Motion::Dynamic, false);
  require(replacement.slot() == f.object.slot() && replacement != f.object &&
              !sp::motion_cause(f.space, identity(f.f.objects), replacement),
          "generation reuse has no previous body cause");
  // A real resting dynamic body loses command provenance on native sleep.
  Scene resting;
  resting.box({5, 5, .2f}, {0, 0, -.2f});
  auto ball = resting.box({.4f, .4f, .4f}, {0, 0, .01f}, sp::Motion::Dynamic, 0, false);
  require(sp::impulse(resting.space, identity(resting.objects), ball, {0, 0, .01f}, {}, 9).status ==
              sp::Status::Ok,
          "sleeping real impulse");
  for (int i = 0; i < 200 && sp::motion_cause(resting.space, identity(resting.objects), ball); ++i)
    resting.tick();
  require(!sp::motion_cause(resting.space, identity(resting.objects), ball) &&
              sp::motion(resting.space, identity(resting.objects), ball)->asleep,
          "native sleep expires dynamic command cause");
}
void copied_pose_callbacks_and_compound_scope() {
  auto config = settings();
  config.gravity_m_s2 = {};
  Scene scene(config);
  auto pusher = scene.box({1, 1, 1}, {0, 0, 2}, sp::Motion::Kinematic, 0, false);
  auto cargo = scene.box({1, 1, 1}, {1.5f, 0, 2}, sp::Motion::Dynamic, 0, false);
  amber::step::State step_state{3};
  bi::MechanicalCauses &causes = step_state.mechanical_causes;
  const record::Origin old{81, 80, {2}, 1., false}, newer{83, 82, {3}, 2., false};
  const auto previous = causes.intern(old);
  require(sp::set_pose(scene.space, identity(scene.objects), transforms(scene.objects), pusher,
                       {{.8f, 0, 2}, {}}, previous)
                  .status == sp::Status::Ok,
          "actual kinematic pose overlaps real dynamic body");
  std::vector<bi::BodyCauseRow> transient;
  bi::capture_motion_causes(causes, scene.space, identity(scene.objects), std::span(&pusher, 1),
                            transient);
  require(transient.empty(), "local selection excludes transient kinematic pose causes from MOVE");
  scene.tick();
  require(!scene.result.contacts.empty() &&
              !sp::motion_cause(scene.space, identity(scene.objects), pusher),
          "contact callback is captured even though stationary pose tag expires");
  // A later command must not relabel an earlier already-copied callback.
  sp::set_pose(scene.space, identity(scene.objects), transforms(scene.objects), pusher,
               {{-2, 0, 2}, {}}, causes.intern(newer));
  step::WorkQueues &queues = step_state.queues;
  record::Journal journal(3, {});
  amber::catalog::State catalog_state;
  catalog::Models &models = catalog_state.models;
  models.rows.resize(2);

  bi::publish_physical(step_state, journal, {14, {10, 11, 12, 13}}, 100, 20, scene.objects_state,
                       catalog_state, scene.result, scene.clock);
  bool captured = false;
  while (auto item = step::pop_foreground(queues))
    if (auto *event = std::get_if<step::EventWork>(&item->work);
        event && event->receiver == cargo && event->sender == pusher) {
      captured = true;
      require(event->origin.cause == old.cause && event->origin.root == old.root &&
                  std::ranges::find(event->origin.authors, PlayerId{2}) !=
                      event->origin.authors.end() &&
                  std::ranges::find(event->origin.authors, PlayerId{3}) ==
                      event->origin.authors.end(),
              "captured contact keeps its accepted cause rather than the later pose command");
      require(event->origin.oldest && *event->origin.oldest <= *old.oldest,
              "contact endpoint credits retain or extend the causal age");
    }
  require(captured, "actual pose collision emitted real event work");
  bi::collect_physical_causes(causes, scene.space);
  // A compound's query-only child follow must not downgrade the root motion cause
  // to a one-step pose cause while the physical aggregate is still in flight.
  auto part = scene.box({.2f, .2f, .2f}, {2, 0, 2}, sp::Motion::Kinematic, 0, false);

  require(sp::attach(scene.space, scene.objects_state, scene.space_state,
                     {part, cargo, {{.5f, 0, 0}, {}}, false})
                      .status == sp::Status::Ok &&
              sp::assemble(scene.space, identity(scene.objects), transforms(scene.objects), cargo,
                           std::span(&part, 1), sp::Motion::Dynamic)
                      .status == sp::Status::Ok,
          "actual physical-root compound created");
  auto impulse = causes.intern(old);
  sp::impulse(scene.space, identity(scene.objects), cargo, {10, 0, 0}, {}, impulse);
  for (int i = 0; i < 4; ++i)
    scene.tick();
  require(sp::motion_cause(scene.space, identity(scene.objects), cargo) == impulse &&
              sp::motion_cause(scene.space, identity(scene.objects), part) == impulse,
          "moving compound and its optical child retain one ongoing motion cause");
  // The inner rigid body also reports fast incoming CCD impacts during Update,
  // after the virtual controller has run. Those genuine callbacks must survive
  // duplicate reduction rather than being discarded with all inner-body events.
  Scene incoming(config);
  auto avatar = incoming.character({3, 0, 0});
  auto projectile = incoming.box({.4f, .4f, .4f}, {0, 0, .4f}, sp::Motion::Dynamic, 0, false);
  sp::impulse(incoming.space, identity(incoming.objects), projectile, {500, 0, 0}, {}, 23);
  bool impact = false;
  for (int i = 0; i < 4; ++i) {
    incoming.tick();
    for (const auto &contact : incoming.result.contacts)
      if (contact.started && contact.first == avatar && contact.second == projectile) {
        impact = true;
        require(contact.causes.second == 23,
                "incoming CCD contact preserves actual projectile cause");
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
  amber::step::State step_state{3};
  bi::MechanicalCauses &causes = step_state.mechanical_causes;
  record::Origin origin{91, 90, {2}, 8.5, false};
  auto token = causes.intern(origin);
  sp::MoveRequest move{body, 1, sp::PointGoal{{3, 0, 0}, .1f}, {}, {}, token};
  bool contact = false;
  for (int i = 0; i < 80 && !contact; ++i) {
    scene.tick({&move, 1});
    for (const auto &row : scene.result.contacts)
      if (row.started && (row.first == body || row.second == body) &&
          std::abs(row.normal.x) > .5f) {
        contact = true;
        const auto captured = row.first == body ? row.causes.first : row.causes.second;
        require(causes.lookup(captured).origin == origin,
                "real CharacterVirtual wall callback captures controller cause");
      }
  }
  require(contact, "real CharacterContactListener emits controller/wall contacts");
  scene.tick();
  require(!sp::motion_cause(scene.space, identity(scene.objects), body),
          "grounded idle character loses old navigation cause before unrelated contacts");
  step::WorkQueues &queues = step_state.queues;
  record::Journal journal(3, {});
  amber::catalog::State catalog_state;
  catalog::Models &models = catalog_state.models;
  models.rows.resize(3);
  models.rows[1].id = 1;
  models.rows[2].id = 2;

  sp::StepResult physical;
  auto other = causes.intern({93, 92, {3}, 5., false});
  physical.relations = {{sp::RelationKind::Contains, body, {999}, true, {token, 0}},
                        {sp::RelationKind::Contains, body, {998}, false, {other, 0}}};
  bi::publish_physical(step_state, journal, {14, {10, 11, 12, 13}}, 100, 20, scene.objects_state,
                       catalog_state, physical, scene.clock);
  bool reduced = false;
  while (auto item = step::pop_foreground(queues))
    if (auto *watch = std::get_if<step::WatchWork>(&item->work);
        watch && watch->source.subject == body) {
      reduced = true;
      require(watch->origin.root != origin.root && watch->origin.root != 92 &&
                  watch->origin.authors == std::vector<PlayerId>({2, 3}) &&
                  watch->origin.oldest == 0.,
              "fact coalescence merges distinct causes and the actual receiver's birth age");
    }
  require(reduced, "physics fact reduction actually queues watch");
  step::enqueue_watch(queues, {{body, 10}, origin});
  bi::publish_physical(step_state, journal, {14, {10, 11, 12, 13}}, 100, 20, scene.objects_state,
                       catalog_state, physical, scene.clock);
  auto first = step::pop_foreground(queues);
  require(std::get<step::WatchWork>(first->work).origin == origin,
          "already queued watch retains its first scheduling cause by documented convention");
}
void contact_overflow_is_recorded() {
  amber::step::State step_state{8};
  bi::MechanicalCauses &causes = step_state.mechanical_causes;
  step::WorkQueues &queues = step_state.queues;
  record::Journal journal(8, {});
  record::Event action;
  action.kind = record::Kind::Action;
  action.actor = record::Actor::player(2);
  const auto origin = journal.append(std::move(action));
  journal.take();
  const auto token = causes.intern(origin);
  amber::ObjectState objects_state;

  amber::catalog::State catalog_state;

  sp::StepResult physical;
  const auto first = Handle::make(1, 1), second = Handle::make(2, 1);
  physical.contacts.push_back({first, second, {}, {1, 0, 0}, true, {token, 0}});
  bi::publish_physical(step_state, journal, {14, {10, 11, 12, 13}}, 1, 20, objects_state,
                       catalog_state, physical, 7);
  const auto records = journal.take();
  require(queues.emitted_events == 1 && records.size() == 1 && records[0]["kind"] == "system" &&
              records[0]["name"] == "overflow" && records[0]["queue"] == "events" &&
              records[0]["limit"] == 1 && records[0]["dropped_term"] == 14 &&
              records[0]["root"] == origin.root && records[0]["cause"] == origin.cause,
          "a dropped physical contact must retain its protective limit and causal record");
}
} // namespace
void density_benchmark() {
  for (int count : {10000, 100000}) {
    auto config = settings();
    config.gravity_m_s2 = {};
    config.max_bodies = count + 64;
    Scene scene(config);
    Box shape({.3f, .3f, .3f});
    const auto geometry = sp::import_geometry(scene.space, shape.source()).geometry;
    bi::MechanicalCauses causes(3);
    const auto measure = [&](std::string_view suffix) {
      scene.tick();
      std::vector<double> times;
      for (int i = 0; i < 200; ++i) {
        const auto start = std::chrono::steady_clock::now();
        scene.tick();
        bi::collect_physical_causes(causes, scene.space);
        times.push_back(
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                .count());
      }
      std::sort(times.begin(), times.end());
      std::cout << "causal_inert_" << count << suffix << " us p50=" << times[100]
                << " p95=" << times[190] << " max=" << times.back() << '\n';
    };
    for (int i = 0; i < count + 32; ++i) {
      if (i == count)
        measure("_idle");
      const bool dynamic = i >= count;
      ObjectInit init;
      init.model = 1;
      init.material = 1;
      init.position = {float(i % 1000) * 2, float(i / 1000) * 2, dynamic ? 10.f : 0.f};
      init.flags = dynamic ? Dynamic : Static;
      const auto object = create(scene.objects, init);
      require(sp::add(scene.space, identity(scene.objects), transforms(scene.objects),
                      {object,
                       geometry,
                       dynamic ? sp::Motion::Dynamic : sp::Motion::Static,
                       {.5f, 0, 0},
                       10,
                       false,
                       false,
                       {}})
                      .status == sp::Status::Ok,
              "density native body");
      if (dynamic) {
        const auto token =
            causes.intern({std::uint64_t(i + 1), std::uint64_t(i + 1), {2}, 1., false});
        sp::impulse(scene.space, identity(scene.objects), object, {1, 0, 0}, init.position, token);
      }
    }
    measure("_dynamic32");
  }
}
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--density") {
      density_benchmark();
      return 0;
    }
    final_navigation_and_immediate_relations();
    impulse_ambiguity_and_partial_lua_failure();
    operation_time_credits_do_not_look_ahead();
    pose_sleep_lifecycle_and_capture();
    copied_pose_callbacks_and_compound_scope();
    character_contacts_and_fact_reduction();
    contact_overflow_is_recorded();
    std::cout << "physical causality tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
