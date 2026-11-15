#include "observe/observe.hpp"
#include "space_fixture.hpp"

namespace ob = amber::observe;
namespace re = amber::react;
using namespace testspace;
struct Values {
  re::Terms terms;
  re::Materials materials;
  re::AssetMeasurements assets;
  re::Properties properties;
  re::Processes processes;
  re::InstanceSettings instance;
  re::ModelSettings models;
  re::PlotSettings plots;
  re::WorldSettings world;
  re::SharedColumns shared;
  re::RelationsRead relations;
  re::RuntimeMeasurements runtime;
  re::ValueRead read(const Objects &o) {
    return {terms,    identity(o), o.plot, o.params, materials, assets,    properties, processes,
            instance, models,      plots,  world,    shared,    relations, runtime,    0};
  }
};
void observation_geometry_and_live_light() {
  Scene scene;
  auto body = scene.character({0, 0, 0});
  auto visible = scene.box({1, 1, 2}, {0, 5, 0});
  auto glass = scene.box({3, .1f, 3}, {0, 3, 0}, sp::Motion::Static, .5f, false);
  auto hidden = scene.box({1, 1, 2}, {5, 5, 0});
  scene.box({3, .1f, 3}, {3, 3, 0}, sp::Motion::Static, 0, false);
  auto partial = scene.box({1, 1, 4}, {-5, 5, 0}, sp::Motion::Static, 0, false);
  scene.box({3, .1f, 1.6f}, {-3, 3, 0}, sp::Motion::Static, 0, false);
  auto large = scene.box({10, 1, 10}, {10, 60, 0}, sp::Motion::Static, 0, false);
  auto held = scene.box({.2f, .2f, .2f}, {70, 70, 0}, sp::Motion::Static, 0, false);
  auto lamp = scene.box({.1f, .1f, .1f}, {0, 5, 3}, sp::Motion::Static, 0, false);
  Values v;
  v.terms.rows.resize(2);
  v.terms.rows[1].id = 1;
  v.terms.rows[1].name = "custom.intensity";
  v.terms.rows[1].source = re::Source::State;
  v.processes.resize(2);
  v.processes[1].set(lamp.slot(), re::Process{100, 0, 100, 0, 1000, 1});
  ob::Lights lights(8);
  ob::ZoneAmbients ambients;
  Relation parts;
  ob::set_light(lights, {lamp, {0, 0, 0}, TermRef{1}, 20, .1f}, transforms(scene.objects));
  ob::Settings settings{30, 1.5f, 256, 1, .1f, .1, 40, 8, 40};
  auto scan = [&](Step now) {
    return ob::scan(scene.space, identity(scene.objects), transforms(scene.objects), parts,
                    {body, {0, 0, 1}, {0, 1, 0}, 1000}, held, settings, lights, ambients,
                    v.read(scene.objects), {}, now, 20, 1, 0, sp::Deadline::max());
  };
  auto first = scan(0);
  require(first.complete, "look query incomplete");
  auto find = [](const auto &rows, Handle h) -> const ob::Seen * {
    for (auto &r : rows)
      if (r.object == h)
        return &r;
    return nullptr;
  };
  auto *seen = find(first.things, visible);
  require(seen &&
              std::find(seen->through.begin(), seen->through.end(), glass) != seen->through.end(),
          "glass must transmit sight and report through");
  require(find(first.things, partial), "partially occluded tall object remains visible");
  require(!find(first.things, hidden), "opaque wall must hide object at all samples");
  require(find(first.things, held)->held && find(first.things, held)->distance_m == 0,
          "held object must be reported independently of sight");
  require(find(first.far, large) && find(first.far, large)->salience == ob::Seen::Salience::Large,
          "far large object missing");
  double before = seen->lux;
  auto second = scan(20);
  require(find(second.things, visible)->lux > before,
          "process-backed intensity must be freshly evaluated");
  std::vector<sp::LightSample> samples;
  ob::light_samples(lights, identity(scene.objects), v.read(scene.objects), 0, 20, {0, 5, 2},
                    samples);
  require(samples.size() == 1 && samples[0].intensity_cd == 100, "indexed light sample");
  auto dark = scan(0);
  require(!find(dark.things, glass)->lit, "dim geometry remains visible with lit false");
  ob::remove_light(lights, lamp);
  ob::light_samples(lights, identity(scene.objects), v.read(scene.objects), 0, 20, {0, 5, 2},
                    samples);
  require(samples.empty() && lights.candidates.empty(), "removed light leaves no retained cells");
}
void zones_subscriptions_hearing_and_cache() {
  Scene scene;
  Values values;
  auto receiver = scene.box({.2f, .2f, .2f}, {0, 0, 1}, sp::Motion::Static, 0, false);
  ObjectInit init;
  init.model = 5;
  auto zone = create(scene.objects, init);
  Box volume({8, 8, 4});
  auto geometry = sp::import_geometry(scene.space, volume.source());
  require(sp::add_volume(scene.space, identity(scene.objects), transforms(scene.objects),
                         {zone, geometry.geometry, {}, sp::VolumeKind::Zone})
                  .status == sp::Status::Ok,
          "sensor add");
  std::vector<Handle> zones;
  require(sp::volumes_at(scene.space, identity(scene.objects), {0, 0, 1}, sp::VolumeKind::Zone,
                         sp::Deadline::max(), zones) &&
              zones == std::vector<Handle>{zone},
          "zone actual containment");
  require(sp::volumes_at(scene.space, identity(scene.objects), {9, 0, 1}, sp::VolumeKind::Zone,
                         sp::Deadline::max(), zones) &&
              zones.empty(),
          "outside zone");
  ob::ZoneAmbients ambient;
  ob::set_zone(ambient, {zone, 40., 2});
  require(ob::ambient_at(ambient, values.read(scene.objects), std::span(&zone, 1), 0, 20, 1) == 40,
          "authored zone ambient");
  ob::LightSubscriptions subscriptions(8);
  ob::subscribe(subscriptions, {receiver, {0, 0, 1}, 5});
  const auto distant = scene.box({.2f, .2f, .2f}, {30, 0, 1});
  ob::subscribe(subscriptions, {distant, {30, 0, 1}, 5});
  ob::Lights lights(8);
  ob::set_light(lights, {zone, {}, 100., 40, .1f}, transforms(scene.objects));
  require(ob::take_dirty(subscriptions).size() == 2, "initial light subscription dirty");
  sp::Bounds edited{{8, 0, 0}, {8.1f, 1, 2}};
  ob::dirty_geometry(subscriptions, lights, std::span(&edited, 1));
  require(ob::take_dirty(subscriptions).size() == 2,
          "occluder near source invalidates actual light range across distant cells");
  ob::dirty_light(subscriptions, {100, 100, 0}, 1);
  require(ob::take_dirty(subscriptions).empty(), "distant light causes no work");
  ob::unsubscribe(subscriptions, receiver, 5);
  ob::unsubscribe(subscriptions, distant, 5);
  require(subscriptions.cells.empty(), "unsubscribe prunes cells");
  ob::ActiveViews views(32);
  ob::track_view(views, 1, {0, 0, 1}, 30);
  ob::track_view(views, 2, {100, 100, 1}, 30);
  require(ob::take_dirty_views(views).size() == 2, "new views dirty");
  sp::Bounds change{{-1, -1, 0}, {1, 1, 2}};
  ob::dirty_views(views, std::span(&change, 1));
  require(ob::take_dirty_views(views) == std::vector<std::uint64_t>{1},
          "spatial observer invalidation excludes unrelated sessions");
  ob::remove_view(views, 1);
  ob::remove_view(views, 2);
  require(views.cells.empty(), "roaming observer cells are pruned");
  ob::Hearing hearing;
  std::vector<ob::Listener> listeners{{1, receiver, {0, 0, 1}}, {2, receiver, {10, 0, 1}}};
  for (int i = 0; i < 45; ++i)
    ob::emit_sound(hearing,
                   {receiver, {0, 0, 1}, 5, std::string("message ") + std::to_string(i), 1, {}},
                   listeners);
  std::size_t omitted;
  auto heard = ob::take_heard(hearing, 1, 40, omitted);
  require(omitted == 5 && heard.size() == 40 && hearing.queues.empty(),
          "hearing newest count and distance-only delivery");
  ob::SessionCache cache;
  ob::PreparedThing thing{{receiver, 1, 0, 2, false, false, true, {}, ob::Seen::Salience::Near},
                          {receiver,
                           "arbitrary.model",
                           {{"size", 4}},
                           {"claim", "fictional description", "text"},
                           {"custom.action"}}};
  ob::SelfDescription self{receiver, {}, {0, 0, 0}, 0, nullptr, {}};
  std::vector<std::string> actions{"body.custom"};
  auto render = [&] {
    return ob::observation(cache, 0, 20, self, std::span(&thing, 1), {}, heard, actions, {});
  };
  auto first = render();
  require(first["things"][0].contains("measured") && first["things"][0]["says"]["name"] == "claim",
          "measured truth and maker text separate");
  auto second = render();
  require(second["things"][0].size() == 3 && !second.contains("body_actions"),
          "unchanged observation is compact");
  thing.description.measured["size"] = 5;
  auto changed = render();
  require(changed["things"][0].contains("measured"), "live value invalidates descriptive cache");
  thing.seen.reachable = false;
  auto far = render();
  require(!far["things"][0].contains("actions"), "out-of-reach action discovery hidden");
  ob::forget(cache);
  require(render().contains("body_actions"), "reconnect resets per-session cache");
}
void benchmark_inert_density() {
  for (std::uint32_t count : {0u, 10000u, 100000u}) {
    auto config = settings();
    config.max_bodies = 110000;
    Scene scene(config);
    Values values;
    Relation parts;
    ob::Lights lights(8);
    ob::ZoneAmbients zones;
    auto observer = scene.character({0, -2, 0});
    for (int i = 0; i < 12; ++i)
      scene.box({.3f, .3f, 1}, {float(i % 4) * 3 - 4.5f, float(i / 4) * 3 + 2, 0},
                sp::Motion::Static, 0, false);
    Box shape({.2f, .2f, .2f});
    auto geometry = sp::import_geometry(scene.space, shape.source());
    for (std::uint32_t i = 0; i < count; ++i) {
      ObjectInit init;
      init.model = 1;
      init.position = {1000 + float(i % 1000) * 2, 1000 + float(i / 1000) * 2, 0};
      init.flags = Static;
      auto h = create(scene.objects, init);
      require(sp::add(scene.space, identity(scene.objects), transforms(scene.objects),
                      {h, geometry.geometry, sp::Motion::Static, {.5f, 0, 0}, 1, false, false, {}})
                      .status == sp::Status::Ok,
              "inert density body add");
    }
    scene.tick(); // Complete the native broadphase update before steady-state sampling.
    ob::Settings settings{30, 1.5f, 256, 1, .1f, .1, 40, 8, 40};
    std::vector<double> samples;
    for (int i = 0; i < 200; ++i) {
      auto start = std::chrono::steady_clock::now();
      auto result = ob::scan(scene.space, identity(scene.objects), transforms(scene.objects), parts,
                             {observer, {0, -2, 1}, {0, 1, 0}, 10}, {}, settings, lights, zones,
                             values.read(scene.objects), {}, 0, 20, 20, 0, sp::Deadline::max());
      require(result.complete && !result.things.empty(),
              "benchmark must perform actual geometry look");
      samples.push_back(
          std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
              .count());
    }
    std::sort(samples.begin(), samples.end());
    std::cout << "look_12_local_inert_" << count << " us p50=" << samples[100]
              << " p95=" << samples[190] << '\n';
  }
}
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--bench") {
      benchmark_inert_density();
      return 0;
    }
    observation_geometry_and_live_light();
    zones_subscriptions_hearing_and_cache();
    std::cout << "observation tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
