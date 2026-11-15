#include "step/step.hpp"

namespace amber::step {
void invalidate_travel(TravelAccess &cache, const perm::Grants &grants, Step now) {
  cache.bodies.clear();
  cache.next_expiry = 0;
  for (const auto &grant : grants.rows)
    if (grant.live && grant.expires_step > now &&
        (!cache.next_expiry || grant.expires_step < cache.next_expiry))
      cache.next_expiry = grant.expires_step;
}
std::span<const space::TravelBoundary> travel_boundaries(TravelAccess &cache, IdentityRead identity,
                                                         AuthorityRead authority, Handle body,
                                                         const perm::Plots &plots,
                                                         const perm::Players &players,
                                                         const perm::Grants &grants, Step now) {
  if (cache.next_expiry && now >= cache.next_expiry)
    invalidate_travel(cache, grants, now);
  if (!identity.valid(body))
    return {};
  const auto player = authority.owner[body.slot()];
  if (auto found = cache.bodies.find(body.bits); found != cache.bodies.end()) {
    if (found->second.player == player)
      return found->second.forbidden;
    cache.bodies.erase(found);
  }
  TravelAccess::Row row{player, {}};
  for (const auto &plot : plots.rows)
    if (plot.id && !perm::check_land(perm::Holder::player(player), perm::LandVerb::Enter, plot,
                                     players, grants, now)) {
      const auto [x0, y0, x1, y1] = plot.bounds;
      row.forbidden.push_back({{{x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}},
                               std::numeric_limits<float>::lowest(),
                               std::numeric_limits<float>::max()});
    }
  return cache.bodies.emplace(body.bits, std::move(row)).first->second.forbidden;
}
} // namespace amber::step
