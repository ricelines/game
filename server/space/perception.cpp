#include "space.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <unordered_set>

namespace amber::observe {
namespace {
Cell cell(Vec3 p, float side) {
  return {std::int32_t(std::floor(p.x / side)), std::int32_t(std::floor(p.y / side))};
}
template <class F> void cells(Vec3 p, float radius, float side, F &&f) {
  auto a = cell({p.x - radius, p.y - radius, 0}, side);
  auto b = cell({p.x + radius, p.y + radius, 0}, side);
  for (auto x = a[0]; x <= b[0]; ++x)
    for (auto y = a[1]; y <= b[1]; ++y)
      f(Cell{x, y});
}
float distance(Vec3 point, space::Bounds b) {
  return (point - Vec3{std::clamp(point.x, b.min.x, b.max.x), std::clamp(point.y, b.min.y, b.max.y),
                       std::clamp(point.z, b.min.z, b.max.z)})
      .length();
}
float bearing(Vec3 forward, Vec3 delta) {
  return std::atan2(forward.y * delta.x - forward.x * delta.y,
                    forward.x * delta.x + forward.y * delta.y) *
         180 / std::numbers::pi_v<float>;
}
Handle logical_root(const space::Space &world, IdentityRead identity, const Relation &parts,
                    Handle object) {
  // Only declared parts collapse, and only into a root without its own collider.
  for (;;) {
    auto *parents = parts.in.get(object.slot());
    if (!parents || parents->empty())
      return object;
    Handle parent = parents->front();
    if (!identity.valid(parent) || !space::is_logical_root(world, identity, parent))
      return object;
    object = parent;
  }
}
void descendants(const Relation &parts, Handle root, std::vector<Handle> &out) {
  out.push_back(root);
  for (std::size_t i = 0; i < out.size(); ++i)
    if (auto *children = parts.out.get(out[i].slot()))
      out.insert(out.end(), children->begin(), children->end());
}
} // namespace
Lights::Lights(float side) : cell_m(side) {
  if (!(side > 0))
    throw std::invalid_argument("light cell size must be positive");
}
void remove_light(Lights &lights, Handle h) {
  auto it = lights.rows.find(h.bits);
  if (it == lights.rows.end())
    return;
  for (auto key : it->second.cells) {
    auto c = lights.candidates.find(key);
    std::erase(c->second, h);
    if (c->second.empty())
      lights.candidates.erase(c);
  }
  lights.rows.erase(it);
}
void set_light(Lights &lights, const LightBinding &binding, TransformRead transforms) {
  if (!(std::isfinite(binding.radius_m) && std::isfinite(binding.source_radius_m) &&
        binding.radius_m >= 0 && binding.source_radius_m > 0))
    throw std::invalid_argument("light radii must be nonnegative and source radius positive");
  remove_light(lights, binding.source);
  auto position = space::transform_point(
      {transforms.position[binding.source.slot()], transforms.rotation[binding.source.slot()]},
      binding.local_site);
  Lights::Row row{binding, position, {}};
  cells(position, binding.radius_m, lights.cell_m, [&](Cell c) {
    row.cells.push_back(c);
    lights.candidates[c].push_back(binding.source);
  });
  lights.rows.emplace(binding.source.bits, std::move(row));
}
void move_light(Lights &lights, Handle source, TransformRead transforms) {
  auto it = lights.rows.find(source.bits);
  if (it != lights.rows.end()) {
    auto binding = it->second.binding;
    set_light(lights, binding, transforms);
  }
}
void set_zone(ZoneAmbients &zones, ZoneAmbient zone) {
  zones.rows.set(zone.zone.slot(), zone);
}
void remove_zone(ZoneAmbients &zones, Handle h) {
  zones.rows.erase(h.slot());
}
void light_samples(const Lights &lights, IdentityRead identity, Vec3 point,
                   std::vector<space::LightSample> &out) {
  out.clear();
  auto c = lights.candidates.find(cell(point, lights.cell_m));
  if (c == lights.candidates.end())
    return;
  for (auto h : c->second) {
    if (!identity.valid(h))
      continue;
    const auto &row = lights.rows.at(h.bits);
    const auto &b = row.binding;
    if ((point - row.position).length_squared() > b.radius_m * b.radius_m)
      continue;
    const auto intensity = b.intensity_cd;
    if (intensity > 0)
      out.push_back({h, row.position, intensity, b.radius_m, b.source_radius_m});
  }
}
double ambient_at(const ZoneAmbients &zones, std::span<const Handle> containing, double world) {
  const ZoneAmbient *best = nullptr;
  for (auto h : containing)
    if (auto *z = zones.rows.get(h.slot()); z && z->zone == h &&
                                            (!best || z->priority > best->priority ||
                                             (z->priority == best->priority && h < best->zone)))
      best = z;
  return best ? best->ambient_lux : world;
}
double illuminate(const space::Space &world, IdentityRead identity, Handle receiver, Vec3 p,
                  double ambient, std::span<const space::LightSample> samples) {
  return space::illuminance(world, identity, receiver, p, ambient, samples);
}
LightSubscriptions::LightSubscriptions(float side) : cell_m(side) {
  if (!(side > 0))
    throw std::invalid_argument("light cell size must be positive");
}
void unsubscribe(LightSubscriptions &s, Handle h, TermId term) {
  auto key = std::pair{h.bits, term};
  auto it = s.rows.find(key);
  if (it == s.rows.end())
    return;
  auto c = s.cells.find(cell(it->second.position, s.cell_m));
  c->second.erase(key);
  if (c->second.empty())
    s.cells.erase(c);
  s.rows.erase(it);
  s.dirty.erase(key);
}
void subscribe(LightSubscriptions &s, LightPoint point) {
  unsubscribe(s, point.subject, point.measured_term);
  auto key = std::pair{point.subject.bits, point.measured_term};
  s.rows.emplace(key, point);
  s.cells[cell(point.position, s.cell_m)].insert(key);
  s.dirty.insert(key);
}
void dirty_points(LightSubscriptions &s, std::span<const space::Bounds> bounds) {
  for (auto b : bounds) {
    auto lo = cell(b.min, s.cell_m), hi = cell(b.max, s.cell_m);
    for (auto x = lo[0]; x <= hi[0]; ++x)
      for (auto y = lo[1]; y <= hi[1]; ++y)
        if (auto it = s.cells.find({x, y}); it != s.cells.end())
          for (auto key : it->second)
            if (distance(s.rows.at(key).position, b) == 0)
              s.dirty.insert(key);
  }
}
void dirty_geometry(LightSubscriptions &s, const Lights &lights,
                    std::span<const space::Bounds> bounds) {
  dirty_points(s, bounds);
  std::unordered_set<std::uint64_t> affected;
  for (auto b : bounds) {
    const auto lo = cell(b.min, lights.cell_m), hi = cell(b.max, lights.cell_m);
    for (auto x = lo[0]; x <= hi[0]; ++x)
      for (auto y = lo[1]; y <= hi[1]; ++y)
        if (auto found = lights.candidates.find({x, y}); found != lights.candidates.end())
          for (auto h : found->second) {
            const auto &light = lights.rows.at(h.bits);
            if (distance(light.position, b) <= light.binding.radius_m)
              affected.insert(h.bits);
          }
  }
  // A changed occluder can cast a shadow anywhere in this light's influence.
  for (auto bits : affected) {
    const auto &light = lights.rows.at(bits);
    dirty_light(s, light.position, light.binding.radius_m);
  }
}
void dirty_light(LightSubscriptions &s, Vec3 p, float radius) {
  cells(p, radius, s.cell_m, [&](Cell c) {
    if (auto it = s.cells.find(c); it != s.cells.end())
      for (auto key : it->second)
        if ((s.rows.at(key).position - p).length_squared() <= radius * radius)
          s.dirty.insert(key);
  });
}
std::vector<LightPoint> take_dirty(LightSubscriptions &s) {
  std::vector<LightPoint> out;
  out.reserve(s.dirty.size());
  for (auto key : s.dirty)
    out.push_back(s.rows.at(key));
  s.dirty.clear();
  return out;
}
Scan scan(const amber::ObjectState &objects_state, const Observer &observer, Handle held,
          const Settings &settings, amber::space::State &space_state, double world_lux,
          std::size_t requested) {
  auto &world = space_state.space;

  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto transforms = amber::transforms(std::as_const(objects_state.objects));
  auto &parts = objects_state.declared_parts;
  auto &lights = space_state.lights;
  auto &ambients = space_state.zones;
  auto &motion = space_state.motion;

  Scan out;
  space::volumes_at(world, identity, observer.eye, space::VolumeKind::Zone, out.zones);
  std::vector<space::Nearby> nearby;
  space::near(world, identity, observer.eye, settings.far_m, {observer.body, {}, false}, nearby);
  struct Candidate {
    space::Nearby nearby;
    Handle object;
    space::Bounds bounds;
    float distance_m, angular_size;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(nearby.size());
  std::unordered_set<std::uint64_t> visited;
  for (const auto &row : nearby) {
    const auto object = logical_root(world, identity, parts, row.object);
    if (object == observer.body || !visited.insert(object.bits).second)
      continue;
    const auto bounds = space::bounds(space_state, identity, object).value_or(row.world_bounds);
    const auto size = bounds.max - bounds.min;
    const auto dist = distance(observer.eye, bounds);
    candidates.push_back(
        {row, object, bounds, dist, std::max({size.x, size.y, size.z}) / std::max(dist, .001f)});
  }
  std::sort(candidates.begin(), candidates.end(), [&](const auto &a, const auto &b) {
    const bool near_a = a.nearby.distance_m <= settings.sight_m;
    const bool near_b = b.nearby.distance_m <= settings.sight_m;
    if (near_a != near_b)
      return near_a;
    if (!near_a && a.angular_size != b.angular_size)
      return a.angular_size > b.angular_size;
    return a.nearby.distance_m == b.nearby.distance_m ? a.nearby.object < b.nearby.object
                                                      : a.nearby.distance_m < b.nearby.distance_m;
  });
  const auto speed = [&](Handle object) {
    const auto found = std::ranges::lower_bound(
        motion, object.bits, {}, [](const MotionSample &row) { return row.object.bits; });
    return found != motion.end() && found->object == object ? found->velocity.length() : 0.f;
  };
  std::vector<space::LightSample> samples, eye_samples;
  bool eye_samples_ready = false;
  std::vector<Handle> ignore, zones;
  auto limit = requested ? requested : settings.things_max;
  for (const auto &candidate : candidates) {
    auto object = candidate.object;
    auto b = candidate.bounds;
    auto center = (b.min + b.max) * .5f;
    float dist = candidate.distance_m;
    bool is_held = object == held;
    bool near = dist <= settings.sight_m || is_held;
    if ((near && out.things.size() >= limit) || (!near && out.far.size() >= settings.far_max))
      continue;
    auto size = b.max - b.min;
    bool large = 2 * std::atan(candidate.angular_size / 2) * 180 / std::numbers::pi_v<float> >=
                 settings.far_angular_degrees;
    bool moving = speed(object) >= settings.moving_m_s;
    bool bright = false;
    if (auto it = lights.rows.find(object.bits); it != lights.rows.end()) {
      if (!eye_samples_ready) {
        light_samples(lights, identity, observer.eye, eye_samples);
        eye_samples_ready = true;
      }
      for (auto s : eye_samples)
        if (s.source == object) {
          auto lux = illuminate(world, identity, observer.body, observer.eye, 0, std::span(&s, 1));
          bright = lux >= settings.far_source_lux;
        }
    }
    if (!near && !large && !moving && !bright)
      continue;
    ignore.clear();
    descendants(parts, object, ignore);
    ignore.push_back(observer.body);
    std::array<Vec3, 5> points{center, Vec3{b.min.x, center.y, b.min.z + size.z * .25f},
                               Vec3{b.max.x, center.y, b.min.z + size.z * .25f},
                               Vec3{b.min.x, center.y, b.min.z + size.z * .75f},
                               Vec3{b.max.x, center.y, b.min.z + size.z * .75f}};
    space::Visibility visible{is_held, 1, {}};
    if (!is_held)
      for (auto p : points) {
        visible =
            space::visibility(world, identity, observer.eye, p, {{}, {}, false, ignore}, .0001f);
        if (visible.clear)
          break;
      }
    if (!visible.clear)
      continue;
    zones.clear();
    space::volumes_at(world, identity, center, space::VolumeKind::Zone, zones);
    light_samples(lights, identity, center, samples);
    auto illumination = illuminate(world, identity, object, center,
                                   ambient_at(ambients, zones, world_lux), samples);
    Seen seen{object,
              is_held ? 0 : dist,
              bearing(observer.forward, center - observer.eye),
              illumination,
              is_held,
              illumination >= observer.dim_lux,
              distance(transforms.position[observer.body.slot()], b) <= settings.reach_m,
              visible.through,
              near     ? Seen::Salience::Near
              : large  ? Seen::Salience::Large
              : moving ? Seen::Salience::Moving
                       : Seen::Salience::Bright};
    (near ? out.things : out.far).push_back(std::move(seen));
  }
  if (held && identity.valid(held) &&
      std::none_of(out.things.begin(), out.things.end(),
                   [&](const Seen &s) { return s.object == held; })) {
    auto delta = transforms.position[held.slot()] - observer.eye;
    out.things.push_back({held,
                          0,
                          bearing(observer.forward, delta),
                          world_lux,
                          true,
                          world_lux >= observer.dim_lux,
                          true,
                          {},
                          Seen::Salience::Near});
  }
  auto order = [](const Seen &a, const Seen &b) {
    return a.distance_m == b.distance_m ? a.object < b.object : a.distance_m < b.distance_m;
  };
  std::sort(out.things.begin(), out.things.end(), order);

  if (out.things.size() > limit)
    out.things.resize(limit);
  return out;
}
void emit_sound(Hearing &hearing, const Sound &sound, std::span<const Listener> listeners) {
  for (auto listener : listeners) {
    auto distance = (listener.ear - sound.at).length();
    if (distance <= sound.radius_m)
      hearing.queues[listener.session].push_back({sound, distance});
  }
}
std::vector<Heard> take_heard(Hearing &hearing, std::uint64_t session, std::size_t limit,
                              std::size_t &omitted) {
  auto it = hearing.queues.find(session);
  omitted = 0;
  if (it == hearing.queues.end())
    return {};
  auto &queue = it->second;
  omitted = queue.size() > limit ? queue.size() - limit : 0;
  std::vector<Heard> result;
  result.reserve(std::min(queue.size(), limit));
  for (auto i = omitted; i < queue.size(); ++i)
    result.push_back(std::move(queue[i]));
  hearing.queues.erase(it);
  return result;
}
void remove_listener(Hearing &h, std::uint64_t session) {
  h.queues.erase(session);
}
} // namespace amber::observe
