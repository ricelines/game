#include "settings.hpp"
#include "space_fixture.hpp"
#include "step/start_private.hpp"
#include <iostream>
using namespace amber;
using testspace::require;
namespace {
void layout_and_pool() {
  const auto settings = read_settings(AMBER_CONFIG_PATH);
  perm::State permissions;
  space::State world(settings.space, settings.light_cell_m);
  auto land = settings.land;
  land.way_group = 1;
  step::fill_pool(permissions, world, land);
  const auto &plots = permissions.plots.rows;
  require(plots.size() == 12, "one forage block creates three ways and eight lots");
  require(permissions.plots.by_owner.at(0) ==
              std::vector<PlotId>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
          "surveyed plots are owned by root");
  require(plots[1].kind == perm::PlotKind::Street &&
              plots[1].bounds == std::array<float, 4>{0, -50, 72, 50},
          "Street piece bounds");
  require(plots[2].kind == perm::PlotKind::Alley &&
              plots[2].bounds == std::array<float, 4>{0, 50, 8, 122},
          "alley bounds");
  require(plots[3].bounds == std::array<float, 4>{8, 114, 72, 122}, "back way bounds");
  require(plots[4].bounds == std::array<float, 4>{8, 50, 24, 82} &&
              plots[11].bounds == std::array<float, 4>{56, 82, 72, 114},
          "two rows of lots");
  require(plots[1].owner == 0 && plots[1].group == 1 && plots[1].preset == (6 | (1 << 8)) &&
              plots[4].preset == 2,
          "ways and lots have their specified rights");
  require(world.terrain == std::set<std::pair<std::int32_t, std::int32_t>>{{0, -1}, {0, 0}},
          "terrain covers every plot");
  const auto first =
      perm::plot_record(permissions.plots, permissions.grants, 4, land.port_spacing_m, 0);
  require(first["fronts"] == "street" && first["area_m2"] == 512 && first["port"] == 0,
          "lot record measures frontage, area and nearest port");
  for (PlotId id = 4; id <= 11; ++id)
    require(perm::plot_record(permissions.plots, permissions.grants, id, land.port_spacing_m,
                              0)["fronts"] == (id <= 7 ? "street" : "back way"),
            "every surveyed lot fronts its row's street or back way, including alley corners");
  for (PlotId id = 4; id <= 7; ++id)
    perm::set_plot_owner(permissions.plots, id, 2);
  step::fill_pool(permissions, world, land);
  require(plots.size() == 23 && plots[12].bounds == std::array<float, 4>{-72, -50, 0, 50} &&
              plots[15].bounds == std::array<float, 4>{-64, 50, -48, 82},
          "next deficient Street pool surveys north before south and assigns ids in order");
  snap::Buffer bytes;
  snap::Output out{bytes};
  perm::write_players(out, permissions.players, permissions.ledger, permissions.plots,
                      permissions.groups);
  bytes.resize(out.adapter().writtenBytesCount());
  perm::State restored;
  snap::Input in{bytes.data(), bytes.size()};
  perm::read_players(in, restored.players, restored.ledger, restored.plots, restored.groups);
  require(restored.plots.by_owner == permissions.plots.by_owner,
          "restore reconstructs each owner's plots");
  snap::check_complete(in);
  space::State rebuilt(settings.space, settings.light_cell_m);
  for (const auto &plot : restored.plots.rows)
    if (plot.id)
      space::ensure_terrain(rebuilt, plot.bounds);
  require(rebuilt.terrain == world.terrain, "restored plots rebuild the same terrain tiles");
}
void terrain_supports_bodies_and_navigation() {
  testspace::Scene scene;
  space::add_terrain(scene.space_state, 0, 0);
  space::add_terrain(scene.space_state, 0, 0);
  const auto body = scene.character({10, 10, 1});
  for (int tick = 0; tick < 40; ++tick)
    scene.tick();
  require(std::abs(scene.objects.position[body.slot()].z) < .1f, "body stands on terrain at zero");
  react::State lighting({4096, 50, 5, 64});
  lighting.terms.by_name["base.outdoor_lux"] = 1;
  lighting.world_settings.values[1] = Value(120.0);
  require(space::illuminance(scene.space, identity(scene.objects), body, {10, 10, 1},
                             step::outdoor_lux(lighting), {}) == 120,
          "outdoor objects receive the world setting's 120 lux");
  scene.wait_navigation();
  const auto route = space::path(scene.space, identity(scene.objects), body,
                                 space::PointGoal{{20, 20, 0}, .1f}, {});
  require(route.status == space::PathStatus::Ready, "navigation crosses terrain");
  require(scene.result.contacts.empty(), "terrain has no object contact events");
}
} // namespace
int main() {
  try {
    layout_and_pool();
    terrain_supports_bodies_and_navigation();
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
