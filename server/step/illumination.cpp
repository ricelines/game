#include "step.hpp"

#include <algorithm>

namespace amber::step {
namespace {
void dirty_light(observe::LightSubscriptions &points, observe::ActiveViews &views,
                 const observe::Lights &lights, Handle source) {
  const auto found = lights.rows.find(source.bits);
  if (found == lights.rows.end())
    return;
  const auto &light = found->second;
  observe::dirty_light(points, light.position, light.binding.radius_m);
  const Vec3 radius{light.binding.radius_m, light.binding.radius_m, light.binding.radius_m};
  const space::Bounds bounds{light.position - radius, light.position + radius};
  observe::dirty_views(views, std::span{&bounds, 1});
}
void dirty_zone(observe::LightSubscriptions &points, observe::ActiveViews &views,
                const space::Space &world, Handle zone) {
  if (const auto bounds = space::volume_bounds(world, zone, space::VolumeKind::Zone)) {
    observe::dirty_points(points, std::span{&*bounds, 1});
    observe::dirty_views(views, std::span{&*bounds, 1});
  }
}
const react::Process *process(const react::Processes &processes, Handle object, TermId term) {
  return term < processes.size() ? processes[term].get(object.slot()) : nullptr;
}
bool advances(const react::Process *p, Step now, double hz) {
  if (!p || p->rate == 0)
    return false;
  const auto value = react::process_value(*p, now, hz);
  return p->rate > 0 ? value < p->hi : value > p->lo;
}
double sampled(const react::ValueRead &values, Handle object, TermId term, Step now, double hz) {
  const auto answer = react::read(values, object, term, now, hz);
  return answer.present && answer.value.is_numeric() ? answer.value.numeric() : 0;
}
void light_changed(IlluminationSources &sources, observe::Lights &lights,
                   observe::LightSubscriptions &points, observe::ActiveViews &views,
                   const react::ValueRead &values, Handle object, TermId term, Step now,
                   double hz) {
  auto found = lights.rows.find(object.bits);
  if (found == lights.rows.end()) {
    sources.running_lights.erase(object.bits);
    return;
  }
  const auto value = sampled(values, object, term, now, hz);
  if (found->second.binding.intensity_cd != value) {
    found->second.binding.intensity_cd = value;
    dirty_light(points, views, lights, object);
  }
  if (advances(process(values.processes, object, term), now, hz))
    sources.running_lights.insert(object.bits);
  else
    sources.running_lights.erase(object.bits);
}
void zone_changed(IlluminationSources &sources, observe::ZoneAmbients &zones,
                  observe::LightSubscriptions &points, observe::ActiveViews &views,
                  const space::Space &world, const react::ValueRead &values, Handle object,
                  TermId term, Step now, double hz) {
  auto *zone = zones.rows.get(object.slot());
  if (!zone || zone->zone != object) {
    sources.running_zones.erase(object.bits);
    return;
  }
  const auto value = sampled(values, object, term, now, hz);
  if (zone->ambient_lux != value) {
    zone->ambient_lux = value;
    dirty_zone(points, views, world, object);
  }
  if (advances(process(values.processes, object, term), now, hz))
    sources.running_zones.insert(object.bits);
  else
    sources.running_zones.erase(object.bits);
}
} // namespace
void register_illumination(IlluminationSources &sources, observe::Lights &lights,
                           observe::ZoneAmbients &zones, observe::LightSubscriptions &points,
                           observe::ActiveViews &views, const space::Space &world,
                           TransformRead transforms, const catalog::Models &models,
                           const catalog::ParameterBlocks &parameters,
                           const catalog::Assets &assets, const react::ValueRead &values,
                           std::span<const Handle> objects, float source_radius, Step now,
                           double hz) {
  for (const auto object : objects) {
    const auto &asset = assets.rows.at(catalog::resolve_asset(models, parameters,
                                                              values.identity.model[object.slot()],
                                                              values.params[object.slot()])
                                           .hash);
    dirty_light(points, views, lights, object);
    sources.lights.erase(object.bits);
    sources.running_lights.erase(object.bits);
    if (const auto role = net::light_binding(asset, values.terms, object, source_radius)) {
      const auto value = net::binding_value(role->intensity, object, values, now, hz);
      observe::set_light(lights,
                         {object, role->local_site, value, role->radius_m, role->source_radius_m},
                         transforms);
      if (const auto *term = std::get_if<TermRef>(&role->intensity)) {
        sources.lights.emplace(object.bits, term->id);
        if (advances(process(values.processes, object, term->id), now, hz))
          sources.running_lights.insert(object.bits);
      }
    } else
      observe::remove_light(lights, object);
    dirty_light(points, views, lights, object);
    dirty_zone(points, views, world, object);
    sources.zones.erase(object.bits);
    sources.running_zones.erase(object.bits);
    if (const auto role = net::zone_binding(asset, values.terms)) {
      observe::set_zone(zones, {object, net::binding_value(role->ambient, object, values, now, hz),
                                role->priority});
      if (const auto *term = std::get_if<TermRef>(&role->ambient)) {
        sources.zones.emplace(object.bits, term->id);
        if (advances(process(values.processes, object, term->id), now, hz))
          sources.running_zones.insert(object.bits);
      }
    } else
      observe::remove_zone(zones, object);
  }
}
void move_illumination(observe::Lights &lights, observe::LightSubscriptions &points,
                       observe::ActiveViews &views, TransformRead transforms,
                       std::span<const Handle> objects) {
  for (auto object : objects) {
    dirty_light(points, views, lights, object);
    observe::move_light(lights, object, transforms);
    dirty_light(points, views, lights, object);
  }
}
void retire_illumination(IlluminationSources &sources, observe::Lights &lights,
                         observe::ZoneAmbients &zones, observe::LightSubscriptions &points,
                         observe::ActiveViews &views, const space::Space &world,
                         std::span<const Handle> objects) {
  for (auto object : objects) {
    dirty_light(points, views, lights, object);
    dirty_zone(points, views, world, object);
    observe::remove_light(lights, object);
    observe::remove_zone(zones, object);
    sources.lights.erase(object.bits);
    sources.zones.erase(object.bits);
    sources.running_lights.erase(object.bits);
    sources.running_zones.erase(object.bits);
  }
}
void illumination_values_changed(IlluminationSources &sources, observe::Lights &lights,
                                 observe::ZoneAmbients &zones, observe::LightSubscriptions &points,
                                 observe::ActiveViews &views, const space::Space &world,
                                 const react::ValueRead &values, std::span<const FactKey> facts,
                                 Step now, double hz) {
  for (const auto fact : facts) {
    if (const auto found = sources.lights.find(fact.subject.bits);
        found != sources.lights.end() && found->second == fact.term)
      light_changed(sources, lights, points, views, values, fact.subject, fact.term, now, hz);
    if (const auto found = sources.zones.find(fact.subject.bits);
        found != sources.zones.end() && found->second == fact.term)
      zone_changed(sources, zones, points, views, world, values, fact.subject, fact.term, now, hz);
  }
}
void advance_illumination(IlluminationSources &sources, observe::Lights &lights,
                          observe::ZoneAmbients &zones, observe::LightSubscriptions &points,
                          observe::ActiveViews &views, const space::Space &world,
                          const react::ValueRead &values, Step now, double hz) {
  // Update only sources with a running process; dormant geometry costs no work.
  std::erase_if(sources.running_lights, [&](std::uint64_t bits) {
    const auto term = sources.lights.find(bits);
    const auto row = lights.rows.find(bits);
    if (term == sources.lights.end() || row == lights.rows.end())
      return true;
    const auto value = sampled(values, Handle{bits}, term->second, now, hz);
    if (row->second.binding.intensity_cd != value) {
      row->second.binding.intensity_cd = value;
      dirty_light(points, views, lights, Handle{bits});
    }
    return !advances(process(values.processes, Handle{bits}, term->second), now, hz);
  });
  std::erase_if(sources.running_zones, [&](std::uint64_t bits) {
    const auto term = sources.zones.find(bits);
    auto *row = zones.rows.get(Handle{bits}.slot());
    if (term == sources.zones.end() || !row || row->zone.bits != bits)
      return true;
    const auto value = sampled(values, Handle{bits}, term->second, now, hz);
    if (row->ambient_lux != value) {
      row->ambient_lux = value;
      dirty_zone(points, views, world, Handle{bits});
    }
    return !advances(process(values.processes, Handle{bits}, term->second), now, hz);
  });
}
void sync_illuminance_reads(observe::LightSubscriptions &points,
                            const react::Subscriptions &subscriptions, IdentityRead identity,
                            TransformRead transforms, TermId illuminance,
                            std::span<const Handle> consumers) {
  std::vector<Handle> obsolete;
  for (const auto &[key, point] : points.rows) {
    (void)key;
    const auto found = subscriptions.rows.find({point.subject, illuminance});
    if (!identity.valid(point.subject) || found == subscriptions.rows.end() ||
        found->second.empty())
      obsolete.push_back(point.subject);
  }
  for (auto object : obsolete)
    observe::unsubscribe(points, object, illuminance);
  for (auto object : consumers) {
    if (identity.valid(object))
      if (const auto source = subscriptions.rows.find({object, illuminance});
          source != subscriptions.rows.end() && !source->second.empty() &&
          !points.rows.contains({object.bits, illuminance}))
        observe::subscribe(points, {object, transforms.position[object.slot()], illuminance});
  }
}
void move_illuminance_reads(observe::LightSubscriptions &points, TransformRead transforms,
                            TermId illuminance, std::span<const Handle> moved) {
  for (auto object : moved)
    if (points.rows.contains({object.bits, illuminance}))
      observe::subscribe(points, {object, transforms.position[object.slot()], illuminance});
}
void retire_illuminance_reads(observe::LightSubscriptions &points,
                              react::RuntimeMeasurements &measurements, TermId illuminance,
                              std::span<const Handle> objects) {
  for (auto object : objects) {
    observe::unsubscribe(points, object, illuminance);
    measurements.values.erase({object, illuminance});
  }
}
void sample_illuminance(observe::LightSubscriptions &points, const space::Space &world,
                        IdentityRead identity, const observe::Lights &lights,
                        const observe::ZoneAmbients &ambients,
                        react::RuntimeMeasurements &measurements, double world_lux,
                        react::DeferredEffects &effects) {
  for (const auto &point : observe::take_dirty(points)) {
    if (!identity.valid(point.subject))
      continue;
    std::vector<Handle> zones;
    space::volumes_at(world, identity, point.position, space::VolumeKind::Zone, zones);
    std::vector<space::LightSample> samples;
    observe::light_samples(lights, identity, point.position, samples);
    const auto answer =
        observe::illuminate(world, identity, point.subject, point.position,
                            observe::ambient_at(ambients, zones, world_lux), samples);
    react::apply_derived_fact(measurements,
                              {{point.subject, point.measured_term}, Value(answer), {}}, effects);
  }
}
void update_frame_illumination(IlluminationSources &sources, observe::Lights &lights,
                               observe::ZoneAmbients &zones, observe::LightSubscriptions &points,
                               observe::ActiveViews &views, const space::Space &world,
                               TransformRead transforms, const react::ValueRead &values,
                               const react::Subscriptions &subscriptions,
                               react::RuntimeMeasurements &measurements,
                               std::span<const Handle> moved,
                               std::span<const space::Bounds> dirty_bounds,
                               std::span<const FactKey> changed, TermId illuminance, Step now,
                               double hz, react::DeferredEffects &effects) {
  move_illumination(lights, points, views, transforms, moved);
  move_illuminance_reads(points, transforms, illuminance, moved);
  observe::dirty_views(views, dirty_bounds);
  observe::dirty_geometry(points, lights, dirty_bounds, &views);
  std::vector<Handle> changed_sources;
  changed_sources.reserve(changed.size());
  for (const auto fact : changed)
    changed_sources.push_back(fact.subject);
  sync_illuminance_reads(points, subscriptions, values.identity, transforms, illuminance,
                         changed_sources);
  illumination_values_changed(sources, lights, zones, points, views, world, values, changed, now,
                              hz);
  advance_illumination(sources, lights, zones, points, views, world, values, now, hz);
  sample_illuminance(points, world, values.identity, lights, zones, measurements, 0, effects);
}
} // namespace amber::step
