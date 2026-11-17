#pragma once

#include <Jolt/Jolt.h>

#include "space.hpp"

#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/SubShapeIDPair.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace amber::space {
inline JPH::Vec3 jv(Vec3 v) {
  return {v.x, v.y, v.z};
}
inline Vec3 av(JPH::Vec3Arg v) {
  return {v.GetX(), v.GetY(), v.GetZ()};
}
inline JPH::Quat jq(Quat q) {
  return {q.x, q.y, q.z, q.w};
}
inline Quat aq(JPH::QuatArg q) {
  return {q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
}
inline Vec3 nav(Vec3 v) {
  return {v.x, v.z, -v.y};
}
inline Vec3 world(Vec3 v) {
  return {v.x, -v.z, v.y};
}
inline Pose compose(Pose a, Pose b) {
  return {a.position + av(jq(a.rotation) * jv(b.position)), aq(jq(a.rotation) * jq(b.rotation))};
}
inline Pose relative(Pose parent, Pose child) {
  auto inverse = jq(parent.rotation).Conjugated();
  return {av(inverse * jv(child.position - parent.position)), aq(inverse * jq(child.rotation))};
}
inline bool overlaps(Bounds a, Bounds b) {
  return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
         a.min.z <= b.max.z && a.max.z >= b.min.z;
}
struct Geometry {
  JPH::ShapeRefC mesh, hull;
  std::vector<Vec3> vertices;
  std::vector<int> indices;
  Bounds bounds;
};
struct Mirror {
  struct MembershipPrediction {
    Step sampled = 0, due = 0;
    Vec3 center, velocity;
    std::uint64_t volumes = 0, path = 0;
    bool reconcile_zones = true, containers_dirty = true;
  } membership;
  BodySpec spec;
  Pose pose;
  JPH::BodyID body;
  std::unique_ptr<JPH::CharacterVirtual> character;
  int agent = -1;
  std::uint64_t intent = 0, path_revision = 0;
  std::vector<Vec3> route;
  std::size_t waypoint = 0;
  float stuck_s = 0;
  Vec3 last_goal;
  PathStatus path_status = PathStatus::NoSurface;
  std::optional<Pose> kinematic_start;
  std::vector<Handle> compound_parts;
  struct OwnGeometry {
    BodySpec spec;
    JPH::BodyID query_body;
  };
  std::optional<OwnGeometry> own_geometry;
};
struct Volume {
  VolumeSpec spec;
  Pose world;
  JPH::BodyID body;
};
struct NavInstance {
  std::shared_ptr<const Geometry> geometry;
  Pose pose;
  bool walkable;
};
struct NavJob {
  Tile tile;
  std::uint64_t revision;
  std::vector<NavInstance> instances;
};
struct NavResult {
  Tile tile;
  std::uint64_t revision;
  std::vector<unsigned char> bytes;
  std::string error;
};
class Layers final : public JPH::BroadPhaseLayerInterface {
public:
  JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
    return JPH::BroadPhaseLayer(layer == 0 ? 0 : 1);
  }
};
class BroadFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
  bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad) const override {
    return layer == 4 ? broad.GetValue() == 1 : layer != 0 || broad.GetValue() != 0;
  }
};
class PairFilter final : public JPH::ObjectLayerPairFilter {
public:
  bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
    if (a == 4 || b == 4)
      return a == 2 || b == 2;
    return !(a == 0 && b == 0) && !(a == 2 && b == 2);
  }
};
struct CachedContact : Contact {
  std::optional<VolumeKind> sensor;
};
struct Space::Native {
  Settings settings;
  Layers layers;
  BroadFilter broad_filter;
  PairFilter pair_filter;
  JPH::PhysicsSystem physics;
  JPH::TempAllocatorImpl temp;
  JPH::JobSystemSingleThreaded jobs;
  JPH::CharacterVsCharacterCollisionSimple characters;
  std::vector<std::shared_ptr<Geometry>> geometries{nullptr};
  std::shared_ptr<Geometry> terrain_geometry;
  std::map<std::uint64_t, Mirror> bodies;
  std::set<std::uint64_t> character_handles, logical_roots;
  std::vector<Volume> volumes;
  std::map<std::uint64_t, Attachment> attachments;
  std::unordered_map<std::uint64_t, std::uint64_t> compound_parent;
  std::unordered_map<std::uint64_t, GeometryId> compound_geometry;
  std::vector<GeometryId> free_compound_geometry;
  std::set<std::uint64_t> dirty_objects;
  std::vector<Bounds> dirty_bounds;
  std::vector<Contact> contacts;
  std::vector<RelationChange> zone_changes;
  std::unordered_map<std::uint32_t, Handle> handles;
  struct Listener final : JPH::ContactListener {
    Native &state;
    std::map<JPH::SubShapeIDPair, CachedContact> pairs;
    explicit Listener(Native &n) : state(n) {}
    void add(const JPH::Body &, const JPH::Body &, const JPH::ContactManifold &, bool);
    void OnContactAdded(const JPH::Body &, const JPH::Body &, const JPH::ContactManifold &,
                        JPH::ContactSettings &) override;
    void OnContactPersisted(const JPH::Body &, const JPH::Body &, const JPH::ContactManifold &,
                            JPH::ContactSettings &) override;
    void OnContactRemoved(const JPH::SubShapeIDPair &) override;
  } listener;
  struct CharacterListener final : JPH::CharacterContactListener {
    Native &state;
    using Key = std::tuple<std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t>;
    std::map<Key, CachedContact> pairs;
    explicit CharacterListener(Native &n) : state(n) {}
    void add(const JPH::CharacterVirtual *, const JPH::CharacterContact &, bool);
    void remove(const JPH::CharacterVirtual *, const JPH::BodyID &, const JPH::CharacterID &,
                const JPH::SubShapeID &);
    void OnContactAdded(const JPH::CharacterVirtual *, const JPH::CharacterContact &,
                        JPH::CharacterContactSettings &) override;
    void OnContactPersisted(const JPH::CharacterVirtual *, const JPH::CharacterContact &,
                            JPH::CharacterContactSettings &) override;
    void OnContactRemoved(const JPH::CharacterVirtual *, const JPH::BodyID &,
                          const JPH::SubShapeID &) override;
    void OnCharacterContactAdded(const JPH::CharacterVirtual *, const JPH::CharacterContact &,
                                 JPH::CharacterContactSettings &) override;
    void OnCharacterContactPersisted(const JPH::CharacterVirtual *, const JPH::CharacterContact &,
                                     JPH::CharacterContactSettings &) override;
    void OnCharacterContactRemoved(const JPH::CharacterVirtual *, const JPH::CharacterID &,
                                   const JPH::SubShapeID &) override;
  } character_listener;
  dtNavMesh *mesh = nullptr;
  dtNavMeshQuery *query = nullptr;
  dtCrowd *crowd = nullptr;
  std::uint64_t revision = 0, geometry_revision = 0;
  struct Connectivity {
    std::uint64_t revision = std::numeric_limits<std::uint64_t>::max();
    std::unordered_map<dtPolyRef, std::uint32_t> labels;
    std::deque<dtPolyRef> frontier;
    std::uint32_t component = 0;
    int tile = 0, polygon = 0;
    bool complete = false;
  } connectivity;
  std::uint64_t volume_revision = 1;
  std::map<Tile, std::uint64_t> dirty_tiles, scheduled_tiles;
  std::mutex nav_mutex;
  std::condition_variable nav_condition;
  std::deque<NavJob> nav_jobs;
  std::deque<NavResult> nav_results;
  bool stopping = false;
  std::vector<std::thread> nav_workers;
  explicit Native(const Settings &);
  ~Native();
  Bounds world_bounds(const Mirror &) const;
  Bounds world_bounds(const Mirror &, Pose) const;
  void changed(Handle, Bounds old, Bounds current, bool navigation);
  void dirty_navigation(Bounds);
  void wake(Bounds);
};
NavResult build_tile(const NavJob &, const NavSettings &);
void start_navigation_workers(Space::Native &);
void stop_navigation_workers(Space::Native &);
bool travel_blocked(Vec3 from, Vec3 to, std::span<const TravelBoundary>);
void edge(Edges &, RelationKind, Handle, Handle, bool, std::vector<RelationChange> &);
void erase_edges(Edges &, RelationKind, Handle, std::vector<RelationChange> &);
OperationResult update_motion(Space::Native &, Mirror &, Motion);
JPH::ObjectLayer layer(Motion);
Bounds shape_bounds(const JPH::Shape *, Pose);
bool navigates(const Mirror &);
void native_pose(Space::Native &, Mirror &, Pose);
JPH::BodyID create_rigid(Space::Native &, const Mirror &, Motion);
void destroy_rigid(Space::Native &, JPH::BodyID);
void query_shape(Space::Native &, Mirror &, bool query);
void extract_native(Space::Native &, Handle);
void remove_compound_member(Space &, IdentityRead, SpaceFlags, Handle);
void flags_for(SpaceFlags, Handle, Motion, bool held = false);
void sensor_contact_changed(Space::Native &, const CachedContact &, bool added);
bool has_sensor_contact(const Space::Native &, const CachedContact &);
OperationResult can_release_occupants(const Space &, Handle, std::span<const Handle>,
                                      bool retiring_body, bool stay_static = false);
OperationResult release_occupants(Space &, IdentityRead, Handle, std::span<const Handle>,
                                  ParentWrite, SpaceFlags, PhysicalRelations &,
                                  std::vector<RelationChange> &, bool stay_static = false);
} // namespace amber::space
