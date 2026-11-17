#include "step.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace amber::step {
namespace {
std::set<std::pair<int, int>> surveyed(const perm::Plots &plots, const LandSettings &s) {
  std::set<std::pair<int, int>> result;
  for (const auto &plot : plots.rows)
    if (plot.id && plot.kind != perm::PlotKind::Street) {
      const auto x = (plot.bounds[0] + plot.bounds[2]) / 2;
      const auto y = (plot.bounds[1] + plot.bounds[3]) / 2;
      const auto depth = int(std::floor((std::abs(y) - s.street_width_m / 2) / s.block_m)) + 1;
      result.emplace(int(std::floor(x / s.block_m)), y > 0 ? depth : -depth);
    }
  return result;
}
bool street_lot(const perm::Plot &p, const LandSettings &s) {
  return p.bounds[1] == s.street_width_m / 2 || p.bounds[3] == -s.street_width_m / 2;
}
} // namespace
void survey_block(perm::State &perm, space::State &space, const LandSettings &s, Block block) {
  if (!block.j)
    throw std::invalid_argument("a land block cannot lie on the Street");
  if (surveyed(perm.plots, s).contains({block.i, block.j}))
    return;
  const float x = block.i * s.block_m;
  const float y = s.street_width_m / 2 + (std::abs(block.j) - 1) * s.block_m;
  auto add = [&](perm::PlotKind kind, std::array<float, 4> bounds) {
    if (block.j < 0 && kind != perm::PlotKind::Street)
      bounds = {bounds[0], -bounds[3], bounds[2], -bounds[1]};
    const auto id = PlotId(perm.plots.rows.size());
    const bool way = kind != perm::PlotKind::Lot;
    perm::Plot plot{id, 0, way ? s.way_group : 0, way ? perm::Rights(6 | (1 << 8)) : 2, bounds, {}};
    plot.kind = kind;
    perm.plots.rows.push_back(std::move(plot));
    perm::set_plot_owner(perm.plots, id, 0);
    space::ensure_terrain(space, bounds);
  };
  const bool has_street = std::ranges::any_of(perm.plots.rows, [&](const auto &p) {
    return p.id && p.kind == perm::PlotKind::Street && p.bounds[0] == x;
  });
  if (!has_street)
    add(perm::PlotKind::Street, {x, -s.street_width_m / 2, x + s.block_m, s.street_width_m / 2});
  add(perm::PlotKind::Alley, {x, y, x + s.way_m, y + s.block_m});
  add(perm::PlotKind::Alley, {x + s.way_m, y + s.block_m - s.way_m, x + s.block_m, y + s.block_m});
  const auto depth = (s.block_m - s.way_m) / 2;
  for (int row = 0; row < 2; ++row)
    for (float front = s.way_m; front < s.block_m; front += s.lot_front_m)
      add(perm::PlotKind::Lot,
          {x + front, y + row * depth, x + front + s.lot_front_m, y + (row + 1) * depth});
}
void fill_pool(perm::State &perm, space::State &space, const LandSettings &s) {
  for (;;) {
    std::size_t street = 0, back = 0;
    for (const auto &p : perm.plots.rows)
      if (p.id && !p.owner && p.kind == perm::PlotKind::Lot)
        ++(street_lot(p, s) ? street : back);
    if (street >= s.pool_lots_min && back >= s.pool_lots_min)
      return;
    const auto blocks = surveyed(perm.plots, s);
    int extent = 1;
    for (auto [i, j] : blocks) {
      (void)j;
      extent = std::max(extent, std::abs(i) + 1);
    }
    Block best{};
    double best_distance = INFINITY;
    for (int i = -extent; i <= extent; ++i)
      for (int sign : {1, -1}) {
        int j = sign;
        while (blocks.contains({i, j}))
          j += sign;
        if (street < s.pool_lots_min && std::abs(j) != 1)
          continue;
        const double x = (i + .5) * s.block_m;
        const double y = s.street_width_m / 2 + (std::abs(j) - .5) * s.block_m;
        const auto distance = x * x + y * y;
        if (distance < best_distance || (distance == best_distance && (j > 0) > (best.j > 0)) ||
            (distance == best_distance && (j > 0) == (best.j > 0) && i > best.i)) {
          best = {i, j};
          best_distance = distance;
        }
      }
    survey_block(perm, space, s, best);
  }
}
} // namespace amber::step
