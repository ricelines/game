#include "geometry.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace amber::bindings {
MaterialRefreshPlan prepare_material_refresh(const MaterialRefresh &b,
                                             std::span<const TermId> materials, Step now,
                                             double hz) {
  MaterialRefreshPlan plan;
  const auto identity = b.values.identity;
  for (std::uint32_t slot = 1; slot < identity.model.size(); ++slot) {
    if (!identity.model[slot] ||
        std::ranges::find(materials, identity.material[slot]) == materials.end())
      continue;
    const auto h = identity.at(slot);
    plan.affected.push_back(h);
    const auto asset =
        catalog::resolve_asset(b.models, b.parameters, identity.model[slot], b.values.params[slot]);
    const auto definition = describe_physics(b.geometry, b.assets.rows.at(asset.hash), b.values,
                                             {identity.model[slot], b.values.params[slot],
                                              identity.material[slot], b.values.plots[slot], h},
                                             identity.flags[slot], b.roles, b.defaults, now, hz);
    const auto &spec = definition.body;
    if (!spec.geometry)
      continue;
    if (!(spec.mass_kg > 0) || !std::isfinite(spec.mass_kg) ||
        !std::isfinite(spec.surface.friction) || spec.surface.friction < 0 ||
        !std::isfinite(spec.surface.restitution) || spec.surface.restitution < 0 ||
        !std::isfinite(spec.surface.transmission) || spec.surface.transmission < 0 ||
        spec.surface.transmission > 1)
      throw std::invalid_argument("material edit produces invalid native mass or surface");
    if (space::body_motion(b.space, identity, h))
      plan.physical.push_back({h, {spec.mass_kg, spec.surface}});
  }
  return plan;
}
void apply_material_refresh(space::Space &world, IdentityRead identity,
                            const MaterialRefreshPlan &plan) {
  for (const auto &update : plan.physical) {
    const auto result = space::set_properties(world, identity, update.object, update.properties);
    if (result.status != space::Status::Ok)
      throw std::logic_error("preflighted material refresh failed: " +
                             std::string(space::status_text(result.status)));
  }
}
} // namespace amber::bindings
