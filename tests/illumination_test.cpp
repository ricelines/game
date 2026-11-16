#include "runtime_fixture.hpp"
#include "space_fixture.hpp"
#include "step/step.hpp"

#include <cmath>

namespace {
using namespace amber;
namespace bi = step;
namespace ob = observe;
namespace re = react;
using testspace::require;
void actual_lighting_watch_sampling() {
  test_runtime::Fixture f;
  space::State space_state(testspace::settings(), 8);
  auto &world = space_state.space;
  f.terms.rows[2].source = re::Source::Measured;
  f.terms.rows[2].measurement = re::MeasurementRole::Runtime;
  f.terms.rows[5].kind = re::Kind::Quantity;
  f.objects.position[f.a.slot()] = {30, 0, 1};
  f.objects.position[f.b.slot()] = {8, 4, 0};
  f.objects.position[f.c.slot()] = {0, 0, 1};
  testspace::Box wall({1, 2, 3});
  const auto shape = space::import_geometry(world, wall.source());
  require(
      space::add(world, identity(f.objects), transforms(f.objects),
                 {f.b, shape.geometry, space::Motion::Kinematic, {1, 0, 0}, 10, false, false, {}})
              .status == space::Status::Ok,
      "real illumination occluder");
  space_flags(f.objects).set(f.b.slot(), ScriptedMotion);
  ObjectInit init;
  init.model = 1;
  init.position = {30, 0, 0};
  const auto zone = create(f.objects, init);
  const auto zone_shape = space::sensor_box_geometry(world, {4, 4, 4});
  require(space::add_volume(world, identity(f.objects), transforms(f.objects),
                            {zone, zone_shape, {}, space::VolumeKind::Zone})
                  .status == space::Status::Ok,
          "actual ambient zone sensor");
  f.state(f.c, 1, 9000);
  f.state(zone, 5, 2);
  const auto watcher = f.add(R"(return {kind='model',watches={'novel.result'},
    on={['novel.result']=function(self)end}})");
  f.bind(watcher, f.a);
  auto &points = space_state.light_subscriptions;
  auto &views = space_state.active_views;
  ob::track_view(views, 1, {30, 0, 1}, 1);
  ob::track_view(views, 2, {100, 0, 1}, 1);
  ob::take_dirty_views(views);
  auto &lights = space_state.lights;
  auto &zones = space_state.zones;
  auto values = f.values();
  bi::IlluminationSources sources;
  f.models.rows.resize(4);
  f.objects.model[f.c.slot()] = 2;
  f.objects.model[zone.slot()] = 3;

  auto &assets = f.catalog_state.assets;
  for (ModelId id = 1; id <= 3; ++id) {
    f.models.rows[id].id = id;
    f.models.rows[id].asset = std::to_string(id);
    assets.rows[std::to_string(id)].manifest["bindings"] = nlohmann::json::object();
  }
  assets.rows["2"].manifest["bindings"]["light"] = {
      {"site", {0, 0, 0}}, {"intensity", "novel.heat"}, {"range", 40}};
  assets.rows["3"].manifest["bindings"]["zone"] = {{"ambient", "novel.ready"}, {"priority", 1}};
  auto install = [&](Handle object, Step now) {
    bi::register_illumination(sources, space_state, transforms(f.objects), f.catalog_state, values,
                              std::span(&object, 1), .1f,
                              amber::step::Clock{.step = now, .hz = 20});
  };
  install(f.c, 0);
  install(zone, 0);
  bi::sync_illuminance_reads(points, f.subscriptions, identity(f.objects), transforms(f.objects), 2,
                             std::span(&f.a, 1));
  require(points.rows.size() == 1, "actual watched runtime term creates one sample point");
  auto sample = [&](Step now) {
    f.effects = {};
    bi::sample_illuminance(points, world, identity(f.objects), lights, zones, f.runtime, 1,
                           f.effects);
    return re::read(values, f.a, 2, now, 20).value.numeric();
  };
  auto near = [](double a, double b) { return std::abs(a - b) < .001; };
  require(near(sample(0), 12),
          "actual runtime lux combines authored ambient and inverse square light");
  require(!f.effects.ordered.empty(), "illumination publishes normal derived-fact watch effects");
  auto move_wall = [&](Vec3 position) {
    require(space::set_pose(world, identity(f.objects), transforms(f.objects), f.b, {position, {}})
                    .status == space::Status::Ok,
            "actual occluder move");
    space::StepResult physics;
    space::PhysicalRelations relations;
    space::step(world, f.objects_state, relations, 1, .05f, {}, physics);
    ob::dirty_geometry(points, lights, physics.dirty_bounds, &views);
  };
  move_wall({8, 0, 0});
  require(near(sample(1), 2),
          "near-source occluder invalidates and shadows receiver three cells away");
  move_wall({8, 4, 0});
  require(near(sample(2), 12), "moving actual occluder away restores subscribed lux");
  const auto changed = [&](Handle object, TermId term, Step now) {
    const FactKey fact{object, term};
    bi::illumination_values_changed(sources, space_state, values, std::span(&fact, 1), now, 20);
  };
  const auto advance = [&](Step now) {
    bi::advance_illumination(sources, space_state, values, now, 20);
  };
  re::set_rate(f.react_state, f.terms.rows[1], f.c, -9000, 0, 9000,
               amber::step::Clock{.step = 0, .hz = 20}, f.effects);
  install(f.c, 0);
  require(sources.running_lights.contains(f.c.bits),
          "restored active light process is indexed without needing a new state mutation");
  changed(f.c, 1, 0);
  require(ob::take_dirty_views(views) == std::vector<std::uint64_t>{1},
          "light changes invalidate actual influenced observers only");
  advance(20);
  require(sources.running_lights.empty() && near(sample(20), 2),
          "terminal process sample publishes darkness before stopping periodic work");
  re::set_rate(f.react_state, f.terms.rows[1], f.c, 9000, 0, 9000,
               amber::step::Clock{.step = 20, .hz = 20}, f.effects);
  changed(f.c, 1, 20);
  advance(30);
  require(!sources.running_lights.empty() && near(sample(30), 7),
          "reversing a clamped light process rearms visible sampling");
  advance(40);
  require(sources.running_lights.empty() && near(sample(40), 12), "positive terminal light sample");
  f.state(zone, 5, 20, 40);
  changed(zone, 5, 40);
  require(near(sample(40), 30), "live zone ambient state invalidates its real sensor region");
  re::set_rate(f.react_state, f.terms.rows[5], zone, 20, 0, 40,
               amber::step::Clock{.step = 40, .hz = 20}, f.effects);
  install(zone, 40);
  require(sources.running_zones.contains(zone.bits),
          "restored or rebound ambient process is indexed from current binding");
  changed(zone, 5, 40);
  advance(60);
  require(sources.running_zones.empty() && near(sample(60), 50),
          "live ambient process samples its terminal bound and stops");
  std::vector<space::RelationChange> changes;
  space::PhysicalRelations relations;
  space::remove(world, identity(f.objects), zone, parents(f.objects), space_flags(f.objects),
                relations, changes);
  bi::retire_illumination(sources, lights, zones, points, views, world, std::span(&zone, 1));
  space::StepResult physics;
  space::step(world, f.objects_state, relations, 61, .05f, {}, physics);
  ob::dirty_geometry(points, lights, physics.dirty_bounds);
  require(near(sample(61), 11), "removing real ambient sensor restores world ambient");
  bi::retire_illuminance_reads(points, f.runtime, 2, std::span(&f.a, 1));
  require(points.rows.empty() && !f.runtime.values.contains({f.a, 2}),
          "retirement removes exact-generation subscription and runtime answer");
  const auto second = f.add(R"(return {kind='model', watches={'novel.result'},
    on={['novel.result']=function(self)end}})");
  f.bind(second, f.c, re::SyncMode::Change);
  bi::sync_illuminance_reads(points, f.subscriptions, identity(f.objects), transforms(f.objects), 2,
                             std::span(&f.c, 1));
  require(points.rows.contains({f.c.bits, 2}), "own runtime watch creates its actual sample point");
  re::remove_watches(f.subscriptions, {f.c, second});
  bi::sync_illuminance_reads(points, f.subscriptions, identity(f.objects), transforms(f.objects), 2,
                             std::span(&f.c, 1));
  require(points.rows.empty(), "removing own runtime watch removes its sample point");
}
} // namespace
int main() {
  try {
    actual_lighting_watch_sampling();
    std::cout
        << "Actual geometry, light/ambient processes and runtime watch sampling checks passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
