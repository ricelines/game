#pragma once
namespace amber::space {
struct State;
}

#include "snap/snap.hpp"
#include "store/store.hpp"

#include <chrono>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <variant>

namespace amber::space {
using GeometryId = std::uint32_t;
struct Pose {
  Vec3 position;
  Quat rotation;
};
Vec3 transform_point(Pose, Vec3);
struct Bounds {
  Vec3 min, max;
};
struct GeometrySource {
  std::span<const std::byte> static_jolt, convex_jolt, navigation_mesh;
  Bounds local_bounds;
};
struct ImportResult {
  GeometryId geometry;
  std::string error;
};
enum class Motion { Static, Dynamic, Kinematic, Character };
struct Surface {
  float friction, restitution, transmission;
};
struct CharacterSpec {
  float radius_m, height_m, mass_kg, max_push_force_n;
  float max_speed_m_s, acceleration_m_s2;
  float max_step_m, max_slope_radians, floor_snap_m;
};
struct BodySpec {
  Handle object;
  GeometryId geometry;
  Motion motion;
  Surface surface;
  float mass_kg;
  bool navigation_surface, navigation_obstacle;
  std::optional<CharacterSpec> character;
  bool collision_solid = true;
};
struct NavSettings {
  float cell_m, cell_height_m;
  std::uint32_t tile_cells;
  float radius_m, height_m, climb_m, slope_degrees;
  float max_edge_m, contour_error_m, detail_sample_m, detail_error_m;
  std::uint32_t min_region_cells, merge_region_cells;
  std::uint32_t nodes_per_query, polygons_per_path, workers;
  std::uint32_t max_tiles, polygons_per_tile;
};
struct Settings {
  Vec3 gravity_m_s2;
  NavSettings navigation;
  std::uint32_t max_bodies, max_body_pairs, max_contacts, physics_temp_bytes;
  float contact_slop_m, sweep_skin_m, crowd_correction_m, blocked_after_s;
  std::uint32_t max_characters;
  float crowd_query_radius_multiplier, crowd_optimize_radius_multiplier, crowd_separation_weight;
};
struct Space {
  struct Native;
  std::unique_ptr<Native> native;
  explicit Space(const Settings &);
  ~Space();
  Space(const Space &) = delete;
  Space &operator=(const Space &) = delete;
};
using Edges = amber::Relation;
using CauseToken = std::uint64_t;
struct CausePair {
  CauseToken first = 0, second = 0;
};
enum class CauseScope { Step, Motion };
struct PhysicalRelations {
  Edges contains, supports, attached_to, in_zone;
};
enum class RelationKind { Contains, Supports, AttachedTo, InZone };
struct RelationChange {
  RelationKind kind;
  Handle from, to;
  bool added;
  CausePair causes{};
};
enum class Status {
  Ok,
  StaleHandle,
  MissingGeometry,
  MissingBody,
  UnsupportedMotion,
  Obstructed,
  AlreadyParented,
  AttachmentCycle,
  NotAttached,
  Capacity
};
const char *status_text(Status);
struct OperationResult {
  Status status;
  Handle obstacle;
};
struct BodyCapacity {
  std::uint32_t native_available, characters_available;
};
struct BodyPreflight {
  Status status;
  std::size_t failed_index, required_native, available_native;
};
BodyCapacity capacity(const Space &);
// Future handles may be null. Retiring handles are unique and must be removed
// before adding these bodies. Sensors consume one native body each.
BodyPreflight can_add(const Space &, std::span<const BodySpec>, std::size_t sensor_count = 0,
                      std::span<const Handle> retiring = {});
ImportResult import_geometry(Space &, const GeometrySource &);
GeometryId sensor_box_geometry(Space &, Vec3 size);
struct BodyMotion {
  Motion motion;
  bool collision_solid, independent;
};
std::optional<BodyMotion> body_motion(const Space &, IdentityRead, Handle);
OperationResult add(Space &, IdentityRead, TransformRead, const BodySpec &);
OperationResult register_logical_root(Space &, IdentityRead, Handle);
bool is_logical_root(const Space &, IdentityRead, Handle);
OperationResult assemble(Space &, IdentityRead, TransformRead, Handle,
                         std::span<const Handle> declared_descendants,
                         std::optional<Motion> motion = {}, CauseToken cause = 0);
void remove(Space &, IdentityRead, Handle, ParentWrite, SpaceFlags, PhysicalRelations &,
            std::vector<RelationChange> &);
// Leaves no query-visible native body or attachment. A null custodian is offline storage.
OperationResult can_custody(const Space &, IdentityRead, Handle, Handle custodian, ParentRead,
                            std::span<const Handle> declared_descendants = {});
struct VisibleCustody {
  Handle goods, parent;
  bool grasp = false;
};
OperationResult can_visible_custody(const Space &, IdentityRead, ParentRead, const VisibleCustody &,
                                    std::span<const Handle> declared_descendants = {});
OperationResult visible_custody(amber::ObjectState &objects_state, amber::space::State &space_state,
                                const VisibleCustody &,
                                std::span<const Handle> declared_descendants = {});
OperationResult custody(amber::ObjectState &objects_state, Handle, Handle custodian,
                        amber::space::State &space_state,
                        std::span<const Handle> declared_descendants = {});
OperationResult set_surface(Space &, IdentityRead, Handle, Surface);
struct BodyProperties {
  float mass_kg;
  Surface surface;
};
OperationResult set_properties(Space &, IdentityRead, Handle, BodyProperties);
OperationResult set_pose(Space &, IdentityRead, TransformWrite, Handle, Pose, CauseToken cause = 0);
CauseToken motion_cause(const Space &, IdentityRead, Handle);
void active_cause_tokens(const Space &, std::vector<CauseToken> &);
OperationResult restore_motion_cause(Space &, IdentityRead, Handle, CauseToken);
enum class VolumeKind { Zone, Container };
struct VolumeSpec {
  Handle owner;
  GeometryId geometry;
  Pose local_pose;
  VolumeKind kind;
};
OperationResult add_volume(Space &, IdentityRead, TransformRead, const VolumeSpec &);
void volumes_at(const Space &, IdentityRead, Vec3, VolumeKind, std::vector<Handle> &);
std::optional<Bounds> volume_bounds(const Space &, Handle, VolumeKind);
struct QueryFilter {
  Handle ignore_a, ignore_b;
  bool include_sensors = false;
  std::span<const Handle> ignore = {};
  RunDeadline deadline{};
};
struct Hit {
  Handle object;
  float fraction;
  Vec3 position, normal;
};
struct Visibility {
  bool clear;
  float transmission;
  std::vector<Handle> through;
};
struct Nearby {
  Handle object;
  float distance_m;
  Bounds world_bounds;
};
std::optional<Hit> raycast(const Space &, IdentityRead, Vec3, Vec3, QueryFilter);
Visibility visibility(const Space &, IdentityRead, Vec3, Vec3, QueryFilter,
                      float minimum_transmission);
void near(const Space &, IdentityRead, Vec3, float radius_m, QueryFilter, std::vector<Nearby> &);
std::optional<Bounds> bounds(const Space &, IdentityRead, Handle);
struct LightSample {
  Handle source;
  Vec3 position;
  double intensity_cd;
  float range_m, source_radius_m;
};
double illuminance(const Space &, IdentityRead, Handle receiver, Vec3, double ambient_lux,
                   std::span<const LightSample>);
struct Attachment {
  Handle subject, parent;
  Pose local_pose;
  bool grasp;
};
struct Release {
  Handle subject;
  Motion motion;
  Vec3 linear_velocity_m_s, angular_velocity_rad_s;
};
OperationResult attach(amber::ObjectState &objects_state, amber::space::State &space_state,
                       const Attachment &, CauseToken cause = 0);
OperationResult detach(amber::ObjectState &objects_state, amber::space::State &space_state,
                       const Release &, CauseToken cause = 0);
OperationResult impulse(Space &, IdentityRead, Handle, Vec3 newton_seconds, Vec3 world_point,
                        CauseToken cause = 0);
std::optional<Hit> sweep(const Space &, IdentityRead, Handle, Vec3 translation_m, QueryFilter);
struct SweepMove {
  Status status;
  Pose actual;
  std::optional<Hit> contact;
};
SweepMove translate(Space &, IdentityRead, TransformWrite, Handle, Vec3, float seconds,
                    CauseToken cause = 0);
struct PointGoal {
  Vec3 point;
  float tolerance_m;
};
struct ObjectGoal {
  Handle target;
  float reach_m;
};
using Goal = std::variant<PointGoal, ObjectGoal>;
struct TravelBoundary {
  std::vector<Vec3> polygon;
  float min_z, max_z;
};
struct MoveRequest {
  Handle body;
  std::uint64_t intent;
  Goal goal;
  std::span<const TravelBoundary> forbidden;
  std::optional<float> speed_m_s = {};
  CauseToken cause = 0;
};
enum class PathStatus { Ready, PendingTiles, NoSurface, Unreachable, AccessDenied };
struct Path {
  PathStatus status;
  std::vector<Vec3> corners;
  float length_m;
  std::uint64_t navigation_revision;
};
Path path(const Space &, IdentityRead, Handle, const Goal &, std::span<const TravelBoundary>);
enum class IntentState { Moving, WaitingNavigation, Arrived, Blocked, TargetGone };
struct IntentProgress {
  Handle body;
  std::uint64_t intent;
  IntentState state;
  float remaining_m;
  PathStatus path_status;
};
struct Contact {
  Handle first, second;
  Vec3 point, normal;
  bool started;
  CausePair causes{};
};
struct StepResult {
  std::vector<Handle> moved;
  std::vector<Bounds> dirty_bounds;
  std::vector<Contact> contacts;
  std::vector<RelationChange> relations;
  std::vector<IntentProgress> intents;
};
void step(Space &, amber::ObjectState &objects_state, PhysicalRelations &, Step, float seconds,
          std::span<const MoveRequest>, StepResult &);
struct Tile {
  std::int32_t x, y;
  auto operator<=>(const Tile &) const = default;
};
struct NavigationInstall {
  std::uint64_t revision;
  std::vector<Tile> installed, pending;
  std::vector<std::string> errors;
};
NavigationInstall install_ready_navigation(Space &);
std::uint64_t navigation_revision(const Space &);
struct MotionRow {
  Handle object;
  Vec3 linear_velocity_m_s, angular_velocity_rad_s;
  bool asleep;
};
void capture_motion(const Space &, IdentityRead, std::vector<MotionRow> &);
void capture_motion(const Space &, IdentityRead, std::span<const Handle>, std::vector<MotionRow> &);
std::optional<MotionRow> motion(const Space &, IdentityRead, Handle);
// Matches the speed clamp used by character navigation; absent for other bodies.
std::optional<float> movement_speed(const Space &, IdentityRead, Handle,
                                    std::optional<float> requested = {});
void restore_motion(Space &, IdentityRead, std::span<const MotionRow>);
void rebuild_attachments(Space &, IdentityRead, TransformRead, ParentRead,
                         const Edges &attached_to);
struct EditedMotion {
  Handle object;
  Motion motion;
  Vec3 linear_velocity_m_s, angular_velocity_rad_s;
  bool asleep;
};
struct AssemblyEdit {
  Handle root;
  bool in_scene, promoted;
  std::vector<Handle> before;
  std::vector<EditedMotion> motion;
};
Handle assembly_root(const Space &, IdentityRead, Handle);
BodyPreflight can_rebuild_assembly(const Space &, std::span<const BodySpec>,
                                   std::size_t sensor_count, std::span<const Handle> before,
                                   bool promoted, std::optional<Motion> root_motion = std::nullopt);
AssemblyEdit extract_assembly(Space &, IdentityRead, Handle root,
                              std::span<const Handle> declared_descendants);
OperationResult rebuild_assembly(amber::ObjectState &objects_state,
                                 amber::space::State &space_state, const AssemblyEdit &,
                                 std::span<const BodySpec>, std::span<const Handle> logical_roots,
                                 std::span<const VolumeSpec>,
                                 std::span<const Attachment> internal_attachments);
struct NavigationArea {
  Vec3 point;
  std::uint32_t connected_component;
  bool reachable;
};
struct NavigationReport {
  std::uint64_t revision;
  bool complete = false, reference_navigable = false;
  std::vector<NavigationArea> areas;
};
NavigationReport diagnose_navigation(const Space &, std::span<const Bounds> changed_regions,
                                     Vec3 reference);
struct DebugLine {
  Vec3 from, to;
  std::uint32_t rgba;
};
struct DebugSelection {
  Bounds region;
  bool collision, navigation, paths, volumes;
};
void debug_lines(const Space &, const DebugSelection &, std::vector<DebugLine> &);
void write_physical_relations(snap::Output &, const PhysicalRelations &,
                              const snap::ObjectSelection & = {});
void read_physical_relations(snap::Input &, PhysicalRelations &, std::size_t slots);
void write_motion(snap::Output &, const std::vector<MotionRow> &,
                  const snap::ObjectSelection & = {});
void read_motion(snap::Input &, std::vector<MotionRow> &);

} // namespace amber::space

namespace amber::observe {
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
  double intensity_cd;
  float radius_m, source_radius_m;
};
struct ZoneAmbient {
  Handle zone;
  double ambient_lux;
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
void light_samples(const Lights &, IdentityRead, Vec3 point, std::vector<space::LightSample> &);
double ambient_at(const ZoneAmbients &, std::span<const Handle>, double world_lux);
double illuminate(const space::Space &, IdentityRead, Handle receiver, Vec3, double ambient_lux,
                  std::span<const space::LightSample>);
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
  std::vector<Seen> things, far;
  std::vector<Handle> zones;
};
Scan scan(const amber::ObjectState &objects_state, const Observer &, Handle held, const Settings &,
          amber::space::State &space_state, double world_ambient_lux, std::size_t requested_limit);
struct Sound {
  Handle source;
  Vec3 at;
  float radius_m;
  EventPayload payload;
  PlayerId speaker;
  amber::Origin origin;
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
std::vector<Heard> take_heard(Hearing &, std::uint64_t, std::size_t limit, std::size_t &omitted);
void remove_listener(Hearing &, std::uint64_t);
} // namespace amber::observe

namespace amber::space {
struct State {
  Space space;
  PhysicalRelations physical_relations;
  std::vector<RelationChange> relation_changes;
  observe::Hearing hearing;
  std::vector<observe::Listener> listeners;
  observe::Lights lights;
  observe::LightSubscriptions light_subscriptions;
  observe::ZoneAmbients zones;
  observe::ActiveViews active_views;
  std::vector<observe::MotionSample> motion;
  SparseColumn<std::pair<Handle, Vec3>> previous_positions;
  State(const Settings &settings, float light_cell_m)
      : space(settings), lights(light_cell_m), light_subscriptions(light_cell_m),
        active_views(light_cell_m) {}
};
} // namespace amber::space
