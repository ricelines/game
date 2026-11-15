#include "projection.hpp"

namespace amber::bindings {
namespace {
const catalog::Asset &asset(IdentityRead identity, const std::vector<std::uint32_t> &params,
                            const catalog::Models &models,
                            const catalog::ParameterBlocks &parameters,
                            const catalog::Assets &assets, Handle h) {
  return assets.rows.at(
      catalog::resolve_asset(models, parameters, identity.model[h.slot()], params[h.slot()]).hash);
}
void dirty_light(LightProjection &projection, Handle h) {
  if (auto found = projection.lights.rows.find(h.bits); found != projection.lights.rows.end()) {
    observe::dirty_light(projection.subscriptions, found->second.position,
                         found->second.binding.radius_m);
    if (projection.views) {
      const auto &light = found->second;
      const auto r = light.binding.radius_m;
      const Vec3 extent{r, r, r};
      const space::Bounds bounds{light.position - extent, light.position + extent};
      observe::dirty_views(*projection.views, std::span(&bounds, 1));
    }
  }
}
} // namespace
void register_light_projection(LightProjection &projection, std::span<const Handle> objects) {
  for (auto h : objects) {
    dirty_light(projection, h);
    auto binding = light_binding(asset(projection.identity, projection.params, projection.models,
                                       projection.parameters, projection.assets, h),
                                 projection.terms, h, projection.default_source_radius_m);
    if (binding)
      observe::set_light(projection.lights, *binding, projection.transforms);
    else
      observe::remove_light(projection.lights, h);
    dirty_light(projection, h);
  }
}
void move_light_projection(LightProjection &projection, std::span<const Handle> objects) {
  for (auto h : objects) {
    dirty_light(projection, h);
    observe::move_light(projection.lights, h, projection.transforms);
    dirty_light(projection, h);
  }
}
void retire_light_projection(LightProjection &projection, std::span<const Handle> objects) {
  for (auto h : objects) {
    dirty_light(projection, h);
    observe::remove_light(projection.lights, h);
  }
}
void register_zone_projection(ZoneProjection &projection, std::span<const Handle> objects) {
  for (auto h : objects) {
    auto binding = zone_binding(asset(projection.identity, projection.params, projection.models,
                                      projection.parameters, projection.assets, h),
                                projection.terms);
    if (binding)
      observe::set_zone(projection.zones, {h, binding->ambient, binding->priority});
    else
      observe::remove_zone(projection.zones, h);
  }
}
void retire_zone_projection(ZoneProjection &projection, std::span<const Handle> objects) {
  for (auto h : objects)
    observe::remove_zone(projection.zones, h);
}
void invalidate_mass_projection(MassProjection &projection, std::span<const Handle> objects) {
  invalidate_mass(projection.masses, projection.identity, projection.declared_parts, objects);
}
void retire_mass_projection(MassProjection &projection, std::span<const Handle> objects) {
  invalidate_mass_projection(projection, objects);
  for (auto h : objects)
    retire_mass(projection.masses, projection.measurements, h);
}
void register_replication_projection(ReplicationProjection &projection,
                                     std::span<const Handle> objects) {
  for (auto h : objects)
    register_replication(projection.index, h, projection.values.identity, projection.transforms,
                         projection.models, projection.parameters, projection.assets,
                         projection.values);
}
void move_replication_projection(ReplicationProjection &projection,
                                 std::span<const Handle> objects) {
  for (auto h : objects)
    moved(projection.index, h, projection.transforms);
}
void retire_replication_projection(ReplicationProjection &projection,
                                   std::span<const Handle> objects) {
  for (auto h : objects)
    retire_replication(projection.index, h);
}
} // namespace amber::bindings
