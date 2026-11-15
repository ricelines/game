#pragma once

#include "catalog/catalog.hpp"
#include "space/space.hpp"

#include <deque>
#include <map>
#include <set>

namespace amber::observe {
using Deadline = space::Deadline;
using Scalar = std::variant<double, TermRef>;
using Cell = std::array<std::int32_t, 2>;
struct Observer {
  Handle body;
  Vec3 eye, forward;
  double dim_lux;
};
struct Settings {
  float sight_m, reach_m, far_m, far_angular_degrees, moving_m_s;
  double far_source_lux;
  std::size_t things_max, far_max, events_per_response;
};
struct LightBinding {
  Handle source;
  Vec3 local_site;
  Scalar intensity;
  float radius_m, source_radius_m;
};
struct ZoneAmbient {
  Handle zone;
  Scalar ambient;
  int priority;
};
struct Lights {
  struct Row {
    LightBinding binding;
    Vec3 position;
    std::vector<Cell> cells;
  };
  float cell_m;
  std::unordered_map<std::uint64_t, Row> rows;
  std::map<Cell, std::vector<Handle>> candidates;
  explicit Lights(float cell_m);
};
struct ZoneAmbients {
  SparseColumn<ZoneAmbient> rows;
};
void set_light(Lights &, const LightBinding &, TransformRead);
void move_light(Lights &, Handle, TransformRead);
void remove_light(Lights &, Handle);
void set_zone(ZoneAmbients &, ZoneAmbient);
void remove_zone(ZoneAmbients &, Handle);
void light_samples(const Lights &, IdentityRead, const react::ValueRead &, Step, double hz,
                   Vec3 point, std::vector<space::LightSample> &);
double ambient_at(const ZoneAmbients &, const react::ValueRead &, std::span<const Handle>, Step,
                  double hz, double world_lux);
space::Illumination illuminate(const space::Space &, IdentityRead, Handle receiver, Vec3,
                               double ambient_lux, std::span<const space::LightSample>, Deadline);
struct LightPoint {
  Handle subject;
  Vec3 position;
  TermId measured_term;
};
struct LightSubscriptions {
  float cell_m;
  std::map<std::pair<std::uint64_t, TermId>, LightPoint> rows;
  std::map<Cell, std::set<std::pair<std::uint64_t, TermId>>> cells;
  std::set<std::pair<std::uint64_t, TermId>> dirty;
  explicit LightSubscriptions(float cell_m);
};
void subscribe(LightSubscriptions &, LightPoint);
void unsubscribe(LightSubscriptions &, Handle, TermId);
void dirty_points(LightSubscriptions &, std::span<const space::Bounds>);
struct ActiveViews;
void dirty_geometry(LightSubscriptions &, const Lights &, std::span<const space::Bounds>,
                    ActiveViews *views = nullptr);
void dirty_light(LightSubscriptions &, Vec3, float radius_m);
std::vector<LightPoint> take_dirty(LightSubscriptions &);
struct ActiveViews {
  struct View {
    Vec3 eye;
    float radius_m;
    std::vector<Cell> cells;
  };
  float cell_m;
  std::unordered_map<std::uint64_t, View> rows;
  std::map<Cell, std::set<std::uint64_t>> cells;
  std::set<std::uint64_t> dirty;
  explicit ActiveViews(float cell_m);
};
void track_view(ActiveViews &, std::uint64_t session, Vec3 eye, float radius_m);
void remove_view(ActiveViews &, std::uint64_t session);
void dirty_views(ActiveViews &, std::span<const space::Bounds>);
std::vector<std::uint64_t> take_dirty_views(ActiveViews &);
struct MotionSample {
  Handle object;
  Vec3 velocity;
};
struct Seen {
  Handle object;
  float distance_m, bearing_degrees;
  double lux;
  bool held, lit, reachable;
  std::vector<Handle> through;
  enum class Salience { Near, Large, Moving, Bright } salience;
};
struct Scan {
  bool complete;
  std::vector<Seen> things, far;
  std::vector<Handle> zones;
};
Scan scan(const space::Space &, IdentityRead, TransformRead, const Relation &declared_parts,
          const Observer &, Handle held, const Settings &, const Lights &, const ZoneAmbients &,
          const react::ValueRead &, std::span<const MotionSample>, Step, double hz,
          double world_ambient_lux, std::size_t requested_limit, Deadline);
struct ObjectDescription {
  Handle object;
  std::string model;
  nlohmann::json measured;
  catalog::Text says;
  std::vector<std::string> actions;
  nlohmann::json elaborated = nlohmann::json::array();
};
struct PreparedThing {
  Seen seen;
  ObjectDescription description;
};
struct SelfDescription {
  Handle body, holding;
  Vec3 position;
  float facing_degrees;
  nlohmann::json intent;
  std::vector<std::pair<Handle, std::string>> zones;
};
struct Sound {
  Handle source;
  Vec3 at;
  float radius_m;
  EventPayload payload;
  PlayerId speaker;
  record::Origin origin;
  std::string name{};
  bool speech = false;
};
struct Listener {
  std::uint64_t session;
  Handle body;
  Vec3 ear;
};
struct Heard {
  Sound sound;
  float distance_m;
};
struct Hearing {
  std::unordered_map<std::uint64_t, std::deque<Heard>> queues;
};
void emit_sound(Hearing &, const Sound &, std::span<const Listener>);
nlohmann::json event_payload_json(const EventPayload &);
std::vector<Heard> take_heard(Hearing &, std::uint64_t, std::size_t limit, std::size_t &omitted);
void remove_listener(Hearing &, std::uint64_t);
nlohmann::json observation(Step, double hz, const SelfDescription &, std::span<const PreparedThing>,
                           std::span<const PreparedThing>,
                           std::span<const std::string> body_actions,
                           std::span<const std::string> player_actions);
} // namespace amber::observe
