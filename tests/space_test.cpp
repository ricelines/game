#include "perm/perm.hpp"
#include "space/private.hpp"
#include "space_fixture.hpp"
namespace {
using namespace testspace;
bool edge(const sp::Edges &edges, Handle from, Handle to) {
  auto *list = edges.out.get(from.slot());
  return list && std::find(list->begin(), list->end(), to) != list->end();
}
void returns_release_seated_standing_and_static_occupants() {
  Scene scene;
  const auto seat = scene.box({4, 4, 1}, {0, 0, 1});
  const auto seated = scene.character({0, 0, 2});
  const auto standing = scene.character({1, 0, 2});
  require(sp::attach(scene.objects_state, scene.space_state, {seated, seat, {{0, 0, 1}, {}}, false})
                  .status == sp::Status::Ok,
          "seat return fixture");
  for (int tick = 0; tick < 4; ++tick)
    scene.tick();
  const auto seated_z = scene.objects.position[seated.slot()].z;
  const auto standing_z = scene.objects.position[standing.slot()].z;
  require(sp::custody(scene.objects_state, seat, {}, scene.space_state, {}, true).status ==
                  sp::Status::Ok &&
              !scene.objects.parent[seated.slot()] &&
              sp::body_motion(scene.space, identity(scene.objects), seated)->motion ==
                  sp::Motion::Character,
          "return did not release seated body");
  for (int tick = 0; tick < 12; ++tick)
    scene.tick();
  require(scene.objects.position[seated.slot()].z < seated_z - .1f &&
              scene.objects.position[standing.slot()].z < standing_z - .1f,
          "returned support did not let bodies fall");
  const auto root = scene.box({1, 1, 1}, {10, 0, 0});
  Box mesh({1, 1, 1});
  const auto geometry =
      sp::import_geometry(scene.space, {mesh.mesh, {}, mesh.triangles, mesh.bounds});
  ObjectInit init;
  init.model = 1;
  init.position = {12, 0, 0};
  init.flags = Static;
  const auto ornament = create(scene.objects, init);
  require(
      sp::add(scene.space, identity(scene.objects), transforms(scene.objects),
              {ornament, geometry.geometry, sp::Motion::Static, {.6f, 0, 0}, 1, false, false, {}})
              .status == sp::Status::Ok,
      "triangle-only occupant fixture");
  // Exercise the specified static-attachment fallback directly at the Space boundary.
  scene.space.native->attachments.emplace(ornament.bits,
                                          sp::Attachment{ornament, root, {{2, 0, 0}, {}}, false});
  scene.objects.parent[ornament.slot()] = root;
  const auto pose = scene.objects.position[ornament.slot()];
  require(sp::can_custody(scene.space, identity(scene.objects), root, {}, parents(scene.objects))
                      .status == sp::Status::UnsupportedMotion &&
              sp::custody(scene.objects_state, root, {}, scene.space_state, {}, true).status ==
                  sp::Status::Ok &&
              !scene.objects.parent[ornament.slot()] &&
              scene.objects.position[ornament.slot()] == pose &&
              sp::body_motion(scene.space, identity(scene.objects), ornament)->motion ==
                  sp::Motion::Static,
          "return refused a static occupant without a hull or changed its pose");
  auto limited_settings = settings();
  limited_settings.max_bodies = 2;
  Scene limited(limited_settings);
  ObjectInit logical;
  logical.model = 1;
  const auto shelf = create(limited.objects, logical);
  require(sp::register_logical_root(limited.space, identity(limited.objects), shelf).status ==
              sp::Status::Ok,
          "capacity return logical root");
  const auto attached = limited.box({1, 1, 1}, {2, 0, 0}, sp::Motion::Kinematic);
  limited.box({1, 1, 1}, {4, 0, 0});
  require(sp::attach(limited.objects_state, limited.space_state,
                     {attached, shelf, {{2, 0, 0}, {}}, false})
                  .status == sp::Status::Ok,
          "capacity return attachment");
  require(
      sp::can_custody(limited.space, identity(limited.objects), shelf, {}, parents(limited.objects))
                  .status == sp::Status::Capacity &&
          sp::custody(limited.objects_state, shelf, {}, limited.space_state, {}, true).status ==
              sp::Status::Ok &&
          sp::body_motion(limited.space, identity(limited.objects), attached)->motion ==
              sp::Motion::Static &&
          limited.objects.position[attached.slot()] == Vec3{2, 0, 0},
      "full native capacity blocked a return or moved its occupant");
}

void expired_native_queries_stop() {
  Scene scene;
  const auto object = scene.box({1, 1, 1}, {0, 0, 0});
  sp::QueryFilter filter;
  filter.deadline = {std::chrono::steady_clock::now(), {}};
  std::vector<sp::Nearby> found;
  sp::near(scene.space, identity(scene.objects), {0, 0, 0}, 5, filter, found);
  require(found.empty(), "near stops on an exhausted CPU deadline");
  require(!sp::raycast(scene.space, identity(scene.objects), {-3, 0, .5f}, {3, 0, .5f}, filter),
          "raycast stops on an exhausted CPU deadline");
  require(
      sp::raycast(scene.space, identity(scene.objects), {-3, 0, .5f}, {3, 0, .5f}, {})->object ==
          object,
      "an unlimited query sees the same geometry");
}
void sensor_contacts_use_body_overlap() {
  Scene scene;
  const auto moving = scene.box({.6f, .6f, .6f}, {2, 0, 0}, sp::Motion::Kinematic, 0, false, false);
  ObjectInit init;
  init.model = 1;
  const auto zone = create(scene.objects, init);
  const auto geometry = sp::sensor_box_geometry(scene.space, {2, 4, 4});
  require(sp::add_volume(scene.space, identity(scene.objects), transforms(scene.objects),
                         {zone, geometry, {{0, 0, 1}, {}}, sp::VolumeKind::Zone})
                  .status == sp::Status::Ok,
          "solid zone sensor add");
  const auto container = create(scene.objects, init);
  require(sp::add_volume(scene.space, identity(scene.objects), transforms(scene.objects),
                         {container, geometry, {{0, 0, 1}, {}}, sp::VolumeKind::Container})
                  .status == sp::Status::Ok,
          "container sensor add");
  scene.tick();
  require(!edge(scene.relations.in_zone, moving, zone), "outside body starts in zone");
  auto relocate = [&](float x) {
    require(sp::set_pose(scene.space, identity(scene.objects), transforms(scene.objects), moving,
                         {{x, 0, 0}, {}})
                    .status == sp::Status::Ok,
            "move nonsolid kinematic sensor subject");
    scene.tick();
  };
  // The centre is outside. This must come from shape overlap, not the former
  // centre-in-volume polling path. Nonsolid motion does not wake neighbours.
  relocate(1.2f);
  require(edge(scene.relations.in_zone, moving, zone),
          "rigid sensor ignored body overlap outside its centre");
  require(!edge(scene.relations.contains, container, moving),
          "container adopted zone overlap instead of its separate centre rule");
  require(scene.result.contacts.empty() && !edge(scene.relations.supports, zone, moving),
          "zone sensor leaked a physical collision/support event");
  scene.tick();
  require(scene.result.relations.empty() && edge(scene.relations.in_zone, moving, zone),
          "persisted sensor contact repeated or lost membership");
  relocate(2);
  require(!edge(scene.relations.in_zone, moving, zone), "rigid sensor exit was missed");
  relocate(0);
  require(edge(scene.relations.contains, container, moving), "inside centre lacks containment");
  scene.tick();
  require(scene.result.moved.empty() && scene.result.relations.empty(),
          "stationary container contact produced spurious movement or relation changes");
  std::vector<sp::RelationChange> changes;
  require(sp::custody(scene.objects_state, moving, {}, scene.space_state).status == sp::Status::Ok,
          "collect zone member into hidden custody");
  scene.tick();
  require(!edge(scene.relations.in_zone, moving, zone),
          "hidden inventory retained or recreated sensor membership");

  // A static shape has no ordinary active-body contact update, so an explicit
  // relocation uses the one-shot reconciliation path instead.
  const auto fixed = scene.box({.6f, .6f, .6f}, {2, 0, 0});
  scene.tick();
  require(sp::set_pose(scene.space, identity(scene.objects), transforms(scene.objects), fixed,
                       {{1.2f, 0, 0}, {}})
                  .status == sp::Status::Ok,
          "relocate static sensor subject");
  scene.tick();
  require(edge(scene.relations.in_zone, fixed, zone), "static relocation lost zone overlap");
  scene.objects.position[zone.slot()] = {10, 0, 0};
  scene.tick();
  require(!edge(scene.relations.in_zone, fixed, zone), "moved sensor retained static member");
  scene.objects.position[zone.slot()] = {};
  scene.tick();
  require(edge(scene.relations.in_zone, fixed, zone), "moved sensor failed to regain member");
  sp::remove(scene.space, identity(scene.objects), zone, parents(scene.objects),
             space_flags(scene.objects), scene.relations, changes);
  destroy(scene.objects, zone);
  init.position = {10, 0, 0};
  const auto replacement = create(scene.objects, init);
  require(replacement.slot() == zone.slot() && replacement != zone,
          "sensor generation fixture did not reuse its slot");
  require(sp::add_volume(scene.space, identity(scene.objects), transforms(scene.objects),
                         {replacement, geometry, {{0, 0, 1}, {}}, sp::VolumeKind::Zone})
                  .status == sp::Status::Ok,
          "replacement sensor add");
  scene.tick();
  require(!edge(scene.relations.in_zone, fixed, zone) &&
              !edge(scene.relations.in_zone, fixed, replacement),
          "retired sensor contact leaked to a reused owner slot");
}
void plot_membership_follows_real_motion_and_hidden_custody() {
  Scene scene;
  const auto holder = scene.character({-1, 0, 0});
  const auto item = scene.box({.2f, .2f, .2f}, {-1, 0, 1}, sp::Motion::Kinematic, 0, false, false);
  perm::Plots plots;
  plots.rows.push_back({1, 1, 0, 0, {-5, -5, 0, 5}, {}});
  plots.rows.push_back({2, 2, 0, 0, {0, -5, 5, 5}, {}});
  const std::array selected{item};
  auto refresh = [&](bool visible) {
    return perm::update_plots(scene.objects, plots, selected, visible);
  };
  require(refresh(true) == std::vector<Handle>{item} && scene.objects.plot[item.slot()] == 1,
          "placed goods plot not initialized");

  require(sp::attach(scene.objects_state, scene.space_state, {item, holder, {{0, 0, 1}, {}}, true})
                  .status == sp::Status::Ok,
          "grasp plot test goods");
  require(sp::set_pose(scene.space, identity(scene.objects), transforms(scene.objects), holder,
                       {{1, 0, 0}, {}})
                  .status == sp::Status::Ok,
          "move visible custodian across plot boundary");
  scene.tick();
  require(refresh(true) == std::vector<Handle>{item} && scene.objects.plot[item.slot()] == 2 &&
              scene.objects.parent[item.slot()] == holder,
          "visible held goods did not follow its actual world plot");
  require(sp::custody(scene.objects_state, item, holder, scene.space_state).status ==
              sp::Status::Ok,
          "hide plot test goods in inventory");
  require(refresh(false) == std::vector<Handle>{item} && scene.objects.plot[item.slot()] == 0,
          "hidden inventory retained its custodian's plot");
  require(sp::custody(scene.objects_state, item, {}, scene.space_state).status == sp::Status::Ok,
          "move inventory goods to mailbox custody");
  require(refresh(false).empty() && scene.objects.plot[item.slot()] == 0,
          "mailbox goods gained a plot without being revealed");
}
void gravity_visibility_zones_and_snapshot() {
  Scene scene;
  auto floor = scene.box({30, 20, .2f}, {0, 0, -.2f});
  auto falling = scene.box({.4f, .4f, .4f}, {0, 0, 3}, sp::Motion::Dynamic, 0, false);
  ObjectInit init;
  init.model = 9;
  auto zone = create(scene.objects, init);
  Box volume({8, 8, 4});
  auto geometry = sp::import_geometry(scene.space, volume.source());
  require(sp::add_volume(scene.space, identity(scene.objects), transforms(scene.objects),
                         {zone, geometry.geometry, {}, sp::VolumeKind::Zone})
                  .status == sp::Status::Ok,
          "zone sensor add failed");
  for (int i = 0; i < 150; ++i)
    scene.tick();
  require(std::abs(scene.objects.position[falling.slot()].z) < .04f,
          "gravity did not land on baked mesh");
  if (!edge(scene.relations.supports, floor, falling)) {
    auto b = *sp::bounds(scene.space, identity(scene.objects), falling);
    std::cerr << "support probe base=" << scene.objects.position[falling.slot()].z
              << " bounds=" << b.min.z << "," << b.max.z << "\n";
    auto probe = sp::raycast(scene.space, identity(scene.objects), {0, 0, b.min.z + .015f},
                             {0, 0, b.min.z - .045f}, {falling, {}, false});
    std::cerr << "support ray=" << (probe ? probe->object.bits : 0) << " floor=" << floor.bits
              << "\n";
  }
  require(edge(scene.relations.supports, floor, falling), "support relation absent");
  require(edge(scene.relations.in_zone, falling, zone), "sleeping body lost zone membership");
  std::vector<sp::MotionRow> motion;
  sp::capture_motion(scene.space, identity(scene.objects), motion);
  require(motion.size() == 1 && motion[0].asleep, "sleep was not captured");
  const std::array selected{floor, falling, Handle::make(falling.slot(), falling.generation() + 1)};
  std::vector<sp::MotionRow> subset;
  sp::capture_motion(scene.space, identity(scene.objects), selected, subset);
  require(
      subset.size() == 1 && subset[0].object == falling && subset[0].asleep &&
          sp::motion(scene.space, identity(scene.objects), floor).has_value() &&
          !sp::motion(scene.space, identity(scene.objects), selected.back()),
      "selected capture omits static/stale rows while readonly motion supports every native mode");
  sp::restore_motion(scene.space, identity(scene.objects), motion);
  scene.tick();
  require(edge(scene.relations.in_zone, falling, zone), "restoring sleep lost zone");
  auto glass = scene.box({.1f, 4, 3}, {2, 0, 0}, sp::Motion::Static, 1);
  auto wall = scene.box({.1f, 4, 3}, {4, 0, 0});
  auto view = sp::visibility(scene.space, identity(scene.objects), {0, 0, 1}, {3, 0, 1}, {}, .01f);
  require(view.clear &&
              std::find(view.through.begin(), view.through.end(), glass) != view.through.end(),
          "transparent material did not pass sight");
  auto blocked =
      sp::visibility(scene.space, identity(scene.objects), {0, 0, 1}, {5, 0, 1}, {}, .01f);
  require(!blocked.clear, "opaque wall passed sight");
  auto ray = sp::raycast(scene.space, identity(scene.objects), {3, 0, 1}, {5, 0, 1}, {});
  require(ray && ray->object == wall, "ray did not hit real wall");
  scene.objects.position[zone.slot()] = {100, 100, 0};
  scene.tick();
  require(!edge(scene.relations.in_zone, falling, zone), "translated sensor retained old members");
  std::vector<sp::DebugLine> lines;
  sp::debug_lines(scene.space, {{{-20, -20, -1}, {20, 20, 10}}, true, false, false, true}, lines);
  require(!lines.empty(), "idle world debug is empty");
  std::vector<sp::RelationChange> changes;
  sp::remove(scene.space, identity(scene.objects), floor, parents(scene.objects),
             space_flags(scene.objects), scene.relations, changes);
  destroy(scene.objects, floor);
  for (int i = 0; i < 30; ++i)
    scene.tick();
  require(scene.objects.position[falling.slot()].z < -.2f,
          "removing support did not wake falling body");
}
void terrain_sweep() {
  Scene scene;
  sp::add_terrain(scene.space_state, 0, 0);
  auto leaf = scene.box({2.4f, .08f, 2.4f}, {18.55f, 45.9f, 0}, sp::Motion::Kinematic, 0, false);
  auto move = sp::translate(scene.space, identity(scene.objects), transforms(scene.objects), leaf,
                            {-.06f, 0, 0}, scene.dt);
  require(move.status == sp::Status::Ok, "terrain does not obstruct motion along its surface");
  move = sp::translate(scene.space, identity(scene.objects), transforms(scene.objects), leaf,
                       {0, 0, -.1f}, scene.dt);
  require(move.status == sp::Status::Obstructed && move.actual.position.z >= -.001f,
          "terrain blocks motion into its surface");
}
void attachments_and_sweep() {
  Scene scene;
  scene.box({20, 20, .2f}, {0, 0, -.2f});
  auto holder = scene.character({-3, 0, 0});
  auto item = scene.box({.2f, .2f, .2f}, {-2.5f, 0, 0}, sp::Motion::Dynamic, 0, false);
  std::vector<sp::RelationChange> changes;
  auto attached =
      sp::attach(scene.objects_state, scene.space_state, {item, holder, {{.5f, 0, 1}, {}}, true});
  require(attached.status == sp::Status::Ok, "grasp failed");
  require(scene.objects.parent[item.slot()] == holder && (scene.objects.flags[item.slot()] & Held),
          "grasp did not update masked flags/parent");
  auto cycle = sp::attach(scene.objects_state, scene.space_state, {holder, item, {}, false});
  require(cycle.status == sp::Status::AttachmentCycle, "attachment cycle accepted");
  auto release = sp::detach(scene.objects_state, scene.space_state,
                            {item, sp::Motion::Dynamic, {2, 0, 0}, {}});
  require(release.status == sp::Status::Ok, "release failed");
  for (int i = 0; i < 40; ++i)
    scene.tick();
  require(!(scene.objects.flags[item.slot()] & Held) && scene.objects.position[item.slot()].x > -2,
          "release velocity did not move object");
  auto door = scene.box({.15f, 2, 2.4f}, {0, 0, 0}, sp::Motion::Kinematic, 0, false);
  auto chair = scene.box({.6f, .6f, .8f}, {1.5f, 0, 0});
  auto move = sp::translate(scene.space, identity(scene.objects), transforms(scene.objects), door,
                            {3, 0, 0}, scene.dt);
  require(move.status == sp::Status::Obstructed && move.contact && move.contact->object == chair,
          "chair did not block kinematic sweep");
  require(move.actual.position.x < 1.2f, "sweep tunneled through chair");
  scene.tick();
  require(std::abs(scene.objects.position[door.slot()].x - move.actual.position.x) < .01f,
          "kinematic displacement was applied twice");
  scene.tick();
  require(std::abs(scene.objects.position[door.slot()].x - move.actual.position.x) < .01f,
          "kinematic body kept moving without a new motor request");
  sp::remove(scene.space, identity(scene.objects), item, parents(scene.objects),
             space_flags(scene.objects), scene.relations, changes);
  destroy(scene.objects, item);
  ObjectInit init;
  init.model = 8;
  auto replacement = create(scene.objects, init);
  require(replacement.slot() == item.slot() && replacement.generation() != item.generation(),
          "test did not reuse slot");
  require(sp::set_pose(scene.space, identity(scene.objects), transforms(scene.objects), item, {})
                  .status == sp::Status::StaleHandle,
          "stale handle changed replacement");
}
void snapshot_continuation_and_fixed_parts() {
  Scene original, restored;
  original.box({20, 20, .2f}, {0, 0, -.2f});
  restored.box({20, 20, .2f}, {0, 0, -.2f});
  auto first = original.box({.5f, .5f, .5f}, {-2, 0, 3}, sp::Motion::Dynamic, 0, false);
  auto second = restored.box({.5f, .5f, .5f}, {-2, 0, 3}, sp::Motion::Dynamic, 0, false);
  for (int i = 0; i < 8; ++i)
    original.tick();
  sp::set_pose(restored.space, identity(restored.objects), transforms(restored.objects), second,
               {original.objects.position[first.slot()], original.objects.rotation[first.slot()]});
  std::vector<sp::MotionRow> rows;
  sp::capture_motion(original.space, identity(original.objects), rows);
  rows[0].object = second;
  sp::restore_motion(restored.space, identity(restored.objects), rows);
  for (int i = 0; i < 10; ++i) {
    original.tick();
    restored.tick();
  }
  require((original.objects.position[first.slot()] - restored.objects.position[second.slot()])
                  .length() < .001f,
          "MOVE restore did not continue physical trajectory");
  require(sp::impulse(restored.space, identity(restored.objects), second, {10, 0, 0},
                      restored.objects.position[second.slot()])
                  .status == sp::Status::Ok,
          "dynamic impulse failed");
  sp::capture_motion(restored.space, identity(restored.objects), rows);
  require(rows[0].linear_velocity_m_s.x > .99f, "impulse did not use measured mass");

  auto grandchild = restored.box({.2f, .2f, .2f}, {-5, 0, 1}, sp::Motion::Kinematic, 0, false);
  auto part = restored.box({.2f, 1, 2}, {-5, 0, 0}, sp::Motion::Kinematic, 0, true);
  ObjectInit root_init;
  root_init.model = 3;
  auto root = create(restored.objects, root_init);

  require(sp::attach(restored.objects_state, restored.space_state,
                     {grandchild, part, {{0, 0, 2}, {}}, false})
                  .status == sp::Status::Ok,
          "nested part attach failed");
  require(
      sp::attach(restored.objects_state, restored.space_state, {part, root, {}, false}).status ==
          sp::Status::Ok,
      "empty assembly attach failed");
  require(sp::set_pose(restored.space, identity(restored.objects), transforms(restored.objects),
                       root, {{4, 0, 0}, {}})
                  .status == sp::Status::Ok,
          "empty root transform failed");
  restored.tick();
  require(std::abs(restored.objects.position[grandchild.slot()].x - 4) < .001f &&
              std::abs(restored.objects.position[grandchild.slot()].z - 2) < .001f,
          "nested attachments depended on slot order");
  auto ray = sp::raycast(restored.space, identity(restored.objects), {3, 0, 1}, {5, 0, 1}, {});
  require(ray && ray->object == part, "fixed part became noncollidable when attached");
  auto assembly = sp::bounds(restored.space, identity(restored.objects), root);
  require(assembly && assembly->max.z > 2.1f, "empty root did not include child bounds");
}
void crowd_doorway_and_stale_tiles() {
  Scene scene;
  scene.box({24, 10, .2f}, {0, 0, -.2f});
  scene.box({.2f, 4.1f, 3}, {0, 2.95f, 0});
  scene.box({.2f, 4.1f, 3}, {0, -2.95f, 0});
  auto a = scene.character({-3, -.3f, 0}), b = scene.character({3, .3f, 0});
  scene.wait_navigation();
  std::vector<sp::MoveRequest> requests{{a, 1, sp::PointGoal{{3, -.3f, 0}, .15f}, {}},
                                        {b, 2, sp::PointGoal{{-3, .3f, 0}, .15f}, {}}};
  unsigned completed = 0;
  float nearest = INFINITY;
  for (int i = 0; i < 300 && !requests.empty(); ++i) {
    scene.tick(requests);
    nearest = std::min(
        nearest, (scene.objects.position[a.slot()] - scene.objects.position[b.slot()]).length());
    for (const auto &progress : scene.result.intents)
      if (progress.state == sp::IntentState::Arrived) {
        ++completed;
        std::erase_if(requests,
                      [&](const auto &request) { return request.intent == progress.intent; });
      }
  }
  require(nearest > .55f, "crowd proxies bypassed character collision");
  require(completed == 2, "two crowd agents failed to negotiate doorway");

  Scene edits;
  edits.box({24, 16, .2f}, {0, 0, -.2f});
  auto walker = edits.character({-5, 0, 0});
  edits.wait_navigation();
  auto wall = edits.box({.2f, 16, 3}, {0, 0, 0});
  auto queued = sp::install_ready_navigation(edits.space);
  require(!queued.pending.empty(), "edit did not queue tile work");
  sp::set_pose(edits.space, identity(edits.objects), transforms(edits.objects), wall,
               {{7, 0, 0}, {}});
  sp::install_ready_navigation(edits.space);
  sp::set_pose(edits.space, identity(edits.objects), transforms(edits.objects), wall,
               {{10, 0, 0}, {}});
  edits.wait_navigation();
  auto route =
      sp::path(edits.space, identity(edits.objects), walker, sp::PointGoal{{5, 0, 0}, .1f}, {});
  require(route.status == sp::PathStatus::Ready, "stale worker result restored an obsolete wall");
  sp::MoveRequest gone{walker, 9, sp::ObjectGoal{wall, 1.5f}, {}};
  std::vector<sp::RelationChange> changes;
  sp::remove(edits.space, identity(edits.objects), wall, parents(edits.objects),
             space_flags(edits.objects), edits.relations, changes);
  destroy(edits.objects, wall);
  edits.tick({&gone, 1});
  require(edits.result.intents.size() == 1 &&
              edits.result.intents[0].state == sp::IntentState::TargetGone,
          "destroyed goal did not end exactly once");
}
void predicted_sensor_crossings_and_containment() {
  Scene scene;
  scene.box({20, 10, .2f}, {0, 0, -.2f});
  auto walker = scene.character({-4, 0, 0});
  ObjectInit init;
  init.model = 9;
  auto zone = create(scene.objects, init);
  auto container = create(scene.objects, init);
  Box shape({2, 6, 3});
  auto volume = sp::import_geometry(scene.space, shape.source());
  require(sp::add_volume(scene.space, identity(scene.objects), transforms(scene.objects),
                         {zone, volume.geometry, {}, sp::VolumeKind::Zone})
                  .status == sp::Status::Ok,
          "zone add failed");
  require(sp::add_volume(scene.space, identity(scene.objects), transforms(scene.objects),
                         {container, volume.geometry, {}, sp::VolumeKind::Container})
                  .status == sp::Status::Ok,
          "container add failed");
  scene.wait_navigation();
  sp::MoveRequest request{walker, 1, sp::PointGoal{{4, 0, 0}, .1f}, {}};
  int entered = 0, left = 0;
  bool contained = false, overlap_before_center = false;
  for (int i = 0; i < 150; ++i) {
    scene.tick({&request, 1});
    for (const auto &change : scene.result.relations)
      if (change.kind == sp::RelationKind::InZone && change.from == walker && change.to == zone) {
        if (change.added)
          ++entered;
        else
          ++left;
      }
    if (edge(scene.relations.in_zone, walker, zone) &&
        scene.objects.position[walker.slot()].x < -1.01f)
      overlap_before_center = true;
    if (edge(scene.relations.contains, container, walker)) {
      contained = true;
      require(scene.objects.parent[walker.slot()] == container,
              "containment did not set immediate parent");
    }
  }
  require(entered == 1 && left == 1 && contained && overlap_before_center,
          "scheduled zone entry/exit was missed or repeated");
  require(!scene.objects.parent[walker.slot()], "zone exit retained container parent");
}
void real_stair_navigation() {
  Scene scene;
  scene.box({30, 20, .2f}, {0, 0, -.2f});
  for (int i = 0; i < 10; ++i)
    scene.box({.35f, 3, .18f * (i + 1)}, {2 + i * .35f, 0, 0});
  scene.box({5, 5, .2f}, {7.825f, 0, 1.6f});
  auto walker = scene.character({-2, 0, 0});
  scene.wait_navigation();
  auto route =
      sp::path(scene.space, identity(scene.objects), walker, sp::PointGoal{{8, 0, 1.8f}, .15f}, {});
  require(route.status == sp::PathStatus::Ready && route.corners.size() > 1,
          "real staircase has no path");
  sp::MoveRequest move{walker, 1, sp::PointGoal{{8, 0, 1.8f}, .15f}, {}};
  bool arrived = false;
  for (int i = 0; i < 450; ++i) {
    scene.tick({&move, 1});
    for (const auto &progress : scene.result.intents)
      if (progress.state == sp::IntentState::Arrived)
        arrived = true;
    if (arrived)
      break;
  }
  auto position = scene.objects.position[walker.slot()];
  std::cout << "stair endpoint " << position.x << "," << position.y << "," << position.z << "\n";
  require(arrived && position.z > 1.7f, "CharacterVirtual failed to climb Detour staircase");
  sp::TravelBoundary closed{{{4, -5, 0}, {5, -5, 0}, {5, 5, 0}, {4, 5, 0}}, -1, 5};
  auto denied = sp::path(scene.space, identity(scene.objects), walker,
                         sp::PointGoal{{-2, 0, 0}, .1f}, {&closed, 1});
  require(denied.status == sp::PathStatus::AccessDenied, "forbidden entry path accepted");
  auto wall = scene.box({.2f, 20, 4}, {6, 0, 0});
  (void)wall;
  auto revision = sp::navigation_revision(scene.space);
  scene.wait_navigation();
  require(sp::navigation_revision(scene.space) > revision, "construction did not replace tiles");
  auto unreachable =
      sp::path(scene.space, identity(scene.objects), walker, sp::PointGoal{{-2, 0, 0}, .1f}, {});
  require(unreachable.status == sp::PathStatus::Unreachable,
          "new wall did not invalidate navigation");
}
void crowd_deviation_respects_retained_plot_boundaries() {
  Scene scene;
  scene.box({20, 12, .2f}, {0, 0, -.2f});
  const auto first = scene.character({-4, -.15f, 0});
  const auto second = scene.character({4, .15f, 0});
  scene.wait_navigation();
  for (int i = 0; i < 4; ++i)
    scene.tick();
  const std::array<sp::TravelBoundary, 2> closed{
      {{{{-2, -.25f, 0}, {2, -.25f, 0}, {2, -3, 0}, {-2, -3, 0}}, -1, 5},
       {{{-2, .25f, 0}, {2, .25f, 0}, {2, 3, 0}, {-2, 3, 0}}, -1, 5}}};
  std::vector<sp::MoveRequest> moves{{first, 1, sp::PointGoal{{4, -.15f, 0}, .1f}},
                                     {second, 2, sp::PointGoal{{-4, .15f, 0}, .1f}}};
  bool stopped = false;
  for (int i = 0; i < 160 && !stopped; ++i) {
    scene.tick(moves, closed);
    for (const auto &progress : scene.result.intents)
      stopped |= progress.path_status == sp::PathStatus::AccessDenied;
    for (const auto body : {first, second}) {
      const auto p = scene.objects.position[body.slot()];
      require(p.x <= -2 || p.x >= 2 || std::abs(p.y) <= .25f,
              "crowd avoidance cannot carry a walker into a forbidden plot");
    }
  }
  require(stopped && scene.objects.position[first.slot()].x > -3.5f,
          "a legal walk advances and then stops when crowd motion reaches a closed lot edge");
}
void displaced_walk_replans_with_fresh_boundaries() {
  Scene scene;
  scene.box({24, 14, .2f}, {0, 0, -.2f});
  const auto walker = scene.character({-5, 0, 0});
  scene.wait_navigation();
  for (int i = 0; i < 4; ++i)
    scene.tick();
  sp::TravelBoundary closed{{{0, 1, 0}, {1, 1, 0}, {1, 4, 0}, {0, 4, 0}}, -1, 5};
  const sp::MoveRequest move{walker, 1, sp::PointGoal{{5, 0, 0}, .1f}};
  scene.tick({&move, 1}, {&closed, 1});
  require(scene.result.intents[0].path_status == sp::PathStatus::Ready,
          "the first route passes beside the closed plot");
  sp::set_pose(scene.space, identity(scene.objects), transforms(scene.objects), walker,
               {{-2, 3, 0}, {}});
  const auto plans = scene.planned_moves;
  scene.tick({&move, 1}, {&closed, 1});
  require(
      scene.planned_moves == plans + 1 &&
          scene.result.intents[0].path_status == sp::PathStatus::AccessDenied &&
          scene.objects.position[walker.slot()].x < 0,
      "displacement outside the route footprint replans and checks a newly relevant closed lot");
}
void waiting_navigation_finishes_after_unrelated_tiles() {
  Scene scene;
  scene.box({4, 6, .2f}, {-5, 0, -.2f});
  const auto walker = scene.character({-5, 0, 0});
  scene.wait_navigation();
  for (int i = 0; i < 4; ++i)
    scene.tick();
  scene.box({1, 1, .2f}, {50, 50, 0});
  const sp::MoveRequest move{walker, 1, sp::PointGoal{{5, 0, 0}, .1f}};
  sp::plan_move(scene.space, identity(scene.objects), move, {});
  require(!sp::needs_plan(scene.space, identity(scene.objects), move),
          "an unchanged pending route does not repeat its path query");
  scene.wait_navigation();
  require(sp::needs_plan(scene.space, identity(scene.objects), move),
          "finishing the unrelated pending tiles releases the waiting attempt");
  scene.tick({&move, 1});
  require(scene.result.intents[0].state == sp::IntentState::Blocked,
          "a genuinely unreachable goal ends after pending navigation finishes");
}
void object_goal_uses_reachable_side() {
  Scene scene;
  scene.box({12, 12, .2f}, {-4, 0, -.2f});
  const auto target = scene.box({2, 2, 2}, {2, 0, 0});
  const auto walker = scene.character({-6, 0, 0});
  scene.wait_navigation();
  for (int i = 0; i < 4; ++i)
    scene.tick();
  const sp::MoveRequest move{walker, 1, sp::ObjectGoal{target, 1.5f}};
  const auto route = sp::path(scene.space, identity(scene.objects), walker, move.goal, {});
  require(route.status == sp::PathStatus::Ready && route.corners.back().x < 1,
          "an object with its far side off the floor retains the reachable near-side route");
  bool arrived = false;
  for (int i = 0; i < 200 && !arrived; ++i) {
    scene.tick({&move, 1});
    arrived = scene.result.intents[0].state == sp::IntentState::Arrived;
  }
  require(arrived && scene.objects.position[walker.slot()].x > -.5f &&
              scene.objects.position[walker.slot()].x < 1,
          "the walker reaches the supported side within reach of the object");
}
void one_plan_follows_multiple_corners() {
  Scene scene;
  scene.box({24, 20, .2f}, {0, 0, -.2f});
  scene.box({.4f, 14, 3}, {-2, -3, 0});
  scene.box({.4f, 14, 3}, {2, 3, 0});
  const auto walker = scene.character({-8, 0, 0});
  scene.wait_navigation();
  for (int i = 0; i < 4; ++i)
    scene.tick();
  const sp::MoveRequest move{walker, 1, sp::PointGoal{{8, 0, 0}, .15f}};
  const auto route = sp::path(scene.space, identity(scene.objects), walker, move.goal, {});
  require(route.status == sp::PathStatus::Ready && route.corners.size() >= 4,
          "alternating walls require a route with at least three corners");
  bool arrived = false;
  for (int i = 0; i < 650 && !arrived; ++i) {
    scene.tick({&move, 1});
    arrived = scene.result.intents[0].state == sp::IntentState::Arrived;
  }
  require(arrived && scene.objects.position[walker.slot()].x > 7.7f,
          "one-corner crowd targets carry the walker through the entire winding route");
  require(scene.planned_moves == 1, "advancing route corners does not repeat path planning");
}
void tile_rebuilds_invalidate_only_overlapping_routes() {
  Scene scene;
  scene.box({30, 14, .2f}, {0, 0, -.2f});
  const auto walker = scene.character({-8, 0, 0});
  scene.wait_navigation();
  for (int i = 0; i < 4; ++i)
    scene.tick();
  const sp::MoveRequest move{walker, 1, sp::PointGoal{{8, 0, 0}, .15f}};
  scene.tick({&move, 1});
  require(scene.planned_moves == 1, "initial walking intent plans once");
  const auto revision = sp::navigation_revision(scene.space);
  scene.box({1, 1, .2f}, {50, 50, 0});
  scene.wait_navigation();
  require(sp::navigation_revision(scene.space) > revision,
          "the distant construction really installs new navigation tiles");
  scene.tick({&move, 1});
  require(scene.planned_moves == 1, "a rebuilt tile outside the route leaves its plan intact");
  scene.box({.6f, .6f, 2}, {0, 0, 0});
  scene.wait_navigation();
  scene.tick({&move, 1});
  require(scene.planned_moves == 2 && scene.result.intents[0].path_status == sp::PathStatus::Ready,
          "a rebuilt tile on the route plans a new path around the construction");
}
template <class Function> void benchmark(const char *label, int count, Function function) {
  std::vector<double> times;
  times.reserve(count);
  for (int i = 0; i < count; ++i) {
    auto start = std::chrono::steady_clock::now();
    function();
    times.push_back(
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
            .count());
  }
  std::sort(times.begin(), times.end());
  std::cout << label << " us p50=" << times[times.size() / 2]
            << " p95=" << times[times.size() * 95 / 100] << " max=" << times.back()
            << " n=" << times.size() << "\n";
}
void benchmarks() {
  Scene scene;
  scene.box({40, 40, .2f}, {0, 0, -.2f});
  for (int i = 0; i < 30; ++i)
    scene.box({.5f, .5f, 1}, {float(i % 10) * 2 - 10, float(i / 10) * 3, 0});
  auto walker = scene.character({-15, -5, 0});
  scene.wait_navigation();
  benchmark("ray_32_objects", 1000, [&] {
    sp::raycast(scene.space, identity(scene.objects), {-15, -5, 1}, {15, 5, 1},
                {walker, {}, false});
  });
  sp::MoveRequest request{walker, 2, sp::PointGoal{{15, -5, 0}, .1f}, {}};
  benchmark("character_step", 100, [&] { scene.tick({&request, 1}); });
  std::vector<sp::MotionRow> rows;
  benchmark("motion_capture", 1000,
            [&] { sp::capture_motion(scene.space, identity(scene.objects), rows); });
}
void sensor_benchmark() {
  Scene scene;
  for (int i = 0; i < 100; ++i)
    scene.box({.2f, .2f, .2f}, {float(i % 10) - 5, float(i / 10) - 5, 0}, sp::Motion::Static, 0,
              false);
  const auto shape = sp::sensor_box_geometry(scene.space, {20, 20, 4});
  for (int i = 0; i < 20; ++i) {
    ObjectInit init;
    init.model = 1;
    const auto zone = create(scene.objects, init);
    require(sp::add_volume(scene.space, identity(scene.objects), transforms(scene.objects),
                           {zone, shape, {{0, 0, 1}, {}}, sp::VolumeKind::Zone})
                    .status == sp::Status::Ok,
            "benchmark zone add");
  }
  scene.tick();
  benchmark("sensor_step_100_objects_20_overlapping_zones", 200, [&] { scene.tick(); });
  require(scene.result.relations.empty() && scene.result.moved.empty(),
          "steady zone contacts published fake changes");
}
void requested_speed_respects_authored_profile() {
  Scene scene;
  scene.box({40, 20, .2f}, {0, 0, -.2f});
  auto walker = scene.character({-5, 0, 0});
  scene.wait_navigation();
  sp::MoveRequest request{walker, 1, sp::PointGoal{{10, 0, 0}, .1f}, .2f};
  for (int i = 0; i < 15; ++i)
    scene.tick({&request, 1});
  const auto slow = scene.objects.position[walker.slot()].x + 5;
  require(slow > .05f && slow < .3f, "requested slow speed was ignored");
  request.speed_m_s = 100;
  const auto previous = scene.objects.position[walker.slot()].x;
  for (int i = 0; i < 15; ++i)
    scene.tick({&request, 1});
  const auto fast = scene.objects.position[walker.slot()].x - previous;
  require(fast > .8f && fast < 1.6f, "requested speed did not clamp to authored maximum");
}

void custody_and_empty_root_updates() {
  Scene scene;
  auto child = scene.box({1, 1, 1}, {0, 0, 0});
  ObjectInit init;
  init.model = 1;
  auto parent = create(scene.objects, init);
  require(sp::register_logical_root(scene.space, identity(scene.objects), parent).status ==
              sp::Status::Ok,
          "empty root registration failed");

  require(
      sp::attach(scene.objects_state, scene.space_state, {child, parent, {{1, 0, 0}, {}}, false})
              .status == sp::Status::Ok,
      "empty-root part attach failed");
  sp::set_pose(scene.space, identity(scene.objects), transforms(scene.objects), parent,
               {{4, 0, 0}, {}});
  scene.tick();
  require(std::abs(scene.objects.position[child.slot()].x - 5) < .001f,
          "moving an empty root did not move its child");
  auto occupant = scene.character({5, 0, 1});
  require(
      sp::attach(scene.objects_state, scene.space_state, {occupant, child, {{0, 0, 1}, {}}, false})
              .status == sp::Status::Ok,
      "occupant attach failed");
  auto custodian = create(scene.objects, init);
  const auto child_position = scene.objects.position[child.slot()];
  require(
      sp::custody(scene.objects_state, parent, custodian, scene.space_state, {&child, 1}).status ==
          sp::Status::Ok,
      "assembly custody failed");
  require(!sp::bounds(scene.space, identity(scene.objects), child) &&
              scene.objects.parent[child.slot()] == parent &&
              scene.objects.position[child.slot()] == child_position &&
              edge(scene.relations.attached_to, child, parent),
          "assembly extraction left geometry or destroyed ordinary part topology");
  require(sp::bounds(scene.space, identity(scene.objects), occupant).has_value() &&
              !scene.objects.parent[occupant.slot()],
          "assembly collection hid an unrelated occupant");
  require(!sp::raycast(scene.space, identity(scene.objects), {5, -2, .5f}, {5, 2, .5f}, {}),
          "assembly child remained query-visible in custody");
  auto item = scene.box({1, 1, 1}, {8, 0, 0});
  require(sp::custody(scene.objects_state, item, parent, scene.space_state).status ==
              sp::Status::Ok,
          "custody transition failed");
  require(scene.objects.parent[item.slot()] == parent &&
              !sp::bounds(scene.space, identity(scene.objects), item),
          "custody item remained query-visible or lost its custodian");
  require(!sp::raycast(scene.space, identity(scene.objects), {8, -2, .5f}, {8, 2, .5f}, {}),
          "custody retained native collision geometry");
  require(sp::custody(scene.objects_state, item, {}, scene.space_state).status == sp::Status::Ok &&
              !scene.objects.parent[item.slot()],
          "offline mailbox custody rejected a null parent");
  require(sp::custody(scene.objects_state, parent, child, scene.space_state).status ==
              sp::Status::AttachmentCycle,
          "custody permitted a parent cycle");
}

void visible_custody_preserves_scene_and_preflights_capacity() {
  Scene scene;
  const auto holder = scene.character({0, 0, 0});
  const auto goods = scene.box({1, 1, 1}, {2, 0, 1}, sp::Motion::Kinematic);
  const auto part = scene.box({.2f, .2f, .2f}, {3, 0, 1}, sp::Motion::Kinematic);
  std::vector<sp::RelationChange> changes;
  require(sp::attach(scene.objects_state, scene.space_state, {part, goods, {{1, 0, 0}, {}}, false})
                  .status == sp::Status::Ok,
          "visible custody part fixture");
  const std::array descendants{part};
  const auto original = scene.objects.position[goods.slot()];
  const auto available = sp::capacity(scene.space).native_available;
  auto apply = [&](sp::VisibleCustody change) {
    require(sp::can_visible_custody(scene.space, identity(scene.objects), parents(scene.objects),
                                    change, descendants)
                    .status == sp::Status::Ok,
            "visible custody preflight");
    return sp::visible_custody(scene.objects_state, scene.space_state, change, descendants);
  };
  require(apply({goods, holder, true}).status == sp::Status::Ok &&
              (scene.objects.flags[goods.slot()] & Held),
          "administrative restoration attaches real visible goods without snapping");
  require(apply({goods, {}, false}).status == sp::Status::Ok &&
              !scene.objects.parent[goods.slot()] && !(scene.objects.flags[goods.slot()] & Held) &&
              scene.objects.position[goods.slot()] == original &&
              scene.objects.parent[part.slot()] == goods &&
              sp::capacity(scene.space).native_available == available,
          "placed escrow retains actual pose, geometry, native slots and declared attachment");
  const auto hit =
      sp::raycast(scene.space, identity(scene.objects), {2, -2, 1.5f}, {2, 2, 1.5f}, {});
  require(hit && hit->object == goods, "visible escrow remains a real ray target");
  require(apply({goods, holder, true}).status == sp::Status::Ok &&
              scene.objects.parent[goods.slot()] == holder &&
              scene.objects.position[goods.slot()] == original,
          "cancellation restores visible holding at its actual world pose");
  require(sp::can_visible_custody(scene.space, identity(scene.objects), parents(scene.objects),
                                  {goods, part, false}, descendants)
                  .status == sp::Status::AttachmentCycle,
          "visible custody preflights the resulting parent cycle");

  auto config = settings();
  config.max_bodies = 2;
  Scene limited(config);
  ObjectInit init;
  init.model = 1;
  const auto logical_holder = create(limited.objects, init);
  require(
      sp::register_logical_root(limited.space, identity(limited.objects), logical_holder).status ==
          sp::Status::Ok,
      "capacity custody logical endpoint");
  const auto placed = limited.box({1, 1, 1}, {0, 0, 0});
  const auto occupant = limited.box({.2f, .2f, .2f}, {2, 0, 0}, sp::Motion::Kinematic);
  changes.clear();
  require(sp::attach(limited.objects_state, limited.space_state,
                     {occupant, placed, {{2, 0, 0}, {}}, false})
                  .status == sp::Status::Ok,
          "full-pool unrelated occupant");
  require(sp::capacity(limited.space).native_available == 0 &&
              sp::can_visible_custody(limited.space, identity(limited.objects),
                                      parents(limited.objects), {placed, logical_holder, true})
                      .status == sp::Status::Capacity &&
              !limited.objects.parent[placed.slot()] &&
              limited.objects.parent[occupant.slot()] == placed,
          "visible transition rejects missing temporary native capacity without changing either "
          "endpoint");
  require(sp::can_custody(limited.space, identity(limited.objects), placed, {},
                          parents(limited.objects))
                      .status == sp::Status::Ok &&
              sp::custody(limited.objects_state, placed, {}, limited.space_state).status ==
                  sp::Status::Ok &&
              !limited.objects.parent[occupant.slot()] &&
              sp::body_motion(limited.space, identity(limited.objects), occupant)->motion ==
                  sp::Motion::Dynamic &&
              sp::bounds(limited.space, identity(limited.objects), occupant) &&
              !sp::bounds(limited.space, identity(limited.objects), placed),
          "hidden collection uses retiring slot to restore unrelated occupant in exact-full pool");
}

void movable_logical_assembly() {
  Scene scene;
  scene.box({20, 20, .2f}, {0, 0, -.2f});
  auto carrier = scene.character({0, -3, 0});
  ObjectInit init;
  init.model = 7;
  init.position = {0, 0, 2};
  init.flags = Static;
  auto root = create(scene.objects, init);
  require(sp::register_logical_root(scene.space, identity(scene.objects), root).status ==
              sp::Status::Ok,
          "register compound root");
  auto first = scene.box({.4f, .4f, .5f}, {-.5f, 0, 2}, sp::Motion::Kinematic, .5f, false);
  auto second = scene.box({.4f, .4f, .5f}, {.5f, 0, 2}, sp::Motion::Kinematic, 0, false);
  auto non_solid = scene.box({.2f, .2f, .2f}, {0, 0, -1}, sp::Motion::Kinematic, 0, false, false);

  for (auto child : {first, second, non_solid}) {
    auto local = scene.objects.position[child.slot()] - scene.objects.position[root.slot()];
    require(sp::attach(scene.objects_state, scene.space_state, {child, root, {local, {}}, false})
                    .status == sp::Status::Ok,
            "attach compound child");
  }
  std::array members{first, second, non_solid};
  require(
      sp::assemble(scene.space, identity(scene.objects), transforms(scene.objects), root, members)
              .status == sp::Status::Ok,
      "Jolt convex compound construction");
  auto glass = sp::visibility(scene.space, identity(scene.objects), {-.5f, -2, 2.2f},
                              {-.5f, 2, 2.2f}, {}, .001f);
  require(glass.clear && std::abs(glass.transmission - .5f) < .01f &&
              glass.through == std::vector<Handle>{first},
          "compound keeps child optical truth without double attenuation");
  require(sp::attach(scene.objects_state, scene.space_state, {root, carrier, {{0, 1, 1}, {}}, true})
                  .status == sp::Status::Ok,
          "grasp movable assembly");
  scene.tick();
  require(std::abs(scene.objects.position[first.slot()].y + 2) < .02f,
          "ordinary child follows held root");
  require(
      sp::detach(scene.objects_state, scene.space_state, {root, sp::Motion::Dynamic, {0, 0, 0}, {}})
              .status == sp::Status::Ok,
      "release convex assembly dynamically");
  require(
      sp::impulse(scene.space, identity(scene.objects), root, {30, 0, 0}, {0, -2, 1.2f}).status ==
          sp::Status::Ok,
      "compound impulse");
  std::vector<sp::MotionRow> motion;
  sp::capture_motion(scene.space, identity(scene.objects), motion);
  auto root_motion =
      std::find_if(motion.begin(), motion.end(), [&](const auto &m) { return m.object == root; });
  require(root_motion != motion.end() && std::abs(root_motion->linear_velocity_m_s.x - 1) < .01f,
          "compound mass sums each ordinary child exactly once");
  for (int i = 0; i < 100; ++i)
    scene.tick();
  require(scene.objects.position[root.slot()].z > -.05f &&
              scene.objects.position[root.slot()].z < .05f,
          "released assembly rests on solid children while non-solid child crosses floor");
  require(scene.objects.parent[first.slot()] == root && scene.objects.parent[second.slot()] == root,
          "dynamic release preserves ordinary part graph");
}

void structural_capacity_preflight() {
  auto config = settings();
  config.max_bodies = 4;
  config.max_characters = 2;
  Scene scene(config);
  Box box({1, 1, 1});
  auto geometry = sp::import_geometry(scene.space, box.source()).geometry;
  sp::BodySpec future{{}, geometry, sp::Motion::Dynamic, {.5f, 0, 0}, 10, false, false, {}};
  require(sp::capacity(scene.space).native_available == 4 &&
              sp::can_add(scene.space, std::span(&future, 1), 3).status == sp::Status::Ok &&
              sp::can_add(scene.space, std::span(&future, 1), 4).status == sp::Status::Capacity,
          "batch preflight includes explicit sensors without allocating future handles");
  auto invalid = future;
  invalid.mass_kg = 0;
  require(sp::can_add(scene.space, std::span(&invalid, 1)).status == sp::Status::UnsupportedMotion,
          "invalid mass fails before native allocation");
  invalid = future;
  invalid.geometry = 0;
  require(sp::can_add(scene.space, std::span(&invalid, 1)).status == sp::Status::MissingGeometry,
          "missing geometry is distinct from capacity");
  auto first = scene.box({1, 1, 1}, {0, 0, 0});
  auto second = scene.box({1, 1, 1}, {5, 0, 0});
  for (auto kind : {sp::VolumeKind::Zone, sp::VolumeKind::Container})
    require(sp::add_volume(scene.space, identity(scene.objects), transforms(scene.objects),
                           {first, geometry, {}, kind})
                    .status == sp::Status::Ok,
            "sensor consumes actual native capacity");
  auto changed = future;
  changed.object = second;
  require(sp::capacity(scene.space).native_available == 0 &&
              sp::can_add(scene.space, std::span{&future, 1}).status == sp::Status::Capacity &&
              sp::can_rebuild_assembly(scene.space, std::span{&changed, 1}, 0,
                                       std::span{&second, 1}, false)
                      .status == sp::Status::Ok &&
              sp::add_volume(scene.space, identity(scene.objects), transforms(scene.objects),
                             {second, geometry, {}, sp::VolumeKind::Zone})
                      .status == sp::Status::Capacity,
          "full pool rejects new allocation but actual extraction/rebuild credits its old body");
  auto replacement = sp::can_add(scene.space, std::span(&future, 1), 1, std::span(&first, 1));
  require(replacement.status == sp::Status::Ok && replacement.available_native == 3,
          "retirement credits actual body plus both owner sensor bodies");
  std::vector<sp::RelationChange> changes;
  sp::remove(scene.space, identity(scene.objects), first, parents(scene.objects),
             space_flags(scene.objects), scene.relations, changes);
  require(sp::capacity(scene.space).native_available == 3 &&
              sp::can_rebuild_assembly(scene.space, std::span{&changed, 1}, 0,
                                       std::span{&second, 1}, false)
                      .status == sp::Status::Ok,
          "capacity matches owner retirement and the actual assembly rebuild preflight");
  auto walker = scene.character({10, 0, 0});
  future.motion = sp::Motion::Character;
  future.character = sp::CharacterSpec{.3f, 1.8f, 70, 100, 1.8f, 8, .24f, .7f, .3f};
  std::array characters{future, future};
  require(sp::capacity(scene.space).characters_available == 1 &&
              sp::can_add(scene.space, characters).status == sp::Status::Capacity &&
              sp::can_add(scene.space, characters, 0, std::span(&walker, 1)).status ==
                  sp::Status::Ok,
          "CharacterVirtual inner body and independent crowd capacity are counted");
  future.character->height_m = .1f;
  require(sp::can_add(scene.space, std::span(&future, 1)).status == sp::Status::UnsupportedMotion,
          "unsupported character capsule fails before lifecycle/resource writes");
}

void movable_physical_assembly() {
  Scene scene;
  scene.box({20, 20, .2f}, {0, 0, -.2f});
  auto root = scene.box({.4f, .4f, .5f}, {0, 0, 2}, sp::Motion::Dynamic, .8f, false);
  auto first = scene.box({.4f, .4f, .5f}, {1, 0, 2}, sp::Motion::Kinematic, .5f, false);
  auto second = scene.box({.4f, .4f, .5f}, {-1, 0, 2}, sp::Motion::Kinematic, 0, false);
  std::vector<sp::RelationChange> changes;
  for (auto child : {first, second})
    require(sp::attach(scene.objects_state, scene.space_state,
                       {child, root, {{scene.objects.position[child.slot()].x, 0, 0}, {}}, false})
                    .status == sp::Status::Ok,
            "physical root child attachment");
  std::array members{first, second};
  const auto before = sp::capacity(scene.space).native_available;
  require(
      sp::assemble(scene.space, identity(scene.objects), transforms(scene.objects), root, members)
                  .status == sp::Status::Ok &&
          sp::capacity(scene.space).native_available == before - 1,
      "physical compound includes explicit retained root query body");
  auto transmission = [&](Handle h) {
    auto p = scene.objects.position[h.slot()] + Vec3{0, 0, .2f};
    return sp::visibility(scene.space, identity(scene.objects), p + Vec3{0, -2, 0},
                          p + Vec3{0, 2, 0}, {}, .001f);
  };
  require(std::abs(transmission(root).transmission - .8f) < .01f &&
              std::abs(transmission(first).transmission - .5f) < .01f,
          "root and child retain distinct optical material without aggregate duplication");
  require(
      sp::impulse(scene.space, identity(scene.objects), root, {30, 0, 0}, {0, 0, 2.2f}).status ==
          sp::Status::Ok,
      "physical-root impulse");
  std::vector<sp::MotionRow> motion;
  sp::capture_motion(scene.space, identity(scene.objects), motion);
  require(motion.size() == 1 && motion[0].object == root &&
              std::abs(motion[0].linear_velocity_m_s.x - 1) < .01f,
          "one MOVE row sums root and both disjoint child masses");
  for (unsigned i = 0; i < 8; ++i)
    scene.tick();
  auto sight = transmission(root);
  require(sight.through == std::vector<Handle>{root} && std::abs(sight.transmission - .8f) < .01f,
          "actual moving root query geometry follows aggregate pose");
  Box shape({.4f, .4f, .5f});
  const auto geometry = sp::import_geometry(scene.space, shape.source()).geometry;
  const std::array rebuilt{
      sp::BodySpec{root, geometry, sp::Motion::Dynamic, {.6f, 0, .8f}, 10, false, false, {}},
      sp::BodySpec{first, geometry, sp::Motion::Kinematic, {.6f, 0, .5f}, 10, false, false, {}},
      sp::BodySpec{second, geometry, sp::Motion::Kinematic, {.6f, 0, 0}, 10, false, false, {}}};
  const std::array old_scope{root, first, second};
  require(sp::can_rebuild_assembly(scene.space, rebuilt, 0, old_scope, true).status ==
              sp::Status::Ok,
          "actual physical assembly edit preflights before extraction");
  const auto edit = sp::extract_assembly(scene.space, identity(scene.objects), root, members);
  const std::array attachments{sp::Attachment{first, root, {{1, 0, 0}, {}}, false},
                               sp::Attachment{second, root, {{-1, 0, 0}, {}}, false}};
  require(sp::rebuild_assembly(scene.objects_state, scene.space_state, edit, rebuilt, {}, {},
                               attachments)
                      .status == sp::Status::Ok &&
              sp::capacity(scene.space).native_available == before - 1,
          "actual extraction/rebuild preserves the aggregate and physical root query capacity");
  sp::capture_motion(scene.space, identity(scene.objects), motion);
  require(motion.size() == 1 && motion[0].object == root &&
              (motion[0].linear_velocity_m_s - edit.motion[0].linear_velocity_m_s).length() < .001f,
          "one root MOVE row and actual velocity survive the production assembly edit path");
  sp::remove(scene.space, identity(scene.objects), first, parents(scene.objects),
             space_flags(scene.objects), scene.relations, changes);
  destroy(scene.objects, first);
  require(sp::bounds(scene.space, identity(scene.objects), root).has_value() &&
              sp::assembly_root(scene.space, identity(scene.objects), second) == root &&
              sp::body_motion(scene.space, identity(scene.objects), root)->motion ==
                  sp::Motion::Dynamic,
          "ordinary single member retirement preserves enclosing dynamic aggregate collider");
}

void nested_nonsolid_assembly() {
  auto config = settings();
  config.gravity_m_s2 = {};
  Scene scene(config);
  ObjectInit init;
  init.model = 7;
  init.flags = Static;
  auto root = create(scene.objects, init);
  init.position = {1, 0, 0};
  auto logical = create(scene.objects, init);
  for (auto h : {root, logical})
    require(sp::register_logical_root(scene.space, identity(scene.objects), h).status ==
                sp::Status::Ok,
            "nested logical registration");
  auto child = scene.box({.5f, .5f, .5f}, {2, 0, 0}, sp::Motion::Kinematic, .5f, false, false);

  for (const auto &attachment : {sp::Attachment{logical, root, {{1, 0, 0}, {}}, false},
                                 sp::Attachment{child, logical, {{1, 0, 0}, {}}, false}})
    require(sp::attach(scene.objects_state, scene.space_state, attachment).status == sp::Status::Ok,
            "nested logical attachment");
  sp::rebuild_attachments(scene.space, identity(scene.objects), transforms(scene.objects),
                          parents(scene.objects), scene.relations.attached_to);
  std::array descendants{logical, child};
  require(sp::assemble(scene.space, identity(scene.objects), transforms(scene.objects), root,
                       descendants, sp::Motion::Dynamic)
                      .status == sp::Status::Ok &&
              !sp::body_motion(scene.space, identity(scene.objects), root)->collision_solid,
          "non-solid-only compound has explicit query-only dynamic geometry");
  require(
      sp::impulse(scene.space, identity(scene.objects), root, {10, 0, 0}, {2, 0, .25f}).status ==
          sp::Status::Ok,
      "non-solid compound still carries actual mass");
  scene.tick();
  require(scene.objects.position[root.slot()].x > .04f &&
              (scene.objects.position[logical.slot()] - scene.objects.position[root.slot()] -
               Vec3{1, 0, 0})
                      .length() < .001f &&
              (scene.objects.position[child.slot()] - scene.objects.position[root.slot()] -
               Vec3{2, 0, 0})
                      .length() < .001f &&
              std::ranges::find(scene.result.moved, logical) != scene.result.moved.end(),
          "nested logical attachment follows actual motion and publishes its own changed pose");
}

void assembly_geometry_rebuild() {
  auto config = settings();
  config.gravity_m_s2 = {};
  Scene scene(config);
  auto root = scene.box({.4f, .4f, .5f}, {0, 0, 2}, sp::Motion::Static, .8f, false);
  auto seat = scene.box({.4f, .4f, .5f}, {1, 0, 2}, sp::Motion::Kinematic, .5f, false);
  auto removed = scene.box({.4f, .4f, .5f}, {-1, 0, 2}, sp::Motion::Kinematic, 0, false);
  auto carrier = scene.character({5, -3, 0});
  auto seated = scene.character({10, 0, 0});
  auto displaced = scene.character({15, 0, 0});
  std::vector<sp::RelationChange> changes;
  auto attach = [&](Handle child, Handle parent, Vec3 local, bool grasp = false) {
    require(sp::attach(scene.objects_state, scene.space_state, {child, parent, {local, {}}, grasp})
                    .status == sp::Status::Ok,
            "assembly edit fixture attachment");
  };
  attach(seat, root, {1, 0, 0});
  attach(removed, root, {-1, 0, 0});
  std::array old_parts{seat, removed};
  require(sp::assemble(scene.space, identity(scene.objects), transforms(scene.objects), root,
                       old_parts, sp::Motion::Kinematic)
                  .status == sp::Status::Ok,
          "initial assembly promotion");
  attach(root, carrier, {0, 2, 2}, true);
  scene.tick();
  attach(seated, seat, {0, 0, .6f});
  attach(displaced, removed, {0, 0, .6f});
  Box shape({.6f, .6f, .5f});
  const auto geometry = sp::import_geometry(scene.space, shape.source()).geometry;
  const auto sensor = sp::sensor_box_geometry(scene.space, {1, 1, 1});
  require(sp::add_volume(scene.space, identity(scene.objects), transforms(scene.objects),
                         {root, sensor, {}, sp::VolumeKind::Container})
                  .status == sp::Status::Ok,
          "old assembly sensor");
  std::array before{root, seat, removed};
  std::array future{
      sp::BodySpec{root, geometry, sp::Motion::Static, {.6f, 0, .9f}, 20, false, false, {}},
      sp::BodySpec{seat, geometry, sp::Motion::Kinematic, {.6f, 0, .5f}, 10, false, false, {}},
      sp::BodySpec{{}, geometry, sp::Motion::Kinematic, {.6f, 0, 0}, 10, false, false, {}}};
  const auto check = sp::can_rebuild_assembly(scene.space, future, 1, before, true);
  require(check.status == sp::Status::Ok && check.required_native == 5 &&
              check.available_native == sp::capacity(scene.space).native_available + 5,
          "rebuild preflight credits each aggregate/query/child/sensor exactly once");
  require(sp::assembly_root(scene.space, identity(scene.objects), seat) == root &&
              sp::can_rebuild_assembly(scene.space, future, 0, std::span(&seat, 1), true).status ==
                  sp::Status::UnsupportedMotion,
          "partial rigid member edit must widen mechanical scope before extraction");
  auto edit = sp::extract_assembly(scene.space, identity(scene.objects), root, old_parts);
  require(edit.in_scene && edit.promoted &&
              !sp::body_motion(scene.space, identity(scene.objects), root),
          "extraction removes native geometry while preserving assembly state");
  sp::remove(scene.space, identity(scene.objects), removed, parents(scene.objects),
             space_flags(scene.objects), scene.relations, changes);
  destroy(scene.objects, removed);
  require(!scene.objects.parent[displaced.slot()] &&
              sp::body_motion(scene.space, identity(scene.objects), displaced)->motion ==
                  sp::Motion::Character &&
              scene.objects.parent[seated.slot()] == seat,
          "retiring extracted seat detaches only its own external occupant");
  ObjectInit init;
  init.model = 1;
  init.material = 1;
  init.flags = ScriptedMotion;
  init.position = scene.objects.position[root.slot()] + Vec3{2, 0, 0};
  const auto added = create(scene.objects, init);
  future[2].object = added;
  std::array internal{sp::Attachment{seat, root, {{1, 0, 1}, {}}, false},
                      sp::Attachment{added, root, {{2, 0, 0}, {}}, false}};
  std::array volumes{sp::VolumeSpec{root, sensor, {}, sp::VolumeKind::Container}};
  require(sp::rebuild_assembly(scene.objects_state, scene.space_state, edit, future, {}, volumes,
                               internal)
                  .status == sp::Status::Ok,
          "physical held assembly rebuild");
  require(scene.objects.parent[root.slot()] == carrier &&
              (scene.objects.flags[root.slot()] & Held) &&
              scene.objects.parent[added.slot()] == root &&
              scene.objects.position[seated.slot()] ==
                  scene.objects.position[seat.slot()] + Vec3{0, 0, .6f},
          "rebuild preserves grasp and follows retained occupant to new part pose");
  require(
      sp::detach(scene.objects_state, scene.space_state, {root, sp::Motion::Dynamic, {1, 0, 0}, {}})
              .status == sp::Status::Ok,
      "rebuilt held assembly releases as actual dynamic body");
  std::array parts{seat, added};
  auto velocity = [&] {
    std::vector<sp::MotionRow> rows;
    sp::capture_motion(scene.space, identity(scene.objects), rows);
    require(std::ranges::count(rows, root, &sp::MotionRow::object) == 1,
            "physical root query and aggregate produce only one authoritative MOVE row");
    return std::ranges::find(rows, root, &sp::MotionRow::object)->linear_velocity_m_s;
  };
  require(std::abs(velocity().x - 1) < .001f, "release sets actual aggregate velocity");
  auto empty_edit = sp::extract_assembly(scene.space, identity(scene.objects), root, parts);
  std::array only_children{future[1], future[2]};
  require(sp::rebuild_assembly(scene.objects_state, scene.space_state, empty_edit, only_children,
                               std::span(&root, 1), {}, internal)
                      .status == sp::Status::Ok &&
              sp::is_logical_root(scene.space, identity(scene.objects), root) &&
              std::abs(velocity().x - 1) < .001f,
          "physical to geometry-free root preserves actual rigid flight");
  auto start = scene.objects.position[root.slot()];
  for (unsigned i = 0; i < 5; ++i)
    scene.tick();
  require(scene.objects.position[root.slot()].x > start.x + .2f,
          "restored velocity advances actual replacement collider");
  const auto flying_velocity = velocity();
  auto solid_edit = sp::extract_assembly(scene.space, identity(scene.objects), root, parts);
  require(sp::rebuild_assembly(scene.objects_state, scene.space_state, solid_edit, future, {}, {},
                               internal)
                      .status == sp::Status::Ok &&
              !sp::is_logical_root(scene.space, identity(scene.objects), root) &&
              (velocity() - flying_velocity).length() < .001f,
          "geometry-free to physical root preserves ordinary handle and flight");
  require(sp::custody(scene.objects_state, root, {}, scene.space_state, parts).status ==
              sp::Status::Ok,
          "store rebuilt assembly in offline mailbox");
  const auto available = sp::capacity(scene.space).native_available;
  auto hidden = sp::extract_assembly(scene.space, identity(scene.objects), root, parts);
  require(!hidden.in_scene &&
              sp::rebuild_assembly(scene.objects_state, scene.space_state, hidden, future, {},
                                   volumes, internal)
                      .status == sp::Status::Ok &&
              sp::capacity(scene.space).native_available == available &&
              !sp::bounds(scene.space, identity(scene.objects), root) &&
              !sp::bounds(scene.space, identity(scene.objects), seat) &&
              (scene.objects.flags[added.slot()] & Held),
          "custody replacement creates no colliders or sensors and preserves hidden child graph");
}

void density_benchmarks() {
  for (auto count : {10000, 100000}) {
    auto cfg = settings();
    cfg.max_bodies = count + 128;
    sp::Space space(cfg);
    amber::ObjectState objects_state;
    Objects &objects = objects_state.objects;
    sp::PhysicalRelations relations;
    sp::StepResult result;
    Box shape({.5f, .5f, .5f});
    auto geometry = sp::import_geometry(space, shape.source()).geometry;
    for (int i = 0; i < count; ++i) {
      ObjectInit init;
      init.model = 1;
      init.material = 1;
      init.position = {float(i % 1000) * 2 + 100, float(i / 1000) * 2 + 100, 0};
      init.flags = Static;
      auto h = create(objects, init);
      require(sp::add(space, identity(objects), transforms(objects),
                      {h, geometry, sp::Motion::Static, {.5f, 0, 0}, 1, false, false, {}})
                      .status == sp::Status::Ok,
              "density body add failed");
    }
    Step now = 0;
    auto tick = [&] { sp::step(space, objects_state, relations, ++now, .05f, {}, result); };
    tick();
    auto label = std::string("inert_") + std::to_string(count);
    benchmark(label.c_str(), 100, tick);
    std::vector<Handle> moving;
    for (int i = 0; i < 32; ++i) {
      ObjectInit init;
      init.model = 1;
      init.material = 1;
      init.position = {float(i % 8) * 2, float(i / 8) * 2, 100};
      init.flags = Dynamic;
      auto h = create(objects, init);
      moving.push_back(h);
      require(sp::add(space, identity(objects), transforms(objects),
                      {h, geometry, sp::Motion::Dynamic, {.5f, 0, 0}, 1, false, false, {}})
                      .status == sp::Status::Ok,
              "density moving body add failed");
    }
    label += "_dynamic32";
    benchmark(label.c_str(), 100, tick);
    std::vector<sp::MotionRow> rows;
    benchmark((label + "_selected_capture").c_str(), 1000, [&] {
      sp::capture_motion(space, identity(objects), moving, rows);
      require(rows.size() == moving.size(), "selected density capture");
    });
    benchmark((label + "_motion_query").c_str(), 1000, [&] {
      for (auto h : moving)
        require(sp::motion(space, identity(objects), h).has_value(),
                "selected density motion query");
    });
  }
}

void assembly_benchmarks() {
  auto config = settings();
  config.gravity_m_s2 = {};
  Scene scene(config);
  auto root = scene.box({.4f, .4f, .4f}, {0, 0, 2}, sp::Motion::Dynamic, 0, false);
  Box shape({.2f, .2f, .2f});
  const auto geometry = sp::import_geometry(scene.space, shape.source()).geometry;
  std::vector<Handle> parts, before{root};
  std::vector<sp::BodySpec> bodies{
      {root, geometry, sp::Motion::Dynamic, {.5f, 0, 0}, 10, false, false, {}}};
  std::vector<sp::Attachment> attachments;
  std::vector<sp::RelationChange> changes;
  for (unsigned i = 0; i < 16; ++i) {
    auto h = scene.box({.2f, .2f, .2f}, {float(i + 1), 0, 2}, sp::Motion::Kinematic, 0, false);
    parts.push_back(h);
    before.push_back(h);
    bodies.push_back({h, geometry, sp::Motion::Kinematic, {.5f, 0, 0}, 10, false, false, {}});
    attachments.push_back({h, root, {{float(i + 1), 0, 0}, {}}, false});
    require(sp::attach(scene.objects_state, scene.space_state, attachments.back()).status ==
                sp::Status::Ok,
            "benchmark assembly attachment");
  }
  require(sp::assemble(scene.space, identity(scene.objects), transforms(scene.objects), root, parts)
                  .status == sp::Status::Ok,
          "benchmark initial compound");
  benchmark("assembly_preflight_17", 100, [&] {
    require(sp::can_rebuild_assembly(scene.space, bodies, 0, before, true).status == sp::Status::Ok,
            "benchmark assembly preflight");
  });
  benchmark("assembly_extract_rebuild_17", 100, [&] {
    auto edit = sp::extract_assembly(scene.space, identity(scene.objects), root, parts);
    changes.clear();
    require(sp::rebuild_assembly(scene.objects_state, scene.space_state, edit, bodies, {}, {},
                                 attachments)
                    .status == sp::Status::Ok,
            "benchmark actual native assembly rebuild");
  });
}

} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--assembly-bench") {
      assembly_benchmarks();
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--density") {
      density_benchmarks();
      return 0;
    }
    returns_release_seated_standing_and_static_occupants();
    expired_native_queries_stop();
    sensor_contacts_use_body_overlap();
    plot_membership_follows_real_motion_and_hidden_custody();
    gravity_visibility_zones_and_snapshot();
    terrain_sweep();
    attachments_and_sweep();
    real_stair_navigation();
    crowd_deviation_respects_retained_plot_boundaries();
    displaced_walk_replans_with_fresh_boundaries();
    waiting_navigation_finishes_after_unrelated_tiles();
    object_goal_uses_reachable_side();
    one_plan_follows_multiple_corners();
    tile_rebuilds_invalidate_only_overlapping_routes();
    snapshot_continuation_and_fixed_parts();
    crowd_doorway_and_stale_tiles();
    predicted_sensor_crossings_and_containment();
    requested_speed_respects_authored_profile();
    custody_and_empty_root_updates();
    visible_custody_preserves_scene_and_preflights_capacity();
    movable_logical_assembly();
    structural_capacity_preflight();
    movable_physical_assembly();
    nested_nonsolid_assembly();
    assembly_geometry_rebuild();
    benchmarks();
    sensor_benchmark();
    std::cout << "space tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << "space test: " << error.what() << "\n";
    return 1;
  }
}
