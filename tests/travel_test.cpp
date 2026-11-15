#include "bindings/travel.hpp"

#include <iostream>

using namespace amber;
int main() {
  try {
    const auto require = [](bool ok, const char *why) {
      if (!ok)
        throw std::runtime_error(why);
    };
    Objects objects;
    ObjectInit init;
    init.model = 1;
    init.owner = 2;
    const auto body = create(objects, init);
    perm::Players players;
    players.rows.resize(4);
    perm::Plots plots;
    plots.rows.push_back({1, 1, 0, 0, {-3, -2, 3, 2}, {}});
    perm::Grants grants;
    bindings::TravelAccess cache;
    const auto boundaries = [&](Step now) {
      return bindings::travel_boundaries(cache, identity(objects), authority(objects), body, plots,
                                         players, grants, now);
    };
    bindings::invalidate_travel(cache, grants, 1);
    require(boundaries(1).size() == 1, "private land must supply a real movement boundary");
    const auto &region = boundaries(1).front();
    require(region.polygon.front() == Vec3{-3, -2, 0} && region.max_z > 1e6f,
            "boundaries use the authored plot geometry at all traversable elevations");
    const auto holder = perm::Holder::player(2);
    const auto scope = perm::Scope::plot(1);
    const auto enter = perm::Rights{1} << static_cast<unsigned>(perm::LandVerb::Enter);
    const auto grant = perm::create_grant(
        grants, players, perm::Holder::player(1), {1, holder, scope, enter, 0, 0, 10},
        {scope, 1, perm::all_rights, 1, false}, {holder, 2, true}, 1);
    require(bool(grant), "ordinary land entry grant must be created");
    bindings::invalidate_travel(cache, grants, 1);
    require(boundaries(9).empty(), "a current ordinary Enter grant allows movement");
    require(boundaries(10).size() == 1, "cached authority must expire at the exact step");
    plots.rows[1].preset = enter;
    bindings::invalidate_travel(cache, grants, 10);
    require(boundaries(10).empty(), "a public plot permits entry");
    plots.rows[1].banned = {2};
    bindings::invalidate_travel(cache, grants, 10);
    require(boundaries(10).size() == 1, "a ban overrides a public plot");
    objects.owner[body.slot()] = 1;
    require(boundaries(10).empty(), "a changed live owner cannot reuse another player's cache");
    const auto stale = body;
    destroy(objects, body);
    require(bindings::travel_boundaries(cache, identity(objects), authority(objects), stale, plots,
                                        players, grants, 10)
                .empty(),
            "stale generations cannot retain a movement context");
    std::cout << "Actual land authority, cached boundaries, expiry and owner changes passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
