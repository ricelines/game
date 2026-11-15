#include "observe.hpp"

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
double scalar(const Scalar &s, const react::ValueRead &read, Handle h, Step now, double hz) {
  if (const auto *constant = std::get_if<double>(&s))
    return *constant;
  auto a = react::read(read, h, std::get<TermRef>(s).id, now, hz);
  return a.present && a.value.is_numeric() ? a.value.numeric() : 0;
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
nlohmann::json vector(Vec3 p) {
  return {p.x, p.y, p.z};
}
nlohmann::json atom(const Value &v) {
  switch (v.kind) {
  case Value::Kind::None:
    return nullptr;
  case Value::Kind::Number:
    return v.number;
  case Value::Kind::Integer:
    return v.integer;
  case Value::Kind::Boolean:
    return v.boolean;
  case Value::Kind::Object:
    return to_string(v.object);
  }
  return nullptr;
}
template <class T> nlohmann::json event_atom(const T &v) {
  return std::visit(
      [](const auto &value) -> nlohmann::json {
        using V = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<V, Value>)
          return atom(value);
        else if constexpr (std::is_same_v<V, Vec3>)
          return vector(value);
        else if constexpr (std::is_same_v<V, TermRef>)
          return {{"term", value.id}};
        else if constexpr (std::is_same_v<V, std::string>)
          return value;
        else {
          nlohmann::json result = nlohmann::json::object();
          for (const auto &field : value)
            result[field.name] = event_atom(field.value);
          return result;
        }
      },
      v);
}
nlohmann::json full(const PreparedThing &p, bool far) {
  const auto &s = p.seen;
  const auto &d = p.description;
  nlohmann::json row{
      {"h", to_string(s.object)},
      {"model", d.model},
      {"measured", d.measured},
      {"says",
       {{"name", d.says.name}, {"description", d.says.description}, {"writing", d.says.writing}}},
      {"lit", s.lit},
      {"lux", s.lux},
      {"held", s.held}};
  if (!d.elaborated.empty())
    row["elaborated"] = d.elaborated;
  if (s.reachable)
    row["actions"] = d.actions;
  if (!s.through.empty()) {
    row["through"] = nlohmann::json::array();
    for (auto h : s.through)
      row["through"].push_back(to_string(h));
  }
  if (far) {
    const char *reason = s.salience == Seen::Salience::Large    ? "large"
                         : s.salience == Seen::Salience::Moving ? "moving"
                                                                : "bright";
    row["why"] = reason;
  }
  return row;
}
} // namespace
nlohmann::json event_payload_json(const EventPayload &payload) {
  return event_atom(payload);
}
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
void light_samples(const Lights &lights, IdentityRead identity, const react::ValueRead &read,
                   Step now, double hz, Vec3 point, std::vector<space::LightSample> &out) {
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
    auto intensity = scalar(b.intensity, read, h, now, hz);
    if (intensity > 0)
      out.push_back({h, row.position, intensity, b.radius_m, b.source_radius_m});
  }
}
double ambient_at(const ZoneAmbients &zones, const react::ValueRead &read,
                  std::span<const Handle> containing, Step now, double hz, double world) {
  const ZoneAmbient *best = nullptr;
  for (auto h : containing)
    if (auto *z = zones.rows.get(h.slot()); z && z->zone == h &&
                                            (!best || z->priority > best->priority ||
                                             (z->priority == best->priority && h < best->zone)))
      best = z;
  return best ? scalar(best->ambient, read, best->zone, now, hz) : world;
}
space::Illumination illuminate(const space::Space &world, IdentityRead identity, Handle receiver,
                               Vec3 p, double ambient, std::span<const space::LightSample> samples,
                               Deadline deadline) {
  return space::illuminance(world, identity, receiver, p, ambient, samples, deadline);
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
                    std::span<const space::Bounds> bounds, ActiveViews *views) {
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
    if (views) {
      const auto r = light.binding.radius_m;
      const Vec3 extent{r, r, r};
      const space::Bounds influence{light.position - extent, light.position + extent};
      dirty_views(*views, std::span(&influence, 1));
    }
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
ActiveViews::ActiveViews(float side) : cell_m(side) {
  if (!(side > 0))
    throw std::invalid_argument("view cell size must be positive");
}
void remove_view(ActiveViews &views, std::uint64_t session) {
  auto it = views.rows.find(session);
  if (it == views.rows.end())
    return;
  for (auto key : it->second.cells) {
    auto c = views.cells.find(key);
    c->second.erase(session);
    if (c->second.empty())
      views.cells.erase(c);
  }
  views.rows.erase(it);
  views.dirty.erase(session);
}
void track_view(ActiveViews &views, std::uint64_t session, Vec3 eye, float radius) {
  remove_view(views, session);
  ActiveViews::View row{eye, radius, {}};
  cells(eye, radius, views.cell_m, [&](Cell c) {
    row.cells.push_back(c);
    views.cells[c].insert(session);
  });
  views.rows.emplace(session, std::move(row));
  views.dirty.insert(session);
}
void dirty_views(ActiveViews &views, std::span<const space::Bounds> edited) {
  for (auto bounds : edited) {
    auto lo = cell(bounds.min, views.cell_m), hi = cell(bounds.max, views.cell_m);
    for (auto x = lo[0]; x <= hi[0]; ++x)
      for (auto y = lo[1]; y <= hi[1]; ++y)
        if (auto it = views.cells.find({x, y}); it != views.cells.end())
          for (auto session : it->second) {
            const auto &view = views.rows.at(session);
            if (distance(view.eye, bounds) <= view.radius_m)
              views.dirty.insert(session);
          }
  }
}
std::vector<std::uint64_t> take_dirty_views(ActiveViews &views) {
  std::vector<std::uint64_t> out(views.dirty.begin(), views.dirty.end());
  views.dirty.clear();
  return out;
}
Scan scan(const space::Space &world, IdentityRead identity, TransformRead transforms,
          const Relation &parts, const Observer &observer, Handle held, const Settings &settings,
          const Lights &lights, const ZoneAmbients &ambients, const react::ValueRead &values,
          std::span<const MotionSample> motion, Step now, double hz, double world_lux,
          std::size_t requested, Deadline deadline) {
  Scan out{true, {}, {}, {}};
  out.complete = space::volumes_at(world, identity, observer.eye, space::VolumeKind::Zone, deadline,
                                   out.zones);
  std::vector<space::Nearby> candidates;
  out.complete &= space::near(world, identity, observer.eye, settings.far_m,
                              {observer.body, {}, false}, deadline, candidates);
  std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
    return a.distance_m == b.distance_m ? a.object < b.object : a.distance_m < b.distance_m;
  });
  std::unordered_map<std::uint64_t, float> speeds;
  for (auto row : motion)
    speeds[row.object.bits] = row.velocity.length();
  std::unordered_set<std::uint64_t> visited;
  std::vector<space::LightSample> samples;
  std::vector<Handle> ignore, zones;
  auto limit = requested ? requested : settings.things_max;
  for (const auto &candidate : candidates) {
    if (std::chrono::steady_clock::now() >= deadline) {
      out.complete = false;
      break;
    }
    auto object = logical_root(world, identity, parts, candidate.object);
    if (object == observer.body || !visited.insert(object.bits).second)
      continue;
    auto b = space::bounds(world, identity, object).value_or(candidate.world_bounds);
    auto center = (b.min + b.max) * .5f;
    float dist = distance(observer.eye, b);
    bool is_held = object == held;
    bool near = dist <= settings.sight_m || is_held;
    if ((near && out.things.size() >= limit) || (!near && out.far.size() >= settings.far_max))
      continue;
    auto size = b.max - b.min;
    bool large = 2 * std::atan(std::max({size.x, size.y, size.z}) / (2 * std::max(dist, .001f))) *
                     180 / std::numbers::pi_v<float> >=
                 settings.far_angular_degrees;
    bool moving = speeds[object.bits] >= settings.moving_m_s;
    bool bright = false;
    if (auto it = lights.rows.find(object.bits); it != lights.rows.end()) {
      light_samples(lights, identity, values, now, hz, observer.eye, samples);
      for (auto s : samples)
        if (s.source == object) {
          auto lux = illuminate(world, identity, observer.body, observer.eye, 0, std::span(&s, 1),
                                deadline);
          out.complete &= lux.complete;
          bright = lux.lux >= settings.far_source_lux;
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
    space::Visibility visible{true, is_held, 1, {}};
    if (!is_held)
      for (auto p : points) {
        visible = space::visibility(world, identity, observer.eye, p, {{}, {}, false, ignore},
                                    .0001f, deadline);
        if (!visible.complete)
          out.complete = false;
        if (visible.clear || !visible.complete)
          break;
      }
    if (!visible.clear)
      continue;
    zones.clear();
    out.complete &=
        space::volumes_at(world, identity, center, space::VolumeKind::Zone, deadline, zones);
    light_samples(lights, identity, values, now, hz, center, samples);
    auto illumination =
        illuminate(world, identity, object, center,
                   ambient_at(ambients, values, zones, now, hz, world_lux), samples, deadline);
    out.complete &= illumination.complete;
    Seen seen{object,
              is_held ? 0 : dist,
              bearing(observer.forward, center - observer.eye),
              illumination.lux,
              is_held,
              illumination.lux >= observer.dim_lux,
              dist <= settings.reach_m,
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
  std::sort(out.far.begin(), out.far.end(), order);
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
nlohmann::json observation(SessionCache &cache, Step now, double hz, const SelfDescription &self,
                           std::span<const PreparedThing> things,
                           std::span<const PreparedThing> far, std::span<const Heard> heard,
                           std::span<const std::string> body_actions,
                           std::span<const std::string> player_actions) {
  nlohmann::json result{{"step", now},
                        {"time_s", now / hz},
                        {"self",
                         {{"body", to_string(self.body)},
                          {"pos", vector(self.position)},
                          {"facing", self.facing_degrees},
                          {"holding", self.holding ? nlohmann::json(to_string(self.holding))
                                                   : nlohmann::json(nullptr)},
                          {"intent", self.intent},
                          {"zones", nlohmann::json::array()}}},
                        {"things", nlohmann::json::array()},
                        {"far", nlohmann::json::array()},
                        {"events", nlohmann::json::array()}};
  for (auto &[h, name] : self.zones)
    result["self"]["zones"].push_back({{"h", to_string(h)}, {"name", name}});
  std::unordered_set<std::uint64_t> present;
  auto rows = [&](std::span<const PreparedThing> rows, const char *key, bool distant) {
    for (const auto &p : rows) {
      auto row = full(p, distant);
      auto it = cache.known.find(p.seen.object.bits);
      bool changed = it == cache.known.end() || it->second != row;
      cache.known[p.seen.object.bits] = row;
      present.insert(p.seen.object.bits);
      if (!changed)
        row = {{"h", to_string(p.seen.object)}};
      row["dist_m"] = p.seen.distance_m;
      row["bearing"] = p.seen.bearing_degrees;
      result[key].push_back(std::move(row));
    }
  };
  rows(things, "things", false);
  rows(far, "far", true);
  std::erase_if(cache.known, [&](const auto &entry) { return !present.contains(entry.first); });
  auto action_list = [&](auto &cached, std::span<const std::string> actions, const char *key) {
    std::vector<std::string> value(actions.begin(), actions.end());
    if (!cached || *cached != value) {
      result[key] = value;
      cached = std::move(value);
    }
  };
  action_list(cache.body_actions, body_actions, "body_actions");
  action_list(cache.player_actions, player_actions, "player_actions");
  for (const auto &event : heard)
    result["events"].push_back({{"kind", "sound"},
                                {"source", to_string(event.sound.source)},
                                {"at", vector(event.sound.at)},
                                {"dist_m", event.distance_m},
                                {"speaker", event.sound.speaker},
                                {"data", event_payload_json(event.sound.payload)},
                                {"root", event.sound.origin.root}});
  return result;
}
void forget(SessionCache &cache) {
  cache.known.clear();
  cache.body_actions.reset();
  cache.player_actions.reset();
}
} // namespace amber::observe
