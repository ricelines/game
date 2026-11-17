#include "catalog/catalog.hpp"
#include "step.hpp"

#include <algorithm>

namespace amber::step {
double outdoor_lux(const react::State &state) {
  const auto term = state.terms.by_name.find("base.outdoor_lux");
  if (term == state.terms.by_name.end())
    return 0;
  const auto value = state.world_settings.values.find(term->second);
  return value == state.world_settings.values.end() ? 0 : value->second.numeric();
}
void invalidate_world_illumination(space::State &state) {
  for (const auto &[key, point] : state.light_subscriptions.rows) {
    (void)point;
    state.light_subscriptions.dirty.insert(key);
  }
  for (const auto &[session, view] : state.active_views.rows) {
    (void)view;
    state.active_views.dirty.insert(session);
  }
}

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
void light_changed(IlluminationSources &sources, amber::space::State &space_state,
                   const react::ValueRead &values, Handle object, TermId term, Step now,
                   double hz) {
  auto &lights = space_state.lights;
  auto &points = space_state.light_subscriptions;
  auto &views = space_state.active_views;

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
void zone_changed(IlluminationSources &sources, amber::space::State &space_state,
                  const react::ValueRead &values, Handle object, TermId term, Step now, double hz) {
  auto &world = space_state.space;

  auto &zones = space_state.zones;
  auto &points = space_state.light_subscriptions;
  auto &views = space_state.active_views;

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
void register_illumination(IlluminationSources &sources, amber::space::State &space_state,
                           TransformRead transforms, const amber::catalog::State &catalog_state,
                           const react::ValueRead &values, std::span<const Handle> objects,
                           float source_radius, const amber::step::Clock &clock) {
  auto &world = space_state.space;
  const auto now = clock.step;
  const auto hz = clock.hz;

  auto &lights = space_state.lights;
  auto &zones = space_state.zones;
  auto &points = space_state.light_subscriptions;
  auto &views = space_state.active_views;
  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;
  auto &assets = catalog_state.assets;

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
void illumination_values_changed(IlluminationSources &sources, amber::space::State &space_state,
                                 const react::ValueRead &values, std::span<const FactKey> facts,
                                 Step now, double hz) {

  for (const auto fact : facts) {
    if (const auto found = sources.lights.find(fact.subject.bits);
        found != sources.lights.end() && found->second == fact.term)
      light_changed(sources, space_state, values, fact.subject, fact.term, now, hz);
    if (const auto found = sources.zones.find(fact.subject.bits);
        found != sources.zones.end() && found->second == fact.term)
      zone_changed(sources, space_state, values, fact.subject, fact.term, now, hz);
  }
}
void advance_illumination(IlluminationSources &sources, amber::space::State &space_state,
                          const react::ValueRead &values, Step now, double hz) {
  auto &world = space_state.space;

  auto &lights = space_state.lights;
  auto &zones = space_state.zones;
  auto &points = space_state.light_subscriptions;
  auto &views = space_state.active_views;

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
void sync_illuminance_reads(observe::LightSubscriptions &points, const react::State &react_state,
                            const react::ValueRead &values, const catalog::Scripts &scripts,
                            TransformRead transforms, TermId illuminance,
                            std::span<const Handle> consumers, const Clock &clock) {
  for (auto object : consumers) {
    if (values.identity.valid(object) &&
        react::has_watch(react_state, values, scripts, {object, illuminance}, clock.step,
                         clock.hz)) {
      if (!points.rows.contains({object.bits, illuminance}))
        observe::subscribe(points, {object, transforms.position[object.slot()], illuminance});
    } else
      observe::unsubscribe(points, object, illuminance);
  }
}
void refresh_illuminance_reads(observe::LightSubscriptions &points, const react::State &react_state,
                               const react::ValueRead &values, const catalog::Scripts &scripts,
                               TransformRead transforms, TermId illuminance, const Clock &clock,
                               bool discover) {
  if (discover) {
    for (std::uint32_t slot = 1; slot < values.identity.model.size(); ++slot)
      if (values.identity.model[slot]) {
        const auto object = values.identity.at(slot);
        sync_illuminance_reads(points, react_state, values, scripts, transforms, illuminance,
                               std::span(&object, 1), clock);
      }
  } else {
    for (auto row = points.rows.begin(); row != points.rows.end();) {
      const auto point = (row++)->second;
      if (point.measured_term == illuminance)
        sync_illuminance_reads(points, react_state, values, scripts, transforms, illuminance,
                               std::span(&point.subject, 1), clock);
    }
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
react::DeferredEffects
update_frame_illumination(State &step_state, space::State &space_state, TransformRead transforms,
                          const react::ValueRead &values, react::State &react_state,
                          const catalog::Scripts &scripts, const space::StepResult &physics,
                          TermId illuminance) {
  react::DeferredEffects effects;
  const auto &moved = physics.moved;
  const auto &dirty_bounds = physics.dirty_bounds;
  auto &sources = step_state.illumination_sources;
  const auto &changed = step_state.mutated;
  const auto &clock = step_state.clock;

  auto &world = space_state.space;
  const auto now = clock.step;
  const auto hz = clock.hz;

  auto &lights = space_state.lights;
  auto &zones = space_state.zones;
  auto &points = space_state.light_subscriptions;
  auto &views = space_state.active_views;
  auto &measurements = react_state.runtime_measurements;

  move_illumination(lights, points, views, transforms, moved);
  move_illuminance_reads(points, transforms, illuminance, moved);
  observe::dirty_views(views, dirty_bounds);
  observe::dirty_geometry(points, lights, dirty_bounds, &views);
  std::vector<Handle> changed_sources;
  changed_sources.reserve(changed.size());
  for (const auto fact : changed)
    changed_sources.push_back(fact.subject);
  std::sort(changed_sources.begin(), changed_sources.end());
  changed_sources.erase(std::unique(changed_sources.begin(), changed_sources.end()),
                        changed_sources.end());
  sync_illuminance_reads(points, react_state, values, scripts, transforms, illuminance,
                         changed_sources, clock);
  illumination_values_changed(sources, space_state, values, changed, now, hz);
  advance_illumination(sources, space_state, values, now, hz);
  sample_illuminance(points, world, values.identity, lights, zones, measurements,
                     outdoor_lux(react_state), effects);
  return effects;
}
} // namespace amber::step
