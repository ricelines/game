#include "illumination.hpp"

namespace amber::bindings {
namespace {
void dirty(observe::LightSubscriptions &points, observe::ActiveViews &views,
           const observe::Lights::Row &light) {
  observe::dirty_light(points, light.position, light.binding.radius_m);
  const auto extent = Vec3{light.binding.radius_m, light.binding.radius_m, light.binding.radius_m};
  const space::Bounds bounds{light.position - extent, light.position + extent};
  observe::dirty_views(views, std::span{&bounds, 1});
}
const react::Process *process(const react::Processes &processes, Handle object, TermId term) {
  return term < processes.size() ? processes[term].get(object.slot()) : nullptr;
}
void dirty_zone(observe::LightSubscriptions &points, observe::ActiveViews &views,
                const space::Space &world, Handle zone) {
  if (const auto bounds = space::volume_bounds(world, zone, space::VolumeKind::Zone)) {
    observe::dirty_points(points, std::span{&*bounds, 1});
    observe::dirty_views(views, std::span{&*bounds, 1});
  }
}
bool process_advances(const react::Process *p, Step now, double hz) {
  if (!p || p->rate == 0)
    return false;
  const auto value = react::process_value(*p, now, hz);
  return p->rate > 0 ? value < p->hi : value > p->lo;
}
} // namespace
void sync_illuminance_reads(IlluminanceReads &b, std::span<const Handle> consumers) {
  // This is the explicit sample list, not a scan of world objects. Removing an
  // installation is rare; retaining a second dependency index is unnecessary.
  std::vector<Handle> obsolete;
  for (const auto &[key, point] : b.points.rows) {
    (void)key;
    const auto found = b.subscriptions.rows.find({point.subject, b.illuminance});
    if (!b.identity.valid(point.subject) || found == b.subscriptions.rows.end() ||
        found->second.empty())
      obsolete.push_back(point.subject);
  }
  for (auto h : obsolete)
    observe::unsubscribe(b.points, h, b.illuminance);
  for (auto h : consumers) {
    const auto found = b.subscriptions.consumers_on.find(h.bits);
    if (found == b.subscriptions.consumers_on.end())
      continue;
    for (auto consumer : found->second) {
      const auto sources = b.subscriptions.by_consumer.find(consumer);
      if (sources == b.subscriptions.by_consumer.end())
        continue;
      for (auto key : sources->second)
        if (key.term == b.illuminance && b.identity.valid(key.subject) &&
            !b.points.rows.contains({key.subject.bits, b.illuminance}))
          observe::subscribe(
              b.points, {key.subject, b.transforms.position[key.subject.slot()], b.illuminance});
    }
  }
}
void move_illuminance_reads(IlluminanceReads &b, std::span<const Handle> moved) {
  for (auto h : moved)
    if (b.points.rows.contains({h.bits, b.illuminance}))
      observe::subscribe(b.points, {h, b.transforms.position[h.slot()], b.illuminance});
}
void retire_illuminance_reads(IlluminanceReads &b, react::RuntimeMeasurements &values,
                              std::span<const Handle> objects) {
  for (auto h : objects) {
    observe::unsubscribe(b.points, h, b.illuminance);
    values.values.erase({h, b.illuminance});
  }
}
void light_value_changed(RunningLights &running, observe::Lights &lights,
                         observe::LightSubscriptions &points, observe::ActiveViews &views,
                         const react::Processes &processes, FactKey key) {
  const auto found = lights.rows.find(key.subject.bits);
  if (found == lights.rows.end()) {
    running.sources.erase(key.subject.bits);
    return;
  }
  const auto *term = std::get_if<TermRef>(&found->second.binding.intensity);
  if (!term || term->id != key.term)
    return;
  dirty(points, views, found->second);
  const auto *p = process(processes, key.subject, key.term);
  if (p && p->rate != 0)
    running.sources.insert(key.subject.bits);
  else
    running.sources.erase(key.subject.bits);
}
void advance_running_lights(RunningLights &running, const observe::Lights &lights,
                            observe::LightSubscriptions &points, observe::ActiveViews &views,
                            const react::Processes &processes, Step now, double hz) {
  std::erase_if(running.sources, [&](std::uint64_t bits) {
    const auto found = lights.rows.find(bits);
    if (found == lights.rows.end())
      return true;
    const auto *term = std::get_if<TermRef>(&found->second.binding.intensity);
    if (!term)
      return true;
    const auto *p = process(processes, Handle{bits}, term->id);
    if (!p || p->rate == 0)
      return true;
    dirty(points, views, found->second);
    return !process_advances(p, now, hz);
  });
}
void register_illumination_processes(IlluminationProcesses &b, std::span<const Handle> objects) {
  for (auto h : objects) {
    const auto light = b.light_bindings.rows.find(h.bits);
    const auto *intensity = light == b.light_bindings.rows.end()
                                ? nullptr
                                : std::get_if<TermRef>(&light->second.binding.intensity);
    const auto *light_process = intensity ? process(b.processes, h, intensity->id) : nullptr;
    if (light_process && light_process->rate != 0)
      b.lights.sources.insert(h.bits);
    else
      b.lights.sources.erase(h.bits);
    const auto *zone = b.zone_bindings.rows.get(h.slot());
    const auto *ambient = zone && zone->zone == h ? std::get_if<TermRef>(&zone->ambient) : nullptr;
    const auto *zone_process = ambient ? process(b.processes, h, ambient->id) : nullptr;
    if (zone_process && zone_process->rate != 0)
      b.zones.sources.insert(h.bits);
    else
      b.zones.sources.erase(h.bits);
  }
}
void zone_value_changed(RunningZones &running, const observe::ZoneAmbients &zones,
                        observe::LightSubscriptions &points, observe::ActiveViews &views,
                        const space::Space &world, const react::Processes &processes, FactKey key) {
  const auto *zone = zones.rows.get(key.subject.slot());
  if (!zone || zone->zone != key.subject) {
    running.sources.erase(key.subject.bits);
    return;
  }
  const auto *term = std::get_if<TermRef>(&zone->ambient);
  if (!term || term->id != key.term)
    return;
  dirty_zone(points, views, world, key.subject);
  const auto *p = process(processes, key.subject, key.term);
  if (p && p->rate != 0)
    running.sources.insert(key.subject.bits);
  else
    running.sources.erase(key.subject.bits);
}
void advance_running_zones(RunningZones &running_sources, const observe::ZoneAmbients &zones,
                           observe::LightSubscriptions &points, observe::ActiveViews &views,
                           const space::Space &world, const react::Processes &processes, Step now,
                           double hz) {
  std::erase_if(running_sources.sources, [&](std::uint64_t bits) {
    const Handle h{bits};
    const auto *zone = zones.rows.get(h.slot());
    if (!zone || zone->zone != h)
      return true;
    const auto *term = std::get_if<TermRef>(&zone->ambient);
    if (!term)
      return true;
    const auto *p = process(processes, h, term->id);
    if (!p || p->rate == 0)
      return true;
    dirty_zone(points, views, world, h);
    return !process_advances(p, now, hz);
  });
}
void sample_illuminance(IlluminanceSampling &b, Step now, double hz,
                        react::DeferredEffects &effects) {
  for (const auto &point : observe::take_dirty(b.points)) {
    if (!b.values.identity.valid(point.subject))
      continue;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double, std::milli>(b.query_ms));
    std::vector<Handle> zones;
    if (!space::volumes_at(b.space, b.values.identity, point.position, space::VolumeKind::Zone,
                           deadline, zones)) {
      b.points.dirty.insert({point.subject.bits, point.measured_term});
      continue;
    }
    std::vector<space::LightSample> lights;
    observe::light_samples(b.lights, b.values.identity, b.values, now, hz, point.position, lights);
    const auto ambient =
        observe::ambient_at(b.zones, b.values, zones, now, hz, b.world_ambient_lux);
    const auto answer = observe::illuminate(b.space, b.values.identity, point.subject,
                                            point.position, ambient, lights, deadline);
    if (!answer.complete) {
      b.points.dirty.insert({point.subject.bits, point.measured_term});
      continue;
    }
    react::apply_derived_fact(b.measurements,
                              {{point.subject, point.measured_term}, Value(answer.lux), now, {}},
                              effects);
  }
}
} // namespace amber::bindings
