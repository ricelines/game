#include "step.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace amber::step {
void refresh_material(space::Space &space, const GeometryAssets &geometry,
                      const amber::catalog::State &catalog_state, const react::ValueRead &values,
                      PhysicsRoles roles, PhysicsDefaults defaults,
                      std::span<const TermId> materials, Step now, double hz) {
  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;
  auto &assets = catalog_state.assets;

  const auto identity = values.identity;
  for (std::uint32_t slot = 1; slot < identity.model.size(); ++slot) {
    if (!identity.model[slot] ||
        std::ranges::find(materials, identity.material[slot]) == materials.end())
      continue;
    const auto h = identity.at(slot);
    const auto asset =
        catalog::resolve_asset(models, parameters, identity.model[slot], values.params[slot]);
    const auto definition = describe_physics(
        geometry, assets.rows.at(asset.hash), values,
        {identity.model[slot], values.params[slot], identity.material[slot], values.plots[slot], h},
        identity.flags[slot], roles, defaults, now, hz);
    const auto &spec = definition.body;
    if (!spec.geometry)
      continue;
    if (!(spec.mass_kg > 0) || !std::isfinite(spec.mass_kg) ||
        !std::isfinite(spec.surface.friction) || spec.surface.friction < 0 ||
        !std::isfinite(spec.surface.restitution) || spec.surface.restitution < 0 ||
        !std::isfinite(spec.surface.transmission) || spec.surface.transmission < 0 ||
        spec.surface.transmission > 1)
      throw std::invalid_argument("material edit produces invalid native mass or surface");
    if (space::body_motion(space, identity, h)) {
      const auto result = space::set_properties(space, identity, h, {spec.mass_kg, spec.surface});
      if (result.status != space::Status::Ok)
        throw std::invalid_argument(space::status_text(result.status));
    }
  }
}
} // namespace amber::step
