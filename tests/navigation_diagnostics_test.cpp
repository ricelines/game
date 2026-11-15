#include "bindings/navigation.hpp"
#include "space_fixture.hpp"

namespace bi = amber::bindings;
using namespace testspace;
namespace {
bi::InvocationInput input(Handle body, PlayerId author = 7) {
  bi::InvocationInput result;
  result.action_caller = Caller(author);
  result.action_body = body;
  return result;
}
std::vector<bi::NavigationNotice> await_notice(Scene &scene, bi::NavigationDiagnostics &binding) {
  const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  for (;;) {
    const auto installed = sp::install_ready_navigation(scene.space);
    auto notices = bi::publish_navigation(
        binding, installed, std::chrono::steady_clock::now() + std::chrono::microseconds(1000));
    if (!notices.empty())
      return notices;
    require(std::chrono::steady_clock::now() < limit, "navigation notice timeout");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
void actual_stairs_rebuild_and_delivery() {
  Scene scene;
  scene.box({30, 20, .2f}, {0, 0, -.2f});
  std::vector<Handle> built;
  for (int i = 0; i < 10; ++i)
    built.push_back(scene.box({.35f, 3, .18f * (i + 1)}, {2 + i * .35f, 0, 0}));
  built.push_back(scene.box({5, 5, .2f}, {7.825f, 0, 1.6f}));
  const auto builder = scene.character({-2, 0, 0});
  bi::NavigationDiagnostics binding{scene.space, identity(scene.objects),
                                    transforms(scene.objects)};
  react::DeferredEffects effects;
  const auto call = input(builder);
  bi::navigation_birth(binding, built, call, effects);
  const auto pending = sp::install_ready_navigation(scene.space);
  require(!pending.pending.empty(), "fixture did not have unfinished navigation tiles");
  require(bi::publish_navigation(binding, pending, sp::Deadline::max()).empty() &&
              binding.pending.size() == 1,
          "unfinished tiles published or discarded a diagnostic");
  scene.wait_navigation();
  const auto ready = sp::install_ready_navigation(scene.space);
  require(bi::publish_navigation(binding, ready, std::chrono::steady_clock::now()).empty() &&
              binding.pending.size() == 1,
          "expired diagnostic deadline lost pending work");
  auto notices = await_notice(scene, binding);
  require(notices.size() == 1 && notices[0].author == 7 &&
              notices[0].event.at("status") == "connected" &&
              notices[0].event.at("reference_navigable") == true &&
              !notices[0].event.at("areas").empty(),
          "actual staircase connectivity was not delivered to its builder");
  require(binding.pending.empty() && effects.ordered.empty(),
          "diagnostic changed world facts or kept completed work");
  const auto previous_revision = notices[0].event.at("revision").get<std::uint64_t>();
  const auto wall = scene.box({.2f, 20, 4}, {6, 0, 0});
  bi::navigation_birth(binding, std::span{&wall, 1}, call, effects);
  notices = await_notice(scene, binding);
  require(notices[0].event.at("status") == "unreachable" &&
              notices[0].event.at("revision").get<std::uint64_t>() > previous_revision,
          "new real wall did not report disconnected walkable regions");
  bool disconnected = false;
  for (const auto &area : notices[0].event.at("areas"))
    disconnected |= !area.at("reachable").get<bool>();
  require(disconnected, "diagnostic lacked an actual unreachable polygon point");
  std::vector<sp::RelationChange> changes;
  sp::remove(scene.space, identity(scene.objects), wall, parents(scene.objects),
             space_flags(scene.objects), scene.relations, changes);
  destroy(scene.objects, wall);
  lifecycle::Rebinding rebound{built.back(),
                               {},
                               {},
                               scene.objects.position[built.back().slot()],
                               scene.objects.rotation[built.back().slot()]};
  bi::navigation_rebind(binding, std::span{&rebound, 1}, call, effects);
  notices = await_notice(scene, binding);
  require(notices[0].event.at("status") == "connected",
          "removed wall left stale component labels after rebind");
  require(bi::publish_navigation(binding, sp::install_ready_navigation(scene.space),
                                 sp::Deadline::max())
              .empty(),
          "completed diagnostic was published twice");
}
void small_shapes_missing_references_and_stale_handles() {
  Scene scene;
  scene.box({20, 20, .2f}, {0, 0, -.2f});
  const auto body = scene.character({0, 0, 0});
  auto tiny = scene.box({.1f, .1f, .1f}, {2, 0, 4}, sp::Motion::Kinematic, 0, false);
  bi::NavigationDiagnostics binding{scene.space, identity(scene.objects),
                                    transforms(scene.objects)};
  react::DeferredEffects effects;
  bi::navigation_birth(binding, std::span{&tiny, 1}, input(body), effects);
  auto notices = await_notice(scene, binding);
  require(notices[0].event.at("status") == "no_walkable_area" &&
              notices[0].event.at("areas").empty(),
          "tiny nonwalkable shape was guessed to be an unreachable area");
  const auto stranded = scene.character({50, 0, 0});
  const auto island = scene.box({4, 4, .2f}, {0, 0, 3});
  bi::navigation_birth(binding, std::span{&island, 1}, input(stranded, 9), effects);
  notices = await_notice(scene, binding);
  require(notices[0].author == 9 && notices[0].event.at("status") == "no_reference_surface" &&
              !notices[0].event.at("reference_navigable").get<bool>(),
          "missing reference surface was reported as a proven unreachable path");
  bi::navigation_birth(binding, std::span{&island, 1}, input(body), effects);
  require(sp::set_pose(scene.space, identity(scene.objects), transforms(scene.objects), island,
                       {{30, 0, 3}, {}})
                  .status == sp::Status::Ok,
          "pending platform move failed");
  notices = await_notice(scene, binding);
  require(notices[0].event.at("status") == "unreachable" &&
              notices[0].event.at("areas").at(0).at("point").at(0).get<float>() > 25,
          "diagnostic queried stale bounds after another move during tile rebuilding");
  bi::navigation_birth(binding, std::span{&tiny, 1}, input(body), effects);
  std::vector<sp::RelationChange> changes;
  sp::remove(scene.space, identity(scene.objects), tiny, parents(scene.objects),
             space_flags(scene.objects), scene.relations, changes);
  destroy(scene.objects, tiny);
  require(bi::publish_navigation(binding, sp::install_ready_navigation(scene.space),
                                 sp::Deadline::max())
                  .empty() &&
              binding.pending.empty(),
          "retired object kept a diagnostic targeting a stale generation");
  auto autonomous = input(body);
  autonomous.action_caller = Caller(ScriptHolder{3, island});
  bi::navigation_birth(binding, std::span{&island, 1}, autonomous, effects);
  require(binding.pending.empty(), "autonomous script borrowed a player's notification identity");
}
void benchmark_diagnostics() {
  Scene scene;
  const auto floor = scene.box({80, 80, .2f}, {0, 0, -.2f});
  std::vector<sp::Bounds> regions{*sp::bounds(scene.space, identity(scene.objects), floor)};
  for (int i = 0; i < 32; ++i) {
    const auto island = scene.box({2, 2, .2f}, {float(i % 8 * 6 - 21), float(i / 8 * 6 - 9), 4});
    regions.push_back(*sp::bounds(scene.space, identity(scene.objects), island));
  }
  scene.wait_navigation();
  const auto started = std::chrono::steady_clock::now();
  const auto cold = sp::diagnose_navigation(scene.space, regions, {}, sp::Deadline::max());
  const auto cold_us =
      std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
  require(cold.complete && cold.reference_navigable && cold.areas.size() == 33,
          "benchmark fixture did not contain its actual 33 components");
  std::vector<double> samples;
  for (int i = 0; i < 200; ++i) {
    const auto start = std::chrono::steady_clock::now();
    const auto report = sp::diagnose_navigation(scene.space, regions, {}, sp::Deadline::max());
    samples.push_back(
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
            .count());
    require(report.complete && report.areas.size() == 33, "cached component result changed");
  }
  std::sort(samples.begin(), samples.end());
  std::cout << "navigation diagnostics actual 80x80m/32 islands cold_us=" << cold_us
            << " cached_us p50=" << samples[samples.size() / 2]
            << " p95=" << samples[samples.size() * 95 / 100] << " max=" << samples.back() << '\n';
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--bench") {
      benchmark_diagnostics();
      return 0;
    }
    actual_stairs_rebuild_and_delivery();
    small_shapes_missing_references_and_stale_handles();
    std::cout << "Navigation completion diagnostics checks passed.\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
