#include "space_fixture.hpp"
#include "step/run_private.hpp"

using namespace amber;
namespace {
using testspace::require;
void entry_authority_is_sampled_for_each_plan() {
  testspace::Scene scene;
  scene.box({24, 12, .2f}, {0, 0, -.2f});
  const auto body = scene.character({-5, 0, 0});
  scene.objects.owner[body.slot()] = 2;
  scene.wait_navigation();
  for (int i = 0; i < 4; ++i)
    scene.tick();
  perm::State permissions;
  permissions.players.rows.resize(4);
  permissions.plots.rows.push_back({1, 1, 0, 0, {-1, -2, 1, 2}, {}});
  step::State state;
  state.clock = {.step = scene.clock, .hz = 20};
  const auto holder = perm::Holder::player(2);
  const auto scope = perm::Scope::plot(1);
  const auto enter = perm::Rights{1} << static_cast<unsigned>(perm::LandVerb::Enter);
  const auto grant = perm::create_grant(
      permissions.grants, permissions.players, perm::Holder::player(1),
      {1, holder, scope, enter, 0, 0, state.clock.step + 10},
      {scope, 1, perm::all_rights, 1, false}, {holder, 2, true}, state.clock.step);
  require(bool(grant), "ordinary owner grants entry to the walker");
  auto tick = [&] {
    ++state.clock.step;
    space::install_ready_navigation(scene.space);
    auto result = step::advance_motion(scene.space, scene.objects_state, scene.relations, state,
                                       permissions, state.clock, .15f, 1.5f);
    step::finish_motion(state, result.intents);
    return result;
  };
  step::start_move(state.intents, body, Vec3{5, 0, 0}, {});
  for (int i = 0; i < 160 && !state.intents.moving.empty(); ++i)
    tick();
  require(state.intents.moving.empty() &&
              state.intent_endings.back().reason == step::IntentEnd::Arrived &&
              scene.objects.position[body.slot()].x > 4.7f,
          "an entry grant expiring during a walk takes effect at the next plan, not every step");
  const auto before = scene.objects.position[body.slot()];
  step::start_move(state.intents, body, Vec3{-5, 0, 0}, {});
  auto blocked = tick();
  require(blocked.intents.size() == 1 &&
              blocked.intents[0].path_status == space::PathStatus::AccessDenied &&
              scene.objects.position[body.slot()].x >= before.x - .02f,
          "the return plan rejects the now-expired entry grant before crossing the closed plot");

  permissions.plots.rows[1].preset = enter;
  step::start_move(state.intents, body, Vec3{-5, 0, 0}, {});
  require(tick().intents[0].path_status == space::PathStatus::Ready,
          "a new plan uses public entry permissions");
  permissions.plots.rows[1].banned = {2};
  step::start_move(state.intents, body, Vec3{-5, 0, 0}, {});
  require(tick().intents[0].path_status == space::PathStatus::AccessDenied,
          "a new plan applies a ban despite public entry");
  scene.objects.owner[body.slot()] = 1;
  step::start_move(state.intents, body, Vec3{-5, 0, 0}, {});
  require(tick().intents[0].path_status == space::PathStatus::Ready,
          "a new plan uses the body's current owner");
}
} // namespace
int main() {
  try {
    entry_authority_is_sampled_for_each_plan();
    std::cout << "Movement plans use current land authority without per-step grant scans.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
