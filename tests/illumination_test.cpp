#include "bindings/illumination.hpp"
#include "runtime_fixture.hpp"
#include "space_fixture.hpp"

#include <cmath>

namespace {
using namespace amber;
namespace bi = bindings;
namespace ob = observe;
namespace re = react;
using testspace::require;
void actual_lighting_watch_sampling() {
  test_runtime::Fixture f;
  space::Space world(testspace::settings());
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
  ob::LightSubscriptions points(8);
  ob::ActiveViews views(8);
  ob::track_view(views, 1, {30, 0, 1}, 1);
  ob::track_view(views, 2, {100, 0, 1}, 1);
  ob::take_dirty_views(views);
  ob::Lights lights(8);
  ob::set_light(lights, {f.c, {}, TermRef{1}, 40, .1f}, transforms(f.objects));
  ob::ZoneAmbients zones;
  ob::set_zone(zones, {zone, TermRef{5}, 1});
  auto values = f.values();
  bi::IlluminanceReads reads{points, f.subscriptions, identity(f.objects), transforms(f.objects),
                             2};
  bi::IlluminanceSampling sampling{points, world, values, lights, zones, f.runtime, 1, 100};
  bi::sync_illuminance_reads(reads, std::span(&f.a, 1));
  require(points.rows.size() == 1, "actual watched runtime term creates one sample point");
  auto sample = [&](Step now) {
    f.effects = {};
    bi::sample_illuminance(sampling, now, 20, f.effects);
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
    space::step(world, identity(f.objects), transforms(f.objects), parents(f.objects),
                space_flags(f.objects), relations, 1, .05f, {}, physics);
    ob::dirty_geometry(points, lights, physics.dirty_bounds, &views);
  };
  move_wall({8, 0, 0});
  require(near(sample(1), 2),
          "near-source occluder invalidates and shadows receiver three cells away");
  move_wall({8, 4, 0});
  require(near(sample(2), 12), "moving actual occluder away restores subscribed lux");
  bi::RunningLights running_lights;
  bi::RunningZones running_zones;
  bi::IlluminationProcesses running{running_lights, running_zones, lights, zones, f.processes};
  re::set_rate(f.properties, f.processes, f.terms.rows[1], f.c, -9000, 0, 9000, 0, 20, f.effects);
  bi::register_illumination_processes(running, std::span(&f.c, 1));
  require(running_lights.sources.contains(f.c.bits),
          "restored active light process is indexed without needing a new state mutation");
  bi::light_value_changed(running_lights, lights, points, views, f.processes, {f.c, 1});
  require(ob::take_dirty_views(views) == std::vector<std::uint64_t>{1},
          "light changes invalidate actual influenced observers only");
  bi::advance_running_lights(running_lights, lights, points, views, f.processes, 20, 20);
  require(running_lights.sources.empty() && near(sample(20), 2),
          "terminal process sample publishes darkness before stopping periodic work");
  re::set_rate(f.properties, f.processes, f.terms.rows[1], f.c, 9000, 0, 9000, 20, 20, f.effects);
  bi::light_value_changed(running_lights, lights, points, views, f.processes, {f.c, 1});
  bi::advance_running_lights(running_lights, lights, points, views, f.processes, 30, 20);
  require(!running_lights.sources.empty() && near(sample(30), 7),
          "reversing a clamped light process rearms visible sampling");
  bi::advance_running_lights(running_lights, lights, points, views, f.processes, 40, 20);
  require(running_lights.sources.empty() && near(sample(40), 12), "positive terminal light sample");
  f.state(zone, 5, 20, 40);
  bi::zone_value_changed(running_zones, zones, points, views, world, f.processes, {zone, 5});
  require(near(sample(40), 30), "live zone ambient state invalidates its real sensor region");
  re::set_rate(f.properties, f.processes, f.terms.rows[5], zone, 20, 0, 40, 40, 20, f.effects);
  bi::register_illumination_processes(running, std::span(&zone, 1));
  require(running_zones.sources.contains(zone.bits),
          "restored or rebound ambient process is indexed from current binding");
  bi::zone_value_changed(running_zones, zones, points, views, world, f.processes, {zone, 5});
  bi::advance_running_zones(running_zones, zones, points, views, world, f.processes, 60, 20);
  require(running_zones.sources.empty() && near(sample(60), 50),
          "live ambient process samples its terminal bound and stops");
  std::vector<space::RelationChange> changes;
  space::PhysicalRelations relations;
  space::remove(world, identity(f.objects), zone, parents(f.objects), space_flags(f.objects),
                relations, changes);
  ob::remove_zone(zones, zone);
  space::StepResult physics;
  space::step(world, identity(f.objects), transforms(f.objects), parents(f.objects),
              space_flags(f.objects), relations, 61, .05f, {}, physics);
  ob::dirty_geometry(points, lights, physics.dirty_bounds);
  require(near(sample(61), 11), "removing real ambient sensor restores world ambient");
  ob::dirty_light(points, {0, 0, 1}, 40);
  sampling.query_ms = 0;
  require(near(sample(62), 11) && !points.dirty.empty(),
          "incomplete geometry query retains prior answer and schedules retry");
  sampling.query_ms = 100;
  sample(62);
  require(points.dirty.empty(), "completed retry clears dirty point");
  bi::retire_illuminance_reads(reads, f.runtime, std::span(&f.a, 1));
  require(points.rows.empty() && !f.runtime.values.contains({f.a, 2}),
          "retirement removes exact-generation subscription and runtime answer");
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
