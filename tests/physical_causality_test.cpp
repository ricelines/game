#include "bindings/causality_snapshot.hpp"
#include "bindings/physical.hpp"
#include "space_fixture.hpp"

#include <set>

using namespace testspace;
namespace bi = amber::bindings;
namespace re = amber::react;
namespace pe = amber::perm;
namespace {
struct Fixture {
  Scene scene;
  bi::MechanicalCauses causes{3};
  pe::Players players;
  pe::Grants grants;
  pe::OwnerLimits limits;
  pe::Plots plots;
  Relation parts;
  step::Intents intents;
  std::vector<step::IntentEnding> endings;
  re::Vm vm{{4096, 50, 250, 64}};
  re::ScriptStates states;
  re::RandomStates random;
  record::Journal journal{3, {}};
  step::WorkQueues queues;
  step::TimerWheel timers;
  Handle body, object, wall;
  bi::NavigationBinding navigation;
  bi::AttachmentBinding attachment;
  bi::ImpulseBinding impulse;
  bi::SweepBinding sweep;
  bi::PositionBinding position;
  bi::Execution execution;
  bi::PhysicalPublication publication;
  explicit Fixture(sp::Settings config = settings(), sp::Motion mode = sp::Motion::Dynamic)
      : scene(config), body(scene.character({-3, 0, 0})),
        object(scene.box({.4f, .4f, .4f}, {0, 0, 1}, mode, 0, false)),
        wall(scene.box({.4f, 3, 3}, {3, 0, 0}, sp::Motion::Static, 0, false)),
        navigation{intents,
                   endings,
                   identity(scene.objects),
                   authority(scene.objects),
                   limits,
                   players,
                   grants,
                   plots,
                   parts},
        attachment{scene.space,
                   identity(scene.objects),
                   transforms(scene.objects),
                   parents(scene.objects),
                   space_flags(scene.objects),
                   scene.relations,
                   {10, 11, 12, 13},
                   parts,
                   authority(scene.objects),
                   limits,
                   players,
                   grants,
                   10,
                   causes},
        impulse{scene.space,
                identity(scene.objects),
                authority(scene.objects),
                limits,
                players,
                grants,
                parts,
                causes},
        sweep{scene.space,
              identity(scene.objects),
              transforms(scene.objects),
              authority(scene.objects),
              limits,
              players,
              grants,
              parts,
              causes},
        position{identity(scene.objects), transforms(scene.objects)},
        execution{vm, states, random, identity(scene.objects), parts, 91},
        publication{causes, queues, journal, {14, {10, 11, 12, 13}}, 10000, 20, {}} {
    players.rows.resize(3);
    for (auto h : {body, object, wall})
      scene.objects.owner[h.slot()] = 1;
    re::register_binding(vm, "navigate", bi::navigate_binding, &navigation);
    re::register_binding(vm, "cancel_motion", bi::cancel_motion_binding, &navigation);
    re::register_binding(vm, "grasp", bi::grasp_binding, &attachment);
    re::register_binding(vm, "release", bi::release_binding, &attachment);
    re::register_binding(vm, "impulse", bi::impulse_binding, &impulse);
    re::register_binding(vm, "sweep", bi::sweep_binding, &sweep);
    re::register_binding(vm, "position", bi::position_binding, &position);
    auto compiled = re::compile_script(R"(return {on={
      route=function(self) self:navigate(vector.create(2,0,0)) end,
      reroute=function(self) self:navigate(vector.create(1,1,0)) end,
      cancel=function(self) self:cancel_motion() end,
      hold=function(self,_,a) assert(self:grasp(a.object,{position=vector.create(3,0,1)}).effect=='done') end,
      drop=function(self,_,a) assert(self:release(a.object,vector.create(6,0,0)).effect=='done') end,
      push=function(self,_,a) assert(self:impulse(a.object,vector.create(60,0,0),self:position(a.object)).effect=='done') end,
      partial=function(self,_,a) self:impulse(a.object,vector.create(1,0,0),self:position(a.object)); error('after completed operation') end,
      read_only=function(self,_,a) local p=self:position(a.object) end,
      slide=function(self,_,a) self:sweep(a.object,vector.create(0.2,0,0)) end
    }})");
    require(compiled.errors.empty(), "causal fixture source compiles");
    re::ScriptDeclaration declaration;
    declaration.id = 1;
    declaration.author = 2;
    auto loaded = re::load_script(vm, compiled, declaration);
    require(loaded.status == re::RunOutcome::Status::Ok, loaded.error.c_str());
  }
  bi::ActionCompletion run(const char *handler, const record::Origin &parent = {},
                           bool expect_error = false) {
    bi::InvocationInput input;
    input.receiver = body;
    input.step = scene.clock;
    input.acquired = {object, wall};
    input.payload = std::vector<EventField>{{"object", Value(object)}};
    input.direct_body = re::BodyEndpoints{
        body, object, object, 1, {"navigation", "grasp", "release", "impulse", "sweep"}};
    input.origin = parent;
    auto completed = bi::execute(execution, {1, handler, 2, 1, true, false}, std::move(input));
    require(completed.outcome.status ==
                (expect_error ? re::RunOutcome::Status::Error : re::RunOutcome::Status::Ok),
            completed.outcome.error.c_str());
    record::Event action;
    action.kind = record::Kind::Action;
    action.actor = record::Actor::player(1);
    action.name = "arbitrary." + std::string(handler);
    action.step = scene.clock;
    std::vector<bi::CompletedInvocation> calls;
    calls.push_back(std::move(completed));
    return bi::publish_action(journal, queues, timers, std::move(action), std::move(calls), parent,
                              {}, 10000);
  }
  void tick(std::span<const sp::MoveRequest> requests = {}) {
    scene.tick(requests);
    bi::publish_physical(publication, scene.result, scene.clock);
    bi::collect_physical_causes(causes, scene.space);
  }
};
void final_navigation_and_immediate_relations() {
  Fixture f;
  auto moved = f.run("route");
  require(!moved.changed, "own navigation remains outside changed-world numerator");
  auto &intent = f.intents.moving.at(f.body.bits);
  require(intent.origin.cause == moved.origin.cause + 1 && intent.origin.root == moved.origin.root,
          "navigation receives final handler origin, not its empty incoming origin");
  const auto old = intent.id;
  auto replacement = f.run("reroute");
  require(f.endings.size() == 1 && f.endings[0].intent.id == old &&
              f.endings[0].intent.origin.cause == replacement.origin.cause + 1,
          "replacement ending receives the exact replacing handler origin");
  auto cancelled = f.run("cancel");
  require(f.intents.moving.empty() && f.endings.back().intent.origin.root == cancelled.origin.root,
          "cancellation ending receives the cancelling handler origin");
  auto held = f.run("hold");
  require(held.changed && f.scene.objects.parent[f.object.slot()] == f.body,
          "real grasp updates native attachment and authoritative parent");
  bool found = false;
  while (auto item = step::pop_foreground(f.queues))
    if (auto *watch = std::get_if<step::WatchWork>(&item->work)) {
      require(watch->origin.root == held.origin.root &&
                  watch->origin.cause == held.origin.cause + 1,
              "immediate attachment relation watches carry the completed handler origin");
      found |= watch->source.subject == f.object && watch->source.term == 12;
    }
  require(found, "actual attachment relation produced its fact watch");
  auto released = f.run("drop");
  require(released.changed && !f.scene.objects.parent[f.object.slot()], "real release detaches");
  bool contact = false;
  for (int step = 0; step < 20 && !contact; ++step) {
    f.tick();
    for (const auto &row : f.scene.result.contacts)
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
                    event->origin.cause == released.origin.cause + 1,
                "delayed release impact retains the exact release handler cause");
  }
  require(contact, "released real Jolt rigid body hits real wall");
}
void impulse_ambiguity_and_completed_failure() {
  auto config = settings();
  config.gravity_m_s2 = {};
  Fixture f(config);
  const auto first = f.run("push");
  const auto second = f.run("partial", {}, true);
  const auto token = sp::motion_cause(f.scene.space, identity(f.scene.objects), f.object);
  auto summary = f.causes.lookup(token);
  require(summary.ambiguous && !summary.origin.root && !summary.origin.cause &&
              summary.origin.authors == std::vector<PlayerId>({1, 2}),
          "additive impulses from distinct roots retain bounded explicit ambiguity");
  require(first.origin.root != second.origin.root, "test uses genuinely independent action roots");
  bool hit = false, anchor = false;
  for (int step = 0; step < 20 && !hit; ++step) {
    f.tick();
    for (const auto &row : f.scene.result.contacts)
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
        f.causes.begin(sp::motion_cause(f.scene.space, identity(f.scene.objects), f.object), true);
    require(
        sp::impulse(f.scene.space, identity(f.scene.objects), f.object, {.01f, 0, 0}, {}, pending)
                .status == sp::Status::Ok,
        "bounded actual impulse");
    f.causes.finalize(pending, origin);
    bi::collect_physical_causes(f.causes, f.scene.space);
  }
  summary = f.causes.lookup(sp::motion_cause(f.scene.space, identity(f.scene.objects), f.object));
  require(summary.ambiguous && summary.origin.more && summary.origin.authors.size() == 3,
          "many actual impulses keep AUTHORS_MAX and overflow marker");
}
void pose_sleep_lifecycle_and_capture() {
  auto config = settings();
  config.gravity_m_s2 = {};
  Fixture f(config, sp::Motion::Kinematic);
  f.run("read_only");
  require(!sp::motion_cause(f.scene.space, identity(f.scene.objects), f.object),
          "read consultation does not confer mechanical cause");
  f.run("slide");
  require(sp::motion_cause(f.scene.space, identity(f.scene.objects), f.object) != 0,
          "actual accepted sweep stores a step cause");
  // A pose callback captured before expiry survives through its result publication.
  f.tick();
  require(!sp::motion_cause(f.scene.space, identity(f.scene.objects), f.object),
          "stationary scripted pose cannot pollute a later unrelated walker");
  std::vector<sp::RelationChange> transitions;
  require(sp::attach(f.scene.space, identity(f.scene.objects), transforms(f.scene.objects),
                     parents(f.scene.objects), space_flags(f.scene.objects),
                     f.scene.relations.contains, f.scene.relations.supports,
                     f.scene.relations.attached_to, {f.object, f.body, {{3, 0, 1}, {}}, true},
                     transitions)
                      .status == sp::Status::Ok &&
              sp::detach(f.scene.space, identity(f.scene.objects), transforms(f.scene.objects),
                         parents(f.scene.objects), space_flags(f.scene.objects),
                         f.scene.relations.attached_to, {f.object, sp::Motion::Dynamic, {}, {}},
                         transitions)
                      .status == sp::Status::Ok,
          "actual grasp/release creates dynamic motion");
  f.run("push");
  std::vector<bi::BodyCauseRow> rows;
  bi::capture_motion_causes(f.causes, f.scene.space, identity(f.scene.objects), {}, rows);
  require(rows.empty(), "empty selected capture means empty, never whole world");
  const std::array<Handle, 1> selected{f.object};
  bi::capture_motion_causes(f.causes, f.scene.space, identity(f.scene.objects), selected, rows);
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
  bi::restore_motion_causes(fresh, f.scene.space, identity(f.scene.objects), restored);
  const auto saved =
      fresh.lookup(sp::motion_cause(f.scene.space, identity(f.scene.objects), f.object));
  require(saved.origin == rows[0].summary.origin && saved.ambiguous == rows[0].summary.ambiguous,
          "MOVE codec restores exact root, authors, oldest and overflow using fresh native token");
  Fixture restarted(config);
  std::vector<sp::MotionRow> velocities;
  sp::capture_motion(f.scene.space, identity(f.scene.objects), selected, velocities);
  sp::restore_motion(restarted.scene.space, identity(restarted.scene.objects), velocities);
  bi::restore_motion_causes(restarted.causes, restarted.scene.space,
                            identity(restarted.scene.objects), restored);
  bool resumed = false;
  for (int i = 0; i < 20 && !resumed; ++i) {
    restarted.tick();
    while (auto item = step::pop_foreground(restarted.queues))
      if (auto *event = std::get_if<step::EventWork>(&item->work);
          event && event->receiver == restarted.wall && event->sender == restarted.object) {
        resumed = true;
        require(event->origin == rows[0].summary.origin,
                "fresh Jolt world continues in-flight collision cause after MOVE restore");
      }
  }
  require(resumed, "restored moving body produces actual future impact");
  // Restore back into the active fixture's table before its next publication/collection.
  bi::restore_motion_causes(f.causes, f.scene.space, identity(f.scene.objects), restored);
  std::vector<sp::RelationChange> changes;
  require(sp::custody(f.scene.space, identity(f.scene.objects), f.object, f.body,
                      parents(f.scene.objects), space_flags(f.scene.objects), f.scene.relations,
                      changes)
                  .status == sp::Status::Ok,
          "uninstrumented real custody extracts the body");
  require(!sp::motion_cause(f.scene.space, identity(f.scene.objects), f.object),
          "custody cannot borrow earlier impulse cause");
  sp::remove(f.scene.space, identity(f.scene.objects), f.object, parents(f.scene.objects),
             space_flags(f.scene.objects), f.scene.relations, changes);
  destroy(f.scene.objects, f.object);
  auto replacement = f.scene.box({.4f, .4f, .4f}, {0, 0, 1}, sp::Motion::Dynamic, 0, false);
  require(replacement.slot() == f.object.slot() && replacement != f.object &&
              !sp::motion_cause(f.scene.space, identity(f.scene.objects), replacement),
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
  bi::MechanicalCauses causes(3);
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
  step::WorkQueues queues;
  record::Journal journal(3, {});
  bi::PhysicalPublication publication{causes, queues, journal, {14, {10, 11, 12, 13}}, 100, 20, {}};
  bi::publish_physical(publication, scene.result, scene.clock);
  bool captured = false;
  while (auto item = step::pop_foreground(queues))
    if (auto *event = std::get_if<step::EventWork>(&item->work);
        event && event->receiver == cargo && event->sender == pusher) {
      captured = true;
      require(event->origin == old, "callback token keeps exact pre-expiry, pre-replacement cause");
    }
  require(captured, "actual pose collision emitted real event work");
  bi::collect_physical_causes(causes, scene.space);
  // A compound's query-only child follow must not downgrade the root motion cause
  // to a one-step pose cause while the physical aggregate is still in flight.
  auto part = scene.box({.2f, .2f, .2f}, {2, 0, 2}, sp::Motion::Kinematic, 0, false);
  std::vector<sp::RelationChange> changes;
  require(sp::attach(scene.space, identity(scene.objects), transforms(scene.objects),
                     parents(scene.objects), space_flags(scene.objects), scene.relations.contains,
                     scene.relations.supports, scene.relations.attached_to,
                     {part, cargo, {{.5f, 0, 0}, {}}, false}, changes)
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
  bi::MechanicalCauses causes(3);
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
  step::WorkQueues queues;
  record::Journal journal(3, {});
  bi::PhysicalPublication publication{causes, queues, journal, {14, {10, 11, 12, 13}}, 100, 20, {}};
  sp::StepResult physical;
  auto other = causes.intern({93, 92, {3}, 5., false});
  physical.relations = {{sp::RelationKind::Contains, body, {999}, true, {token, 0}},
                        {sp::RelationKind::Contains, body, {998}, false, {other, 0}}};
  bi::publish_physical(publication, physical, scene.clock);
  bool reduced = false;
  while (auto item = step::pop_foreground(queues))
    if (auto *watch = std::get_if<step::WatchWork>(&item->work);
        watch && watch->source.subject == body) {
      reduced = true;
      require(watch->origin.root != origin.root && watch->origin.root != 92 &&
                  watch->origin.authors == std::vector<PlayerId>({2, 3}) &&
                  watch->origin.oldest == 5.,
              "same-publication fact coalescence explicitly merges distinct causes");
    }
  require(reduced, "physics fact reduction actually queues watch");
  step::enqueue_watch(queues, {{body, 10}, origin});
  bi::publish_physical(publication, physical, scene.clock);
  auto first = step::pop_foreground(queues);
  require(std::get<step::WatchWork>(first->work).origin == origin,
          "already queued watch retains its first scheduling cause by documented convention");
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
    impulse_ambiguity_and_completed_failure();
    pose_sleep_lifecycle_and_capture();
    copied_pose_callbacks_and_compound_scope();
    character_contacts_and_fact_reduction();
    std::cout << "physical causality tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
