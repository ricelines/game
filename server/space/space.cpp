#include "private.hpp"

#include <tracy/Tracy.hpp>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/StreamIn.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>
#include <unordered_set>

namespace amber::space {
const char *status_text(Status status) {
  constexpr const char *names[]{
      "done",       "stale_handle",     "missing_geometry", "missing_body", "unsupported_motion",
      "obstructed", "already_parented", "attachment_cycle", "not_attached", "capacity"};
  return names[static_cast<unsigned>(status)];
}
namespace {
struct JoltTypes {
  JoltTypes() {
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory;
    JPH::RegisterTypes();
  }
  ~JoltTypes() {
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
  }
};
void initialize_jolt() {
  static JoltTypes types;
}
class Bytes final : public JPH::StreamIn {
  std::span<const std::byte> input;
  std::size_t offset = 0;
  bool failed = false;

public:
  explicit Bytes(std::span<const std::byte> bytes) : input(bytes) {}
  void ReadBytes(void *out, std::size_t count) override {
    if (count > input.size() - offset) {
      failed = true;
      std::memset(out, 0, count);
      return;
    }
    std::memcpy(out, input.data() + offset, count);
    offset += count;
  }
  bool IsEOF() const override { return failed; }
  bool IsFailed() const override { return failed; }
};
JPH::ShapeRefC decode(std::span<const std::byte> bytes) {
  if (bytes.empty())
    return nullptr;
  Bytes input(bytes);
  JPH::Shape::IDToShapeMap shapes;
  JPH::Shape::IDToMaterialMap materials;
  auto shape = JPH::Shape::sRestoreWithChildren(input, shapes, materials);
  if (input.IsFailed() || shape.HasError())
    throw std::runtime_error(shape.HasError() ? shape.GetError().c_str() : "truncated Jolt shape");
  return shape.Get();
}
std::uint32_t u32(std::span<const std::byte> bytes, std::size_t at) {
  std::uint32_t value;
  std::memcpy(&value, bytes.data() + at, sizeof(value));
  static_assert(std::endian::native == std::endian::little);
  return value;
}
} // namespace
JPH::ObjectLayer layer(Motion mode) {
  return mode == Motion::Static ? 0 : 1;
}
Bounds shape_bounds(const JPH::Shape *shape, Pose pose) {
  const auto world = JPH::RMat44::sRotationTranslation(jq(pose.rotation), jv(pose.position))
                         .PreTranslated(shape->GetCenterOfMass());
  auto bounds = shape->GetWorldSpaceBounds(world, JPH::Vec3::sReplicate(1));
  return {av(bounds.mMin), av(bounds.mMax)};
}

bool navigates(const Mirror &body) {
  return body.spec.collision_solid &&
         (body.spec.navigation_surface || body.spec.navigation_obstacle);
}
void native_pose(Space::Native &n, Mirror &body, Pose pose) {
  if ((pose.position != body.pose.position || pose.rotation != body.pose.rotation) &&
      (body.spec.motion == Motion::Static || n.compound_parent.contains(body.spec.object.bits) ||
       (body.character && n.attachments.contains(body.spec.object.bits)))) {
    // Static query parts and attached virtual characters do not run Jolt's
    // ordinary moving-body/virtual-character sensor contact update.
    body.membership.reconcile_zones = true;
    body.membership.containers_dirty = true;
  }
  if (body.character) {
    body.character->SetPosition(jv(pose.position));
    body.character->SetRotation(jq(pose.rotation));
  } else
    n.physics.GetBodyInterface().SetPositionAndRotation(
        body.body, jv(pose.position), jq(pose.rotation), JPH::EActivation::Activate);
  if (body.own_geometry)
    n.physics.GetBodyInterface().SetPositionAndRotation(body.own_geometry->query_body,
                                                        jv(pose.position), jq(pose.rotation),
                                                        JPH::EActivation::DontActivate);
  body.pose = pose;
}
JPH::BodyID create_rigid(Space::Native &n, const Mirror &body, Motion motion) {
  const auto &geometry = *n.geometries[body.spec.geometry];
  const JPH::Shape *shape =
      motion == Motion::Static ? geometry.mesh.GetPtr() : geometry.hull.GetPtr();
  if (!shape)
    return {};
  auto type = motion == Motion::Static    ? JPH::EMotionType::Static
              : motion == Motion::Dynamic ? JPH::EMotionType::Dynamic
                                          : JPH::EMotionType::Kinematic;
  JPH::BodyCreationSettings settings(shape, jv(body.pose.position), jq(body.pose.rotation), type,
                                     body.spec.collision_solid ? layer(motion) : 4);
  settings.mUserData = body.spec.object.bits;
  settings.mFriction = body.spec.surface.friction;
  settings.mRestitution = body.spec.surface.restitution;
  settings.mCollideKinematicVsNonDynamic = motion == Motion::Kinematic;
  if (motion != Motion::Static) {
    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    settings.mMassPropertiesOverride.mMass = body.spec.mass_kg;
    settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
  }
  return n.physics.GetBodyInterface().CreateAndAddBody(
      settings,
      motion == Motion::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
}
void destroy_rigid(Space::Native &n, JPH::BodyID id) {
  if (id.IsInvalid())
    return;
  n.handles.erase(id.GetIndexAndSequenceNumber());
  n.physics.GetBodyInterface().RemoveBody(id);
  n.physics.GetBodyInterface().DestroyBody(id);
}
void query_shape(Space::Native &n, Mirror &body, bool query) {
  auto &bodies = n.physics.GetBodyInterface();
  const auto &geometry = *n.geometries[body.spec.geometry];
  if (query) {
    bodies.SetMotionType(body.body, JPH::EMotionType::Static, JPH::EActivation::DontActivate);
    bodies.SetShape(body.body, geometry.mesh, false, JPH::EActivation::DontActivate);
    bodies.SetObjectLayer(body.body, 4);
  } else {
    const auto shape = body.spec.motion == Motion::Static ? geometry.mesh : geometry.hull;
    bodies.SetShape(body.body, shape, false, JPH::EActivation::DontActivate);
    bodies.SetMotionType(body.body,
                         body.spec.motion == Motion::Static    ? JPH::EMotionType::Static
                         : body.spec.motion == Motion::Dynamic ? JPH::EMotionType::Dynamic
                                                               : JPH::EMotionType::Kinematic,
                         JPH::EActivation::DontActivate);
    bodies.SetObjectLayer(body.body, body.spec.collision_solid ? layer(body.spec.motion) : 4);
  }
}
void extract_native(Space::Native &n, Handle object) {
  // CharacterVirtual's private contact cache does not notify our listener when
  // the character itself is destroyed. Retire those derived pairs explicitly.
  std::erase_if(n.character_listener.pairs, [&](const auto &entry) {
    const auto &contact = entry.second;
    if (contact.first != object && contact.second != object)
      return false;
    if (!contact.sensor) {
      auto removed = contact;
      removed.started = false;
      n.contacts.push_back(removed);
    }
    return true;
  });
  n.character_handles.erase(object.bits);
  auto it = n.bodies.find(object.bits);
  if (it != n.bodies.end()) {
    auto old = n.world_bounds(it->second);
    for (auto child : it->second.compound_parts) {
      n.compound_parent.erase(child.bits);
      if (auto found = n.bodies.find(child.bits); found != n.bodies.end()) {
        query_shape(n, found->second, false);
        if (navigates(found->second))
          n.dirty_navigation(n.world_bounds(found->second));
      }
    }
    n.compound_parent.erase(object.bits);
    if (it->second.agent >= 0)
      n.crowd->removeAgent(it->second.agent);
    if (it->second.character) {
      n.handles.erase(it->second.body.GetIndexAndSequenceNumber());
      it->second.character.reset();
    } else
      destroy_rigid(n, it->second.body);
    if (it->second.own_geometry)
      destroy_rigid(n, it->second.own_geometry->query_body);
    bool nav = navigates(it->second);
    n.bodies.erase(it);
    n.dirty_bounds.push_back(old);
    n.wake(old);
    if (nav)
      n.dirty_navigation(old);
  }
}

namespace {
Mirror *get(Space::Native &n, IdentityRead identity, Handle object) {
  if (!identity.valid(object))
    return nullptr;
  auto found = n.bodies.find(object.bits);
  return found == n.bodies.end() ? nullptr : &found->second;
}
class ObjectFilter final : public JPH::ObjectLayerFilter {
  bool sensors;

public:
  explicit ObjectFilter(bool value) : sensors(value) {}
  bool ShouldCollide(JPH::ObjectLayer l) const override { return sensors || l != 2; }
};
class BodyFilter final : public JPH::BodyFilter {
  const Space::Native &n;
  QueryFilter filter;
  bool visible_geometry;

public:
  BodyFilter(const Space::Native &state, QueryFilter f, bool visible = false)
      : n(state), filter(f), visible_geometry(visible) {}
  bool ShouldCollide(const JPH::BodyID &id) const override {
    auto found = n.handles.find(id.GetIndexAndSequenceNumber());
    if (found != n.handles.end() && visible_geometry) {
      auto body = n.bodies.find(found->second.bits);
      if (body != n.bodies.end() && !body->second.compound_parts.empty() && body->second.body == id)
        return false;
    }
    return found == n.handles.end() ||
           (found->second != filter.ignore_a && found->second != filter.ignore_b &&
            std::ranges::find(filter.ignore, found->second) == filter.ignore.end());
  }
};
class Rays final : public JPH::CastRayCollector {
public:
  std::vector<JPH::RayCastResult> hits;
  RunDeadline deadline;
  explicit Rays(RunDeadline until) : deadline(until) {}
  void AddHit(const JPH::RayCastResult &hit) override {
    if (deadline.passed())
      ForceEarlyOut();
    else
      hits.push_back(hit);
  }
};
std::vector<Hit> hits(const Space &space, IdentityRead identity, Vec3 from, Vec3 to,
                      QueryFilter filter) {
  auto &n = *space.native;
  Rays collector(filter.deadline);
  ObjectFilter objects(filter.include_sensors);
  BodyFilter bodies(n, filter, true);
  n.physics.GetNarrowPhaseQuery().CastRay({jv(from), jv(to - from)}, JPH::RayCastSettings{},
                                          collector, {}, objects, bodies);
  std::sort(collector.hits.begin(), collector.hits.end(),
            [](auto a, auto b) { return a.mFraction < b.mFraction; });
  std::vector<Hit> out;
  for (const auto &hit : collector.hits) {
    if (filter.deadline.passed())
      break;
    auto owner = n.handles.find(hit.mBodyID.GetIndexAndSequenceNumber());
    if (owner == n.handles.end() || !identity.valid(owner->second))
      continue;
    auto position = from + (to - from) * hit.mFraction;
    Vec3 normal;
    JPH::BodyLockRead lock(n.physics.GetBodyLockInterface(), hit.mBodyID);
    if (lock.Succeeded())
      normal = av(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, jv(position)));
    out.push_back({owner->second, hit.mFraction, position, normal});
  }
  return out;
}
void erase_value(SparseColumn<std::vector<Handle>> &column, Handle key, Handle value) {
  auto *values = column.get(key.slot());
  if (!values)
    return;
  std::erase(*values, value);
  if (values->empty())
    column.erase(key.slot());
}
void set_out(Edges &table, RelationKind kind, Handle from, std::span<const Handle> wanted,
             std::vector<RelationChange> &changes) {
  auto *current = table.out.get(from.slot());
  auto old = current ? *current : std::vector<Handle>{};
  for (auto target : old)
    if (std::find(wanted.begin(), wanted.end(), target) == wanted.end())
      edge(table, kind, from, target, false, changes);
  for (auto target : wanted)
    edge(table, kind, from, target, true, changes);
}
bool point_in(const Geometry &geometry, Pose pose, Vec3 point) {
  JPH::AllHitCollisionCollector<JPH::CollidePointCollector> collector;
  const auto local = relative(pose, {point, {}}).position;
  geometry.mesh->CollidePoint(jv(local) - geometry.mesh->GetCenterOfMass(), {}, collector);
  return collector.HadHit();
}
} // namespace
void flags_for(SpaceFlags flags, Handle h, Motion motion, bool held) {
  auto bits = motion == Motion::Dynamic     ? Dynamic
              : motion == Motion::Kinematic ? ScriptedMotion
              : motion == Motion::Static    ? Static
                                            : 0u;
  flags.set(h.slot(), bits | (held ? Held : 0u));
}
namespace {
JPH::ShapeRefC character_shape(float radius_m, float height_m) {
  auto capsule = JPH::Ref<JPH::Shape>(new JPH::CapsuleShape(height_m * .5f - radius_m, radius_m));
  return new JPH::RotatedTranslatedShape(
      {0, 0, height_m * .5f},
      JPH::Quat::sRotation(JPH::Vec3::sAxisX(), std::numbers::pi_v<float> * .5f), capsule);
}
class SolidObjects final : public JPH::ObjectLayerFilter {
  bool characters;

public:
  explicit SolidObjects(bool include_characters = true) : characters(include_characters) {}
  bool ShouldCollide(JPH::ObjectLayer l) const override {
    return l != 2 && l != 4 && (characters || l != 3);
  }
};
std::vector<Handle> overlapping_zones(Space::Native &n, IdentityRead identity, const Mirror &body) {
  class Sensors final : public JPH::ObjectLayerFilter {
    bool ShouldCollide(JPH::ObjectLayer layer) const override { return layer == 2; }
  } sensors;
  const auto shape = body.character ? body.character->GetShape()
                                    : n.physics.GetBodyInterface().GetShape(body.body).GetPtr();
  const auto transform =
      JPH::RMat44::sRotationTranslation(jq(body.pose.rotation), jv(body.pose.position))
          .PreTranslated(shape->GetCenterOfMass());
  JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> hits;
  n.physics.GetNarrowPhaseQuery().CollideShape(shape, JPH::Vec3::sReplicate(1), transform,
                                               JPH::CollideShapeSettings{}, JPH::RVec3::sZero(),
                                               hits, {}, sensors);
  std::vector<Handle> zones;
  for (const auto &hit : hits.mHits) {
    const auto volume = std::ranges::find(n.volumes, hit.mBodyID2, &Volume::body);
    if (volume != n.volumes.end() && volume->spec.kind == VolumeKind::Zone &&
        volume->spec.owner != body.spec.object && identity.valid(volume->spec.owner))
      zones.push_back(volume->spec.owner);
  }
  std::sort(zones.begin(), zones.end());
  zones.erase(std::unique(zones.begin(), zones.end()), zones.end());
  return zones;
}
void membership(Space &space, IdentityRead identity, ParentWrite parents,
                PhysicalRelations &relations, std::vector<RelationChange> &changes, Step now,
                float seconds) {
  auto &n = *space.native;
  for (const auto &change : n.zone_changes)
    if (identity.valid(change.from) && identity.valid(change.to))
      edge(relations.in_zone, RelationKind::InZone, change.from, change.to, change.added, changes);
  n.zone_changes.clear();
  for (auto bits : n.dirty_objects) {
    Handle object{bits};
    if (!identity.valid(object))
      continue;
    auto found = n.bodies.find(bits);
    if (found == n.bodies.end())
      continue;
    auto &body = found->second;
    auto &predicted = body.membership;
    const bool reconcile = predicted.reconcile_zones;
    if (reconcile) {
      // New/rebuilt/explicitly relocated static geometry has no historical
      // sensor contacts. Ordinary motion uses the contact changes above.
      set_out(relations.in_zone, RelationKind::InZone, object, overlapping_zones(n, identity, body),
              changes);
      predicted.reconcile_zones = false;
    }
    if (!predicted.containers_dirty)
      continue;
    const auto center = body.character
                            ? body.pose.position + Vec3{0, 0, body.spec.character->height_m * .5f}
                            : av(n.physics.GetBodyInterface().GetCenterOfMassPosition(body.body));
    if (!reconcile && predicted.volumes == n.volume_revision && center == predicted.center) {
      predicted.containers_dirty = false;
      continue;
    }
    const bool following = body.character && body.intent && body.waypoint < body.route.size() &&
                           !n.attachments.contains(bits);
    if (!reconcile && following && predicted.volumes == n.volume_revision && now < predicted.due) {
      const auto expected =
          predicted.center + predicted.velocity * (float(now - predicted.sampled) * seconds);
      if ((expected - center).length_squared() <=
          n.settings.contact_slop_m * n.settings.contact_slop_m)
        continue;
    }
    std::vector<Handle> containers;
    volumes_at(space, identity, center, VolumeKind::Container, containers);
    std::erase(containers, object);
    float smallest = INFINITY;
    Handle immediate;
    for (auto container : containers) {
      const auto volume = std::ranges::find_if(n.volumes, [&](const Volume &v) {
        return v.spec.owner == container && v.spec.kind == VolumeKind::Container;
      });
      const auto &bounds = n.geometries[volume->spec.geometry]->bounds;
      const auto size = bounds.max - bounds.min;
      const auto size3 = size.x * size.y * size.z;
      if (size3 < smallest) {
        smallest = size3;
        immediate = container;
      }
    }
    auto *prior = relations.contains.in.get(object.slot());
    const auto previous = prior ? *prior : std::vector<Handle>{};
    for (auto container : previous)
      if (std::find(containers.begin(), containers.end(), container) == containers.end())
        edge(relations.contains, RelationKind::Contains, container, object, false, changes);
    for (auto container : containers)
      if (std::find(previous.begin(), previous.end(), container) == previous.end())
        edge(relations.contains, RelationKind::Contains, container, object, true, changes);
    if (!n.attachments.contains(bits))
      parents.parent[object.slot()] = immediate;
    predicted = {now, 0, center, {}, n.volume_revision, false, false};
    if (following) {
      const auto velocity = av(body.character->GetLinearVelocity());
      const float speed = velocity.length();
      if (speed > n.settings.contact_slop_m / seconds) {
        const float duration = (body.route[body.waypoint] - body.pose.position).length() / speed;
        float boundary = duration;
        for (const auto &volume : n.volumes) {
          if (volume.spec.kind != VolumeKind::Container)
            continue;
          const auto *shape = n.geometries[volume.spec.geometry]->mesh.GetPtr();
          const auto inverse = jq(volume.world.rotation).Conjugated();
          const auto from = inverse * jv(center - volume.world.position) - shape->GetCenterOfMass();
          const auto direction = inverse * jv(velocity * duration);
          JPH::AllHitCollisionCollector<JPH::CastRayCollector> intersections;
          JPH::RayCastSettings settings;
          settings.SetBackFaceMode(JPH::EBackFaceMode::CollideWithBackFaces);
          settings.mTreatConvexAsSolid = false;
          shape->CastRay({from, direction}, settings, {}, intersections);
          for (const auto &hit : intersections.mHits)
            if (hit.mFraction > std::numeric_limits<float>::epsilon())
              boundary = std::min(boundary, hit.mFraction * duration);
        }
        predicted.due = now + std::max<Step>(1, Step(std::ceil(boundary / seconds)));
        predicted.velocity = velocity;
      }
    }
  }
}
} // namespace

Space::Native::Native(const Settings &s)
    : settings(s), temp(s.physics_temp_bytes), jobs(JPH::cMaxPhysicsJobs), listener(*this),
      character_listener(*this) {
  initialize_jolt();
  physics.Init(s.max_bodies, 0, s.max_body_pairs, s.max_contacts, layers, broad_filter,
               pair_filter);
  physics.SetGravity(jv(s.gravity_m_s2));
  physics.SetContactListener(&listener);
  mesh = dtAllocNavMesh();
  query = dtAllocNavMeshQuery();
  crowd = dtAllocCrowd();
  dtNavMeshParams params{};
  params.tileWidth = s.navigation.cell_m * s.navigation.tile_cells;
  params.tileHeight = params.tileWidth;
  params.maxTiles = int(s.navigation.max_tiles);
  params.maxPolys = int(s.navigation.polygons_per_tile);
  if (dtStatusFailed(mesh->init(&params)) ||
      dtStatusFailed(query->init(mesh, int(s.navigation.nodes_per_query))) ||
      !crowd->init(int(s.max_characters), s.navigation.radius_m, mesh))
    throw std::runtime_error("initialize Detour");
  start_navigation_workers(*this);
}
Space::Native::~Native() {
  stop_navigation_workers(*this);
  for (auto &[key, body] : bodies) {
    (void)key;
    if (body.character) {
      body.character.reset();
    } else
      destroy_rigid(*this, body.body);
    if (body.own_geometry)
      destroy_rigid(*this, body.own_geometry->query_body);
  }
  for (auto &volume : volumes)
    destroy_rigid(*this, volume.body);
  JPH::BodyIDVector remaining;
  physics.GetBodies(remaining);
  for (auto id : remaining)
    destroy_rigid(*this, id);
  dtFreeCrowd(crowd);
  dtFreeNavMeshQuery(query);
  dtFreeNavMesh(mesh);
}
Vec3 transform_point(Pose pose, Vec3 local) {
  return pose.position + av(jq(pose.rotation) * jv(local));
}

Space::Space(const Settings &s) {
  initialize_jolt();
  native = std::make_unique<Native>(s);
}
Space::~Space() = default;
void add_terrain(State &state, std::int32_t i, std::int32_t j) {
  if (state.terrain.contains({i, j}))
    return;
  auto &n = *state.space.native;
  const auto size = n.settings.terrain_tile_m;
  if (!n.terrain_geometry) {
    n.terrain_geometry = std::make_shared<Geometry>();
    n.terrain_geometry->vertices = {{0, 0, 0}, {size, 0, 0}, {size, size, 0}, {0, size, 0}};
    n.terrain_geometry->indices = {0, 1, 2, 0, 2, 3};
    n.terrain_geometry->bounds = {{0, 0, 0}, {size, size, 0}};
  }
  JPH::BodyCreationSettings body(new JPH::BoxShape({size / 2, size / 2, .5f}),
                                 jv({(i + .5f) * size, (j + .5f) * size, -.5f}),
                                 JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0);
  body.mUserData = 0;
  const auto id =
      n.physics.GetBodyInterface().CreateAndAddBody(body, JPH::EActivation::DontActivate);
  if (id.IsInvalid())
    throw std::runtime_error("terrain body capacity exhausted");
  state.terrain.emplace(i, j);
  n.dirty_navigation({{i * size, j * size, 0}, {(i + 1) * size, (j + 1) * size, 0}});
}
void ensure_terrain(State &state, const std::array<float, 4> &bounds) {
  const auto size = state.space.native->settings.terrain_tile_m;
  const auto x0 = int(std::floor(bounds[0] / size));
  const auto y0 = int(std::floor(bounds[1] / size));
  const auto x1 = int(std::ceil(bounds[2] / size));
  const auto y1 = int(std::ceil(bounds[3] / size));
  for (auto i = x0; i < x1; ++i)
    for (auto j = y0; j < y1; ++j)
      add_terrain(state, i, j);
}
void Space::Native::Listener::OnContactAdded(const JPH::Body &a, const JPH::Body &b,
                                             const JPH::ContactManifold &m,
                                             JPH::ContactSettings &) {
  add(a, b, m, true);
}
void sensor_contact_changed(Space::Native &n, const CachedContact &contact, bool added) {
  if (contact.sensor == VolumeKind::Zone)
    n.zone_changes.push_back({RelationKind::InZone, contact.first, contact.second, added});
  else if (auto body = n.bodies.find(contact.first.bits); body != n.bodies.end()) {
    body->second.membership.containers_dirty = true;
    n.dirty_objects.insert(contact.first.bits);
  }
}
bool has_sensor_contact(const Space::Native &n, const CachedContact &contact) {
  auto matches = [&](const auto &pair) {
    const auto &other = pair.second;
    return other.sensor == contact.sensor && other.first == contact.first &&
           other.second == contact.second;
  };
  return std::any_of(n.listener.pairs.begin(), n.listener.pairs.end(), matches) ||
         std::any_of(n.character_listener.pairs.begin(), n.character_listener.pairs.end(), matches);
}
void Space::Native::CharacterListener::add(const JPH::CharacterVirtual *character,
                                           const JPH::CharacterContact &hit, bool started) {
  // Jolt calls this from actual contact resolution/support; some paths set
  // mHadCollision immediately after the callback rather than before it.
  const Handle first{character->GetUserData()}, second{hit.mUserData};
  if (!first || !second || first == second)
    return;
  const Key key{first.bits, hit.mBodyB.GetIndexAndSequenceNumber(), hit.mSubShapeIDB.GetValue()};
  if (hit.mIsSensorB) {
    auto found = pairs.find(key);
    if (found == pairs.end()) {
      const auto volume = std::ranges::find(state.volumes, hit.mBodyB, &Volume::body);
      if (volume == state.volumes.end())
        return;
      CachedContact contact{{first, second, {}, {}, true}, volume->spec.kind};
      pairs.emplace(key, contact);
      sensor_contact_changed(state, contact, true);
    } else if (found->second.sensor == VolumeKind::Container) {
      const auto subject_body = state.bodies.find(first.bits);
      if (subject_body != state.bodies.end())
        subject_body->second.membership.containers_dirty = true;
    }
    return;
  }
  Contact contact{first, second, av(hit.mPosition), av(-hit.mContactNormal), started};
  pairs[key] = {contact, {}};
  if (started)
    state.contacts.push_back(contact);
}
void Space::Native::CharacterListener::OnContactRemoved(const JPH::CharacterVirtual *character,
                                                        const JPH::BodyID &body,
                                                        const JPH::SubShapeID &subshape) {
  const Key key{character->GetUserData(), body.GetIndexAndSequenceNumber(), subshape.GetValue()};
  const auto found = pairs.find(key);
  if (found == pairs.end())
    return;
  auto contact = found->second;
  pairs.erase(found);
  if (contact.sensor) {
    if (!has_sensor_contact(state, contact))
      sensor_contact_changed(state, contact, false);
    return;
  }
  contact.started = false;
  state.contacts.push_back(contact);
}
void Space::Native::CharacterListener::OnContactAdded(const JPH::CharacterVirtual *c,
                                                      const JPH::CharacterContact &hit,
                                                      JPH::CharacterContactSettings &) {
  add(c, hit, true);
}
void Space::Native::CharacterListener::OnContactPersisted(const JPH::CharacterVirtual *c,
                                                          const JPH::CharacterContact &hit,
                                                          JPH::CharacterContactSettings &) {
  add(c, hit, false);
}
void Space::Native::Listener::add(const JPH::Body &a, const JPH::Body &b,
                                  const JPH::ContactManifold &m, bool started) {
  if (!a.GetUserData() || !b.GetUserData())
    return;
  const JPH::SubShapeIDPair key{a.GetID(), m.mSubShapeID1, b.GetID(), m.mSubShapeID2};
  if (a.IsSensor() || b.IsSensor()) {
    const auto &sensor = a.IsSensor() ? a : b;
    const auto &subject = a.IsSensor() ? b : a;
    if (sensor.GetUserData() == subject.GetUserData())
      return;
    // Free characters use CharacterVirtual's callbacks; attached characters
    // skip that update and are detected through their inner rigid body.
    if (state.character_handles.contains(subject.GetUserData()) &&
        !state.attachments.contains(subject.GetUserData()))
      return;
    auto found = pairs.find(key);
    if (found == pairs.end()) {
      const auto volume = std::ranges::find(state.volumes, sensor.GetID(), &Volume::body);
      if (volume == state.volumes.end())
        return;
      CachedContact contact{{{subject.GetUserData()}, {sensor.GetUserData()}, {}, {}, true},
                            volume->spec.kind};
      pairs.emplace(key, contact);
      sensor_contact_changed(state, contact, true);
    } else if (found->second.sensor == VolumeKind::Container) {
      const auto subject_body = state.bodies.find(found->second.first.bits);
      if (subject_body != state.bodies.end())
        subject_body->second.membership.containers_dirty = true;
    }
    return;
  }
  Contact contact{{a.GetUserData()},
                  {b.GetUserData()},
                  av(m.GetWorldSpaceContactPointOn1(0)),
                  av(m.mWorldSpaceNormal),
                  started};
  pairs[key] = {contact, {}};
  if (started)
    state.contacts.push_back(contact);
}
void Space::Native::Listener::OnContactRemoved(const JPH::SubShapeIDPair &ids) {
  auto it = pairs.find(ids);
  if (it != pairs.end()) {
    auto contact = it->second;
    pairs.erase(it);
    if (contact.sensor) {
      if (!has_sensor_contact(state, contact))
        sensor_contact_changed(state, contact, false);
      return;
    }
    contact.started = false;
    state.contacts.push_back(contact);
  }
}
void Space::Native::Listener::OnContactPersisted(const JPH::Body &a, const JPH::Body &b,
                                                 const JPH::ContactManifold &manifold,
                                                 JPH::ContactSettings &) {
  add(a, b, manifold, false);
}
Bounds Space::Native::world_bounds(const Mirror &body) const {
  return world_bounds(body, body.pose);
}
Bounds Space::Native::world_bounds(const Mirror &body, Pose pose) const {
  const auto &shape = body.character ? body.character->GetShape()
                      : body.spec.motion == Motion::Static
                          ? geometries[body.spec.geometry]->mesh.GetPtr()
                          : geometries[body.spec.geometry]->hull.GetPtr();
  return shape_bounds(shape, pose);
}
void Space::Native::wake(Bounds region) {
  JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> candidates;
  physics.GetBroadPhaseQuery().CollideAABox({jv(region.min), jv(region.max)}, candidates);
  for (auto id : candidates.mHits) {
    auto owner = handles.find(id.GetIndexAndSequenceNumber());
    if (owner == handles.end())
      continue;
    auto found = bodies.find(owner->second.bits);
    if (found == bodies.end())
      continue;
    auto &body = found->second;
    dirty_objects.insert(found->first);
    body.membership = {};
    if (!body.character && body.spec.motion != Motion::Static)
      physics.GetBodyInterface().ActivateBody(body.body);
  }
}
void Space::Native::changed(Handle object, Bounds old, Bounds current, bool navigation) {
  dirty_objects.insert(object.bits);
  dirty_bounds.push_back(old);
  dirty_bounds.push_back(current);
  auto body = bodies.find(object.bits);
  if (body == bodies.end() || body->second.spec.collision_solid) {
    wake(old);
    wake(current);
  }
  if (navigation) {
    dirty_navigation(old);
    dirty_navigation(current);
  }
}
void edge(Edges &table, RelationKind kind, Handle from, Handle to, bool added,
          std::vector<RelationChange> &changes) {
  auto *values = table.out.get(from.slot());
  bool exists = values && std::find(values->begin(), values->end(), to) != values->end();
  if (exists == added)
    return;
  if (added) {
    if (!values)
      values = &table.out.set(from.slot(), std::vector<Handle>{});
    values->push_back(to);
    auto *reverse = table.in.get(to.slot());
    if (!reverse)
      reverse = &table.in.set(to.slot(), std::vector<Handle>{});
    reverse->push_back(from);
  } else {
    erase_value(table.out, from, to);
    erase_value(table.in, to, from);
  }
  changes.push_back({kind, from, to, added});
}
void erase_edges(Edges &table, RelationKind kind, Handle object,
                 std::vector<RelationChange> &changes) {
  const auto *out = table.out.get(object.slot());
  const auto *in = table.in.get(object.slot());
  auto children = out ? *out : std::vector<Handle>{};
  auto parents = in ? *in : std::vector<Handle>{};
  for (auto target : children)
    edge(table, kind, object, target, false, changes);
  for (auto parent : parents)
    edge(table, kind, parent, object, false, changes);
}
ImportResult import_geometry(Space &space, const GeometrySource &source) {
  try {
    auto geometry = std::make_shared<Geometry>();
    geometry->mesh = decode(source.static_jolt);
    geometry->hull = decode(source.convex_jolt);
    geometry->bounds = source.local_bounds;
    if (!geometry->mesh)
      throw std::runtime_error("static collision shape missing");
    auto bytes = source.navigation_mesh;
    if (bytes.size() < 16 || std::memcmp(bytes.data(), "AMBMESH1", 8) != 0)
      throw std::runtime_error("invalid AMBMESH1 header");
    auto vertices = u32(bytes, 8), indices = u32(bytes, 12);
    if (indices % 3 || bytes.size() != 16ull + vertices * 12ull + indices * 4ull)
      throw std::runtime_error("invalid AMBMESH1 length");
    geometry->vertices.resize(vertices);
    std::memcpy(geometry->vertices.data(), bytes.data() + 16, vertices * 12ull);
    geometry->indices.reserve(indices);
    for (std::uint32_t i = 0; i < indices; ++i) {
      auto index = u32(bytes, 16ull + vertices * 12ull + i * 4ull);
      if (index >= vertices)
        throw std::runtime_error("AMBMESH1 index outside vertices");
      geometry->indices.push_back(int(index));
    }
    auto &geometries = space.native->geometries;
    geometries.push_back(std::move(geometry));
    return {GeometryId(geometries.size() - 1), {}};
  } catch (const std::exception &error) {
    return {0, error.what()};
  }
}
GeometryId sensor_box_geometry(Space &space, Vec3 size) {
  if (!(std::isfinite(size.x + size.y + size.z) && size.x > 0 && size.y > 0 && size.z > 0))
    throw std::invalid_argument("sensor box size must be positive finite");
  auto shape = JPH::BoxShapeSettings(jv(size * .5f), 0).Create();
  if (shape.HasError())
    throw std::invalid_argument(shape.GetError().c_str());
  auto geometry = std::make_shared<Geometry>();
  geometry->mesh = shape.Get();
  geometry->hull = shape.Get();
  geometry->bounds = {size * -.5f, size * .5f};
  auto &n = *space.native;
  auto id = GeometryId(n.geometries.size());
  n.geometries.push_back(std::move(geometry));
  return id;
}
Vec3 clear_character_position(const Space &space, Vec3 origin, float radius_m, float height_m) {
  class Obstruction final : public JPH::CollideShapeCollector {
    float slop;

  public:
    explicit Obstruction(float value) : slop(value) {}
    void AddHit(const JPH::CollideShapeResult &hit) override {
      if (hit.mPenetrationDepth > slop)
        ForceEarlyOut();
    }
  };
  const auto &n = *space.native;
  const auto shape = character_shape(radius_m, height_m);
  SolidObjects solids;
  auto clear = [&](Vec3 position) {
    Obstruction obstruction(n.settings.contact_slop_m);
    const auto matrix =
        JPH::RMat44::sTranslation(jv(position)).PreTranslated(shape->GetCenterOfMass());
    n.physics.GetNarrowPhaseQuery().CollideShape(shape, JPH::Vec3::sReplicate(1), matrix,
                                                 JPH::CollideShapeSettings{}, JPH::RVec3::sZero(),
                                                 obstruction, {}, solids);
    return !obstruction.ShouldEarlyOut();
  };
  if (clear(origin))
    return origin;
  const auto spacing = 2 * (radius_m + n.settings.sweep_skin_m);
  for (int ring = 1;; ++ring) {
    Vec3 offset{-float(ring), -float(ring), 0};
    for (const Vec3 direction : {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{-1, 0, 0}, Vec3{0, -1, 0}})
      for (int step = 0; step < 2 * ring; ++step) {
        const auto position = origin + offset * spacing;
        if (clear(position))
          return position;
        offset = offset + direction;
      }
  }
}
std::optional<BodyMotion> body_motion(const Space &space, IdentityRead identity, Handle object) {
  const auto &n = *space.native;
  if (!identity.valid(object))
    return {};
  auto found = n.bodies.find(object.bits);
  if (found == n.bodies.end())
    return {};
  return BodyMotion{found->second.spec.motion, found->second.spec.collision_solid,
                    !n.compound_parent.contains(object.bits)};
}

namespace {
Status validate_body(const Space::Native &n, const BodySpec &spec) {
  if (!spec.geometry || spec.geometry >= n.geometries.size() || !n.geometries[spec.geometry])
    return Status::MissingGeometry;
  const auto &shape = *n.geometries[spec.geometry];
  if (!std::isfinite(spec.mass_kg) || spec.mass_kg <= 0 || !std::isfinite(spec.surface.friction) ||
      spec.surface.friction < 0 || !std::isfinite(spec.surface.restitution) ||
      spec.surface.restitution < 0 || !std::isfinite(spec.surface.transmission) ||
      spec.surface.transmission < 0 || spec.surface.transmission > 1)
    return Status::UnsupportedMotion;
  switch (spec.motion) {
  case Motion::Static:
    return shape.mesh ? Status::Ok : Status::MissingGeometry;
  case Motion::Dynamic:
  case Motion::Kinematic:
    return shape.hull && !shape.hull->MustBeStatic() ? Status::Ok : Status::UnsupportedMotion;
  case Motion::Character:
    if (!spec.character)
      return Status::UnsupportedMotion;
    const auto &c = *spec.character;
    for (auto value : {c.radius_m, c.height_m, c.mass_kg, c.max_push_force_n, c.max_speed_m_s,
                       c.acceleration_m_s2, c.max_step_m, c.max_slope_radians, c.floor_snap_m})
      if (!std::isfinite(value) || value < 0)
        return Status::UnsupportedMotion;
    return c.radius_m > 0 && c.height_m >= 2 * c.radius_m && c.mass_kg > 0 &&
                   c.max_slope_radians < std::numbers::pi_v<float> / 2
               ? Status::Ok
               : Status::UnsupportedMotion;
  }
  return Status::UnsupportedMotion;
}
} // namespace
BodyCapacity capacity(const Space &space) {
  const auto &n = *space.native;
  return {n.physics.GetMaxBodies() - n.physics.GetNumBodies(),
          n.settings.max_characters - std::uint32_t(n.character_handles.size())};
}
BodyPreflight can_add(const Space &space, std::span<const BodySpec> bodies, std::size_t sensors,
                      std::span<const Handle> retiring) {
  const auto &n = *space.native;
  auto available = capacity(space);
  std::size_t characters = 0, released = 0;
  for (std::size_t index = 0; index < bodies.size(); ++index) {
    auto status = validate_body(n, bodies[index]);
    if (status != Status::Ok)
      return {status, index, bodies.size() + sensors, available.native_available};
    characters += bodies[index].motion == Motion::Character;
  }
  if (!retiring.empty()) {
    std::unordered_set<std::uint64_t> removed;
    removed.reserve(retiring.size());
    for (auto h : retiring) {
      if (!removed.insert(h.bits).second)
        continue;
      if (auto body = n.bodies.find(h.bits); body != n.bodies.end()) {
        released += 1 + body->second.own_geometry.has_value();
        available.characters_available += body->second.character != nullptr;
      }
    }
    for (const auto &volume : n.volumes)
      released += removed.contains(volume.spec.owner.bits);
  }
  const auto native = available.native_available + released, required = bodies.size() + sensors;
  return {required <= native && characters <= available.characters_available ? Status::Ok
                                                                             : Status::Capacity,
          bodies.size(), required, native};
}
OperationResult add(Space &space, IdentityRead identity, TransformRead transforms,
                    const BodySpec &spec) {
  auto &n = *space.native;
  if (!identity.valid(spec.object))
    return {Status::StaleHandle, {}};
  if (n.bodies.contains(spec.object.bits))
    return {Status::AlreadyParented, {}};
  auto checked = can_add(space, std::span(&spec, 1));
  if (checked.status != Status::Ok)
    return {checked.status, {}};
  Mirror body{};
  body.spec = spec;
  body.pose = {transforms.position[spec.object.slot()], transforms.rotation[spec.object.slot()]};
  if (spec.motion == Motion::Character) {
    if (!spec.character || n.character_handles.size() >= n.settings.max_characters)
      return {Status::UnsupportedMotion, {}};
    const auto &c = *spec.character;
    JPH::CharacterVirtualSettings settings;
    settings.mUp = JPH::Vec3::sAxisZ();
    settings.mMaxSlopeAngle = c.max_slope_radians;
    settings.mMass = c.mass_kg;
    settings.mMaxStrength = c.max_push_force_n;
    settings.mShape = character_shape(c.radius_m, c.height_m);
    settings.mInnerBodyShape = settings.mShape;
    settings.mInnerBodyLayer = 3;
    settings.mEnhancedInternalEdgeRemoval = true;
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisZ(), -c.radius_m);
    body.character = std::make_unique<JPH::CharacterVirtual>(
        &settings, jv(body.pose.position), jq(body.pose.rotation), spec.object.bits, &n.physics);
    body.character->SetListener(&n.character_listener);
    body.body = body.character->GetInnerBodyID();
  } else {
    body.body = create_rigid(n, body, spec.motion);
    if (body.body.IsInvalid())
      return {Status::UnsupportedMotion, {}};
  }
  n.handles[body.body.GetIndexAndSequenceNumber()] = spec.object;
  if (body.character)
    n.character_handles.insert(spec.object.bits);
  auto [it, inserted] = n.bodies.emplace(spec.object.bits, std::move(body));
  (void)inserted;
  // Rebalance bulk-loaded broadphase trees at doubling intervals: total build
  // work is linear while spatial queries during loading retain a shallow tree.
  if (std::has_single_bit(n.bodies.size()))
    n.physics.OptimizeBroadPhase();
  auto b = n.world_bounds(it->second);
  n.changed(spec.object, b, b, navigates(it->second));
  return {Status::Ok, {}};
}
OperationResult register_logical_root(Space &space, IdentityRead identity, Handle object) {
  if (!identity.valid(object))
    return {Status::StaleHandle, {}};
  auto &n = *space.native;
  if (n.bodies.contains(object.bits))
    return {Status::AlreadyParented, {}};
  n.logical_roots.insert(object.bits);
  return {Status::Ok, {}};
}

bool is_logical_root(const Space &space, IdentityRead identity, Handle object) {
  return identity.valid(object) && space.native->logical_roots.contains(object.bits);
}

OperationResult update_motion(Space::Native &n, Mirror &body, Motion motion) {
  if (body.character)
    return {motion == Motion::Character ? Status::Ok : Status::UnsupportedMotion, {}};
  if (body.spec.motion == motion)
    return {Status::Ok, {}};
  const auto &geometry = *n.geometries[body.spec.geometry];
  if (motion != Motion::Static && !geometry.hull)
    return {Status::UnsupportedMotion, {}};
  if (n.physics.GetNumBodies() == n.physics.GetMaxBodies())
    return {Status::Capacity, {}};
  auto id = create_rigid(n, body, motion);
  if (id.IsInvalid())
    return {Status::UnsupportedMotion, {}};
  destroy_rigid(n, body.body);
  body.body = id;
  body.spec.motion = motion;
  body.kinematic_start.reset();
  n.handles[id.GetIndexAndSequenceNumber()] = body.spec.object;
  return {Status::Ok, {}};
}
OperationResult set_surface(Space &space, IdentityRead identity, Handle object, Surface surface) {
  auto &n = *space.native;
  auto *body = get(n, identity, object);
  if (!body)
    return {Status::StaleHandle, {}};
  if (body->own_geometry)
    body->own_geometry->spec.surface = surface;
  else
    body->spec.surface = surface;
  n.physics.GetBodyInterface().SetFriction(body->body, surface.friction);
  n.physics.GetBodyInterface().SetRestitution(body->body, surface.restitution);
  auto compound = n.compound_parent.find(object.bits);
  if (body->own_geometry || compound != n.compound_parent.end()) {
    auto &root = body->own_geometry ? *body : n.bodies.at(compound->second);
    float friction = 0, restitution = 0, mass = 0;
    if (root.own_geometry && root.own_geometry->spec.collision_solid) {
      const auto &own = root.own_geometry->spec;
      mass = own.mass_kg;
      friction = own.mass_kg * own.surface.friction;
      restitution = own.surface.restitution;
    }
    for (auto h : root.compound_parts) {
      auto found = n.bodies.find(h.bits);
      if (found == n.bodies.end())
        continue;
      const auto &part = found->second;
      if (part.spec.collision_solid) {
        mass += part.spec.mass_kg;
        friction += part.spec.mass_kg * part.spec.surface.friction;
        restitution = std::max(restitution, part.spec.surface.restitution);
      }
    }
    root.spec.surface.friction = mass > 0 ? friction / mass : 0;
    root.spec.surface.restitution = restitution;
    n.physics.GetBodyInterface().SetFriction(root.body, root.spec.surface.friction);
    n.physics.GetBodyInterface().SetRestitution(root.body, restitution);
  }
  return {Status::Ok, {}};
}
OperationResult set_pose(Space &space, IdentityRead identity, TransformWrite transforms,
                         Handle object, Pose pose) {
  auto &n = *space.native;
  if (!identity.valid(object))
    return {Status::StaleHandle, {}};
  if (n.compound_parent.contains(object.bits))
    return {Status::UnsupportedMotion, {}};
  auto *body = get(n, identity, object);
  if (!body) {
    n.dirty_objects.insert(object.bits);
    transforms.position[object.slot()] = pose.position;
    transforms.rotation[object.slot()] = pose.rotation;
    return {Status::Ok, {}};
  }
  auto old = n.world_bounds(*body);
  if (body->character && body->pose.position != pose.position)
    body->route_dirty = true;
  native_pose(n, *body, pose);
  if (body->kinematic_start) {
    body->kinematic_start.reset();
    n.physics.GetBodyInterface().SetLinearAndAngularVelocity(body->body, JPH::Vec3::sZero(),
                                                             JPH::Vec3::sZero());
  }
  transforms.position[object.slot()] = pose.position;
  transforms.rotation[object.slot()] = pose.rotation;
  auto attachment = n.attachments.find(object.bits);
  if (attachment != n.attachments.end())
    attachment->second.local_pose =
        relative({transforms.position[attachment->second.parent.slot()],
                  transforms.rotation[attachment->second.parent.slot()]},
                 pose);
  n.changed(object, old, n.world_bounds(*body), navigates(*body));
  return {Status::Ok, {}};
}
void remove(Space &space, IdentityRead identity, Handle object, ParentWrite parents,
            SpaceFlags flags, PhysicalRelations &relations, std::vector<RelationChange> &changes) {
  auto &n = *space.native;
  remove_compound_member(space, identity, flags, object);
  n.logical_roots.erase(object.bits);
  extract_native(n, object);
  if (auto geometry = n.compound_geometry.find(object.bits);
      geometry != n.compound_geometry.end()) {
    n.geometries[geometry->second].reset();
    n.free_compound_geometry.push_back(geometry->second);
    n.compound_geometry.erase(geometry);
  }
  ++n.volume_revision;
  std::erase_if(n.volumes, [&](const Volume &volume) {
    if (volume.spec.owner != object)
      return false;
    const auto bounds = shape_bounds(n.geometries[volume.spec.geometry]->mesh, volume.world);
    destroy_rigid(n, volume.body);
    n.dirty_bounds.push_back(bounds);
    n.wake(bounds);
    return true;
  });
  n.attachments.erase(object.bits);
  for (auto child = n.attachments.begin(); child != n.attachments.end();)
    if (child->second.parent == object) {
      Handle h{child->first};
      parents.parent[h.slot()] = {};
      auto *mirror = get(n, identity, h);
      if (mirror) {
        auto mode = mirror->character ? Motion::Character : Motion::Dynamic;
        update_motion(n, *mirror, mode);
        if (mirror->character)
          n.physics.GetBodyInterface().SetObjectLayer(mirror->body, 3);
        flags_for(flags, h, mode);
      }
      child = n.attachments.erase(child);
    } else
      ++child;
  if (const auto *members = relations.contains.out.get(object.slot()))
    for (auto member : *members) {
      n.dirty_objects.insert(member.bits);
      if (identity.valid(member) && parents.parent[member.slot()] == object)
        parents.parent[member.slot()] = {};
    }
  if (const auto *members = relations.in_zone.in.get(object.slot()))
    for (auto member : *members)
      n.dirty_objects.insert(member.bits);
  erase_edges(relations.contains, RelationKind::Contains, object, changes);
  erase_edges(relations.supports, RelationKind::Supports, object, changes);
  erase_edges(relations.attached_to, RelationKind::AttachedTo, object, changes);
  erase_edges(relations.in_zone, RelationKind::InZone, object, changes);
  if (identity.valid(object))
    parents.parent[object.slot()] = {};
}
OperationResult can_custody(const Space &space, IdentityRead identity, Handle object,
                            Handle custodian, ParentRead parents,
                            std::span<const Handle> descendants, bool stay_static) {
  if (!identity.valid(object) || (custodian && !identity.valid(custodian)))
    return {Status::StaleHandle, {}};
  for (auto child : descendants)
    if (!identity.valid(child))
      return {Status::StaleHandle, {}};
  const auto member = [&](Handle h) {
    return h == object || std::ranges::find(descendants, h) != descendants.end();
  };
  const auto &n = *space.native;
  for (auto parent = custodian; parent; parent = parents.parent[parent.slot()]) {
    if (member(parent))
      return {Status::AttachmentCycle, {}};
    // External occupants of collected goods are detached below. A recipient
    // seated on those goods will therefore no longer be their descendant.
    auto attached = n.attachments.find(parent.bits);
    if (attached != n.attachments.end() && member(attached->second.parent))
      break;
  }
  const bool retiring_body =
      n.bodies.contains(object.bits) ||
      std::ranges::any_of(descendants, [&](Handle h) { return n.bodies.contains(h.bits); }) ||
      std::ranges::any_of(n.volumes, [&](const Volume &v) { return member(v.spec.owner); });
  return can_release_occupants(space, object, descendants, retiring_body, stay_static);
}
OperationResult custody(amber::ObjectState &objects_state, Handle object, Handle custodian,
                        amber::space::State &space_state, std::span<const Handle> descendants,
                        bool stay_static) {
  auto &space = space_state.space;

  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto parents = amber::parents(objects_state.objects);
  auto flags = amber::space_flags(objects_state.objects);
  auto &relations = space_state.physical_relations;
  auto &changes = space_state.relation_changes;

  const auto checked =
      can_custody(space, identity, object, custodian, parents, descendants, stay_static);
  if (checked.status != Status::Ok)
    return checked;
  std::set<std::uint64_t> assembly{object.bits};
  for (auto h : descendants)
    assembly.insert(h.bits);
  auto &n = *space.native;
  ++n.volume_revision;
  std::erase_if(n.volumes, [&](const Volume &volume) {
    if (!assembly.contains(volume.spec.owner.bits))
      return false;
    const auto bounds = shape_bounds(n.geometries[volume.spec.geometry]->mesh, volume.world);
    destroy_rigid(n, volume.body);
    n.dirty_bounds.push_back(bounds);
    n.wake(bounds);
    return true;
  });
  // Retiring native goods first supplies the one temporary slot required to
  // restore unrelated kinematic occupants to their ordinary dynamic motion.
  for (auto bits : assembly)
    extract_native(n, Handle{bits});
  const auto released = release_occupants(space, identity, object, descendants, parents, flags,
                                          relations, changes, stay_static);
  if (released.status != Status::Ok)
    return released;
  for (auto bits : assembly) {
    const Handle member{bits};
    n.attachments.erase(bits);
    if (const auto *contained = relations.contains.out.get(member.slot()))
      for (auto item : *contained) {
        n.dirty_objects.insert(item.bits);
        if (!assembly.contains(item.bits) && parents.parent[item.slot()] == member)
          parents.parent[item.slot()] = {};
      }
    if (const auto *zoned = relations.in_zone.in.get(member.slot()))
      for (auto item : *zoned)
        n.dirty_objects.insert(item.bits);
    erase_edges(relations.contains, RelationKind::Contains, member, changes);
    erase_edges(relations.supports, RelationKind::Supports, member, changes);
    erase_edges(relations.in_zone, RelationKind::InZone, member, changes);
    auto *outgoing = relations.attached_to.out.get(member.slot());
    auto attached = outgoing ? *outgoing : std::vector<Handle>{};
    for (auto parent : attached)
      if (!assembly.contains(parent.bits))
        edge(relations.attached_to, RelationKind::AttachedTo, member, parent, false, changes);
    flags.set(member.slot(), Held);
  }
  parents.parent[object.slot()] = custodian;
  return {Status::Ok, {}};
}

OperationResult add_volume(Space &space, IdentityRead identity, TransformRead transforms,
                           const VolumeSpec &spec) {
  auto &n = *space.native;
  if (!identity.valid(spec.owner))
    return {Status::StaleHandle, {}};
  if (!spec.geometry || spec.geometry >= n.geometries.size() || !n.geometries[spec.geometry])
    return {Status::MissingGeometry, {}};
  if (capacity(space).native_available == 0)
    return {Status::Capacity, {}};
  if (std::ranges::any_of(n.volumes, [&](const Volume &volume) {
        return volume.spec.owner == spec.owner && volume.spec.kind == spec.kind;
      }))
    return {Status::AlreadyParented, {}};
  auto pose =
      compose({transforms.position[spec.owner.slot()], transforms.rotation[spec.owner.slot()]},
              spec.local_pose);
  const auto &geometry = *n.geometries[spec.geometry];
  // Authored volumes are solid sensor boxes; imported fixtures may carry a
  // triangle surface plus its solid convex representation.
  const auto *shape = geometry.hull ? geometry.hull.GetPtr() : geometry.mesh.GetPtr();
  JPH::BodyCreationSettings settings(shape, jv(pose.position), jq(pose.rotation),
                                     JPH::EMotionType::Kinematic, 2);
  // Active kinematic sensors track sleeping and static subjects too.
  settings.mIsSensor = true;
  settings.mCollideKinematicVsNonDynamic = true;
  settings.mAllowSleeping = false;
  settings.mUserData = spec.owner.bits;
  auto body = n.physics.GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
  if (body.IsInvalid())
    return {Status::UnsupportedMotion, {}};
  n.handles[body.GetIndexAndSequenceNumber()] = spec.owner;
  n.volumes.push_back({spec, pose, body});
  ++n.volume_revision;
  const auto bounds = shape_bounds(n.geometries[spec.geometry]->mesh, pose);
  n.dirty_bounds.push_back(bounds);
  n.wake(bounds);
  return {Status::Ok, {}};
}
std::optional<Bounds> volume_bounds(const Space &space, Handle owner, VolumeKind kind) {
  const auto &n = *space.native;
  const auto found = std::ranges::find_if(
      n.volumes, [&](const Volume &v) { return v.spec.owner == owner && v.spec.kind == kind; });
  if (found == n.volumes.end())
    return {};
  return shape_bounds(n.geometries[found->spec.geometry]->mesh, found->world);
}
void volumes_at(const Space &space, IdentityRead identity, Vec3 point, VolumeKind kind,
                std::vector<Handle> &out) {
  out.clear();
  const auto &n = *space.native;
  class Sensors final : public JPH::ObjectLayerFilter {
    bool ShouldCollide(JPH::ObjectLayer layer) const override { return layer == 2; }
  } sensors;
  JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> candidates;
  n.physics.GetBroadPhaseQuery().CollidePoint(jv(point), candidates, {}, sensors);
  for (auto id : candidates.mHits) {
    auto volume = std::ranges::find_if(n.volumes, [&](const Volume &v) { return v.body == id; });
    if (volume != n.volumes.end() && volume->spec.kind == kind &&
        identity.valid(volume->spec.owner) &&
        point_in(*n.geometries[volume->spec.geometry], volume->world, point))
      out.push_back(volume->spec.owner);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
}

std::optional<Bounds> bounds(const Space &space, IdentityRead identity, Handle object) {
  if (!identity.valid(object))
    return {};
  const auto &n = *space.native;
  auto collect = [&](auto &&self, Handle subject) -> std::optional<Bounds> {
    std::optional<Bounds> result;
    auto body = n.bodies.find(subject.bits);
    if (body != n.bodies.end())
      result = n.world_bounds(body->second);
    for (const auto &[bits, attachment] : n.attachments) {
      if (attachment.parent != subject || !identity.valid({bits}))
        continue;
      auto child = self(self, Handle{bits});
      if (!child)
        continue;
      if (!result)
        result = child;
      else {
        result->min = {std::min(result->min.x, child->min.x), std::min(result->min.y, child->min.y),
                       std::min(result->min.z, child->min.z)};
        result->max = {std::max(result->max.x, child->max.x), std::max(result->max.y, child->max.y),
                       std::max(result->max.z, child->max.z)};
      }
    }
    return result;
  };
  return collect(collect, object);
}
std::optional<Hit> raycast(const Space &space, IdentityRead identity, Vec3 from, Vec3 to,
                           QueryFilter filter) {
  auto all = hits(space, identity, from, to, filter);
  return all.empty() ? std::nullopt : std::optional<Hit>(all.front());
}
Visibility visibility(const Space &space, IdentityRead identity, Vec3 from, Vec3 to,
                      QueryFilter filter, float minimum) {
  auto all = hits(space, identity, from, to, filter);
  Visibility result{true, 1, {}};
  std::set<std::uint64_t> seen;
  for (const auto &hit : all)
    if (seen.insert(hit.object.bits).second) {
      auto it = space.native->bodies.find(hit.object.bits);
      if (it == space.native->bodies.end())
        continue;
      result.transmission *= it->second.own_geometry
                                 ? it->second.own_geometry->spec.surface.transmission
                                 : it->second.spec.surface.transmission;
      result.through.push_back(hit.object);
      if (result.transmission < minimum) {
        result.clear = false;
        break;
      }
    }
  return result;
}
void near(const Space &space, IdentityRead identity, Vec3 point, float radius, QueryFilter filter,
          std::vector<Nearby> &out) {
  const auto &n = *space.native;
  out.clear();
  class Candidates final : public JPH::CollideShapeBodyCollector {
  public:
    std::vector<JPH::BodyID> bodies;
    RunDeadline deadline;
    explicit Candidates(RunDeadline until) : deadline(until) {}
    void AddHit(const JPH::BodyID &id) override {
      if (deadline.passed())
        ForceEarlyOut();
      else
        bodies.push_back(id);
    }
  } candidates(filter.deadline);
  ObjectFilter objects(filter.include_sensors);
  n.physics.GetBroadPhaseQuery().CollideSphere(jv(point), radius, candidates, {}, objects);
  std::set<std::uint64_t> seen;
  for (auto id : candidates.bodies) {
    if (filter.deadline.passed())
      break;
    auto owner = n.handles.find(id.GetIndexAndSequenceNumber());
    if (owner == n.handles.end())
      continue;
    Handle h = owner->second;
    if (!identity.valid(h) || h == filter.ignore_a || h == filter.ignore_b ||
        std::ranges::find(filter.ignore, h) != filter.ignore.end())
      continue;
    auto found = n.bodies.find(h.bits);
    if (found == n.bodies.end() || !seen.insert(h.bits).second)
      continue;
    const auto &body = found->second;
    auto b = space.native->world_bounds(body);
    Vec3 p{std::clamp(point.x, b.min.x, b.max.x), std::clamp(point.y, b.min.y, b.max.y),
           std::clamp(point.z, b.min.z, b.max.z)};
    auto d = (p - point).length();
    if (d <= radius)
      out.push_back({h, d, b});
  }
  std::sort(out.begin(), out.end(),
            [](const auto &a, const auto &b) { return a.distance_m < b.distance_m; });
}
double illuminance(const Space &space, IdentityRead identity, Handle receiver, Vec3 point,
                   double ambient, std::span<const LightSample> lights) {
  double result = ambient;
  for (const auto &light : lights) {
    double d2 = (point - light.position).length_squared();
    if (d2 > light.range_m * light.range_m)
      continue;
    auto visible =
        visibility(space, identity, point, light.position, {receiver, light.source, false},
                   std::numeric_limits<float>::epsilon());
    if (visible.clear)
      result += light.intensity_cd * visible.transmission /
                std::max(d2, double(light.source_radius_m) * light.source_radius_m);
  }
  return result;
}
OperationResult attach(amber::ObjectState &objects_state, amber::space::State &space_state,
                       const Attachment &attachment) {
  auto &space = space_state.space;

  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto transforms = amber::transforms(objects_state.objects);
  auto parents = amber::parents(objects_state.objects);
  auto flags = amber::space_flags(objects_state.objects);
  auto &contains = space_state.physical_relations.contains;
  auto &supports = space_state.physical_relations.supports;
  auto &attached_to = space_state.physical_relations.attached_to;
  auto &changes = space_state.relation_changes;

  auto &n = *space.native;
  auto *body = get(n, identity, attachment.subject);
  if (n.compound_parent.contains(attachment.subject.bits))
    return {Status::UnsupportedMotion, {}};
  if (!identity.valid(attachment.subject) || !identity.valid(attachment.parent))
    return {Status::StaleHandle, {}};
  if (!body && !n.logical_roots.contains(attachment.subject.bits))
    return {Status::MissingBody, {}};
  if (n.attachments.contains(attachment.subject.bits))
    return {Status::AlreadyParented, {}};
  for (auto p = attachment.parent; p;) {
    if (p == attachment.subject)
      return {Status::AttachmentCycle, {}};
    p = parents.parent[p.slot()];
  }
  if (body && !body->character && !n.geometries[body->spec.geometry]->hull)
    return {Status::UnsupportedMotion, {}};
  const auto old = body ? n.world_bounds(*body) : Bounds{};
  if (body && !body->character) {
    auto result = update_motion(n, *body, Motion::Kinematic);
    if (result.status != Status::Ok) {
      return result;
    }
    n.physics.GetBodyInterface().SetLinearAndAngularVelocity(body->body, JPH::Vec3::sZero(),
                                                             JPH::Vec3::sZero());
  }
  if (body && body->agent >= 0) {
    n.crowd->removeAgent(body->agent);
    body->agent = -1;
    body->route_dirty = true;
    body->membership.due = 0;
  }
  if (body)
    n.physics.GetBodyInterface().SetObjectLayer(
        body->body, attachment.grasp || body->character || !body->spec.collision_solid ? 4 : 1);
  n.attachments[attachment.subject.bits] = attachment;
  Pose parent{transforms.position[attachment.parent.slot()],
              transforms.rotation[attachment.parent.slot()]};
  auto target = compose(parent, attachment.local_pose);
  if (body)
    native_pose(n, *body, target);
  transforms.position[attachment.subject.slot()] = target.position;
  transforms.rotation[attachment.subject.slot()] = target.rotation;
  parents.parent[attachment.subject.slot()] = attachment.parent;
  flags_for(flags, attachment.subject, body ? body->spec.motion : Motion::Kinematic,
            attachment.grasp);
  erase_edges(contains, RelationKind::Contains, attachment.subject, changes);
  erase_edges(supports, RelationKind::Supports, attachment.subject, changes);
  edge(attached_to, RelationKind::AttachedTo, attachment.subject, attachment.parent, true, changes);
  if (body)
    n.changed(attachment.subject, old, n.world_bounds(*body), navigates(*body));
  else
    n.dirty_objects.insert(attachment.subject.bits);
  return {Status::Ok, {}};
}
OperationResult detach(amber::ObjectState &objects_state, amber::space::State &space_state,
                       const Release &release) {
  auto &space = space_state.space;

  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto transforms = amber::transforms(objects_state.objects);
  auto parents = amber::parents(objects_state.objects);
  auto flags = amber::space_flags(objects_state.objects);
  auto &attached_to = space_state.physical_relations.attached_to;
  auto &changes = space_state.relation_changes;

  auto &n = *space.native;
  auto *body = get(n, identity, release.subject);
  if (n.compound_parent.contains(release.subject.bits))
    return {Status::UnsupportedMotion, {}};
  if (!identity.valid(release.subject))
    return {Status::StaleHandle, {}};
  if (!body && !n.logical_roots.contains(release.subject.bits))
    return {Status::MissingBody, {}};
  auto attachment = n.attachments.find(release.subject.bits);
  if (attachment == n.attachments.end())
    return {Status::NotAttached, {}};
  auto release_pose = compose({transforms.position[attachment->second.parent.slot()],
                               transforms.rotation[attachment->second.parent.slot()]},
                              attachment->second.local_pose);
  if (!body) {
    if (release.motion != Motion::Static && release.motion != Motion::Kinematic)
      return {Status::UnsupportedMotion, {}};
    const auto parent = attachment->second.parent;
    transforms.position[release.subject.slot()] = release_pose.position;
    transforms.rotation[release.subject.slot()] = release_pose.rotation;
    parents.parent[release.subject.slot()] = {};
    n.attachments.erase(attachment);
    flags_for(flags, release.subject, release.motion);
    edge(attached_to, RelationKind::AttachedTo, release.subject, parent, false, changes);
    n.dirty_objects.insert(release.subject.bits);
    return {Status::Ok, {}};
  }
  native_pose(n, *body, release_pose);
  transforms.position[release.subject.slot()] = release_pose.position;
  transforms.rotation[release.subject.slot()] = release_pose.rotation;
  const auto shape = body->character ? body->character->GetShape()
                                     : n.geometries[body->spec.geometry]->hull.GetPtr();
  JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
  SolidObjects objects(!body->character);
  BodyFilter bodies(n, {release.subject, attachment->second.parent, false});
  auto matrix = JPH::RMat44::sRotationTranslation(jq(body->pose.rotation), jv(body->pose.position))
                    .PreTranslated(shape->GetCenterOfMass());
  n.physics.GetNarrowPhaseQuery().CollideShape(shape, JPH::Vec3::sReplicate(1), matrix,
                                               JPH::CollideShapeSettings{}, JPH::RVec3::sZero(),
                                               collector, {}, objects, bodies);
  for (const auto &hit : collector.mHits)
    if (hit.mPenetrationDepth > n.settings.contact_slop_m) {
      auto owner = n.handles.find(hit.mBodyID2.GetIndexAndSequenceNumber());
      return {Status::Obstructed, owner == n.handles.end() ? Handle{} : owner->second};
    }
  const auto parent = attachment->second.parent;
  auto mode = body->character ? Motion::Character : release.motion;
  auto status = update_motion(n, *body, mode);
  if (status.status != Status::Ok) {
    return status;
  }
  n.attachments.erase(attachment);
  parents.parent[release.subject.slot()] = {};
  flags_for(flags, release.subject, mode);
  n.physics.GetBodyInterface().SetObjectLayer(body->body, body->character              ? 3
                                                          : body->spec.collision_solid ? layer(mode)
                                                                                       : 4);
  if (body->character)
    body->character->SetLinearVelocity(jv(release.linear_velocity_m_s));
  else
    n.physics.GetBodyInterface().SetLinearAndAngularVelocity(
        body->body, jv(release.linear_velocity_m_s), jv(release.angular_velocity_rad_s));
  edge(attached_to, RelationKind::AttachedTo, release.subject, parent, false, changes);
  n.dirty_objects.insert(release.subject.bits);
  return {Status::Ok, {}};
}
OperationResult impulse(Space &space, IdentityRead identity, Handle object, Vec3 momentum,
                        Vec3 point) {
  auto &n = *space.native;
  auto *body = get(n, identity, object);
  if (!body)
    return {Status::StaleHandle, {}};
  if (body->spec.motion != Motion::Dynamic || n.attachments.contains(object.bits))
    return {Status::UnsupportedMotion, {}};
  if (momentum == Vec3{})
    return {Status::Ok, {}};
  n.physics.GetBodyInterface().AddImpulse(body->body, jv(momentum), jv(point));
  return {Status::Ok, {}};
}
std::optional<Hit> sweep(const Space &space, IdentityRead identity, Handle object, Vec3 delta,
                         QueryFilter filter) {
  const auto &n = *space.native;
  auto it = n.bodies.find(object.bits);
  if (!identity.valid(object) || it == n.bodies.end())
    return {};
  const auto &body = it->second;
  const auto *shape =
      body.character ? body.character->GetShape() : n.geometries[body.spec.geometry]->hull.GetPtr();
  if (!shape)
    return {};
  class Collector final : public JPH::CastShapeCollector {
  public:
    JPH::Vec3 direction;
    std::optional<JPH::ShapeCastResult> closest;
    explicit Collector(Vec3 v) : direction(jv(v)) {}
    void AddHit(const JPH::ShapeCastResult &hit) override {
      auto normal = -hit.mPenetrationAxis.NormalizedOr(JPH::Vec3::sZero());
      if (hit.mFraction == 0 && normal.Dot(direction) >= -std::numeric_limits<float>::epsilon())
        return;
      if (!closest || hit.mFraction < closest->mFraction) {
        closest = hit;
        UpdateEarlyOutFraction(std::max(0.f, hit.mFraction));
      }
    }
  } collector(delta);
  filter.ignore_a = object;
  SolidObjects objects(!body.character);
  BodyFilter bodies(n, filter);
  auto cast = JPH::RShapeCast::sFromWorldTransform(
      shape, JPH::Vec3::sReplicate(1),
      JPH::RMat44::sRotationTranslation(jq(body.pose.rotation), jv(body.pose.position)), jv(delta));
  JPH::ShapeCastSettings settings;
  // Rounded casts give grazing contacts, including terrain, a stable surface normal.
  settings.mUseShrunkenShapeAndConvexRadius = true;
  n.physics.GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector, {},
                                            objects, bodies);
  if (!collector.closest)
    return {};
  auto hit = *collector.closest;
  auto owner = n.handles.find(hit.mBodyID2.GetIndexAndSequenceNumber());
  return Hit{owner == n.handles.end() ? Handle{} : owner->second, hit.mFraction,
             av(hit.mContactPointOn2), av(-hit.mPenetrationAxis.NormalizedOr(JPH::Vec3::sZero()))};
}
SweepMove translate(Space &space, IdentityRead identity, TransformWrite transforms, Handle object,
                    Vec3 delta, float seconds) {
  auto &n = *space.native;
  auto *body = get(n, identity, object);
  if (!body)
    return {Status::StaleHandle, {}, {}};
  if (n.compound_parent.contains(object.bits))
    return {Status::UnsupportedMotion, body->pose, {}};
  if (body->spec.motion != Motion::Kinematic || !n.geometries[body->spec.geometry]->hull)
    return {Status::UnsupportedMotion, body->pose, {}};
  Handle parent;
  auto attachment = n.attachments.find(object.bits);
  if (attachment != n.attachments.end())
    parent = attachment->second.parent;
  auto cast = sweep(space, identity, object, delta, {object, parent, false});
  float fraction =
      cast ? std::max(0.f, cast->fraction - n.settings.sweep_skin_m /
                                                std::max(delta.length(), n.settings.sweep_skin_m))
           : 1.f;
  Pose actual{body->pose.position + delta * fraction, body->pose.rotation};
  if (actual.position == body->pose.position)
    return {cast ? Status::Obstructed : Status::Ok, actual, cast};
  const auto start = body->kinematic_start.value_or(body->pose);
  set_pose(space, identity, transforms, object, actual);
  // Queries see the accepted pose immediately. Physics replays this displacement once
  // during its phase so dynamic bodies receive the kinematic contact velocity.
  auto &interface = n.physics.GetBodyInterface();
  interface.SetPositionAndRotation(body->body, jv(start.position), jq(start.rotation),
                                   JPH::EActivation::Activate);
  interface.MoveKinematic(body->body, jv(actual.position), jq(actual.rotation), seconds);
  interface.SetPositionAndRotation(body->body, jv(actual.position), jq(actual.rotation),
                                   JPH::EActivation::Activate);
  body->kinematic_start = start;
  return {cast ? Status::Obstructed : Status::Ok, actual, cast};
}

void step(Space &space, amber::ObjectState &objects_state, PhysicalRelations &relations, Step now,
          float seconds, std::span<const MoveRequest> requests, StepResult &out) {
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto transforms = amber::transforms(objects_state.objects);
  auto parents = amber::parents(objects_state.objects);
  auto flags = amber::space_flags(objects_state.objects);

  ZoneScopedN("physics and navigation");
  ZoneValue(requests.size());
  auto &n = *space.native;
  out = {};
  std::set<std::uint64_t> touched = n.dirty_objects;
  touched.insert(n.character_handles.begin(), n.character_handles.end());
  auto include_active = [&] {
    const auto count = n.physics.GetNumActiveBodies(JPH::EBodyType::RigidBody);
    const auto *ids = n.physics.GetActiveBodiesUnsafe(JPH::EBodyType::RigidBody);
    for (std::uint32_t i = 0; i < count; ++i) {
      auto found = n.handles.find(ids[i].GetIndexAndSequenceNumber());
      if (found != n.handles.end())
        touched.insert(found->second.bits);
    }
  };
  // Include bodies active before Update too: the final falling frame may put a body to sleep.
  include_active();
  std::map<std::uint64_t, const MoveRequest *> active;
  std::set<std::uint64_t> target_gone;
  for (const auto &request : requests)
    active[request.body.bits] = &request;
  for (auto bits : n.character_handles) {
    auto &body = n.bodies.at(bits);
    if (n.attachments.contains(bits))
      continue;
    auto input = active.find(bits);
    if (input == active.end()) {
      if (body.intent) {
        if (body.agent >= 0)
          n.crowd->resetMoveTarget(body.agent);
        body.intent = 0;
        body.route.clear();
        body.forbidden.clear();
        body.membership.due = 0;
      }
      continue;
    }
    const auto &request = *input->second;
    if (const auto *target = std::get_if<ObjectGoal>(&request.goal);
        target && !identity.valid(target->target)) {
      out.intents.push_back(
          {request.body, request.intent, IntentState::TargetGone, PathStatus::Unreachable});
      target_gone.insert(bits);
      body.route.clear();
      body.path_status = PathStatus::Unreachable;
      body.membership.due = 0;
      if (body.agent >= 0)
        n.crowd->resetMoveTarget(body.agent);
      continue;
    }
    if (body.agent >= 0) {
      auto parameters = n.crowd->getAgent(body.agent)->params;
      const auto maximum = body.spec.character->max_speed_m_s;
      const auto requested = std::clamp(request.speed_m_s.value_or(maximum), 0.f, maximum);
      if (parameters.maxSpeed != requested) {
        parameters.maxSpeed = requested;
        n.crowd->updateAgentParameters(body.agent, &parameters);
      }
      auto *agent = n.crowd->getEditableAgent(body.agent);
      auto actual = nav(body.pose.position);
      agent->corridor.movePosition(&actual.x, n.query, n.crowd->getFilter(0));
      std::copy_n(agent->corridor.getPos(), 3, agent->npos);
      auto velocity = nav(av(body.character->GetLinearVelocity()));
      agent->vel[0] = velocity.x;
      agent->vel[1] = 0;
      agent->vel[2] = velocity.z;
      if (body.path_status == PathStatus::Ready && body.waypoint + 1 < body.route.size())
        request_corner(n, body);
    }
  }
  if (!n.character_handles.empty())
    n.crowd->update(seconds, nullptr);
  for (auto bits : n.character_handles) {
    auto &body = n.bodies.at(bits);
    if (n.attachments.contains(bits))
      continue;
    auto old = body.pose.position;
    Vec3 desired;
    const auto input = active.find(bits);
    if (input != active.end() && body.path_status == PathStatus::Ready && body.agent >= 0) {
      auto *agent = n.crowd->getAgent(body.agent);
      desired = world({agent->vel[0], 0, agent->vel[2]});
    }
    if (input != active.end() && travel_blocked(old, old + desired * seconds, body.forbidden)) {
      desired = {};
      body.path_status = PathStatus::AccessDenied;
    }
    auto vertical = av(body.character->GetLinearVelocity()).z;
    if (body.character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround &&
        vertical <= 0)
      vertical = body.character->GetGroundVelocity().GetZ();
    auto velocity = desired + Vec3{0, 0, vertical} + n.settings.gravity_m_s2 * seconds;
    body.character->SetLinearVelocity(jv(velocity));
    JPH::CharacterVirtual::ExtendedUpdateSettings settings;
    const auto &c = *body.spec.character;
    settings.mWalkStairsStepUp = {0, 0, c.max_step_m};
    settings.mStickToFloorStepDown = {0, 0, -c.floor_snap_m};
    BodyFilter filter(n, {{bits}, {}, false});
    body.character->ExtendedUpdate(seconds, jv(n.settings.gravity_m_s2), settings,
                                   n.physics.GetDefaultBroadPhaseLayerFilter(3),
                                   n.physics.GetDefaultLayerFilter(3), filter, {}, n.temp);
    body.pose = {av(body.character->GetPosition()), aq(body.character->GetRotation())};
    if (body.character->GetGroundState() != JPH::CharacterBase::EGroundState::OnGround &&
        body.agent >= 0) {
      n.crowd->removeAgent(body.agent);
      body.agent = -1;
      body.route.clear();
      body.route_dirty = true;
      body.membership.due = 0;
    }
    if (input != active.end() && !target_gone.contains(bits)) {
      const auto &request = *input->second;
      float tolerance = c.radius_m;
      if (const auto *point = std::get_if<PointGoal>(&request.goal))
        tolerance = point->tolerance_m;
      if (!body.route.empty() && body.waypoint + 1 == body.route.size() &&
          (body.route.back() - body.pose.position).length() <= tolerance) {
        out.intents.push_back({{bits}, request.intent, IntentState::Arrived, body.path_status});
        n.crowd->resetMoveTarget(body.agent);
        body.route.clear();
        body.membership.due = 0;
      } else {
        if ((body.pose.position - old).length() < n.settings.contact_slop_m)
          body.stuck_s += seconds;
        else
          body.stuck_s = 0;
        auto state = body.path_status == PathStatus::PendingTiles ? IntentState::WaitingNavigation
                     : body.path_status != PathStatus::Ready ||
                             (body.stuck_s >= n.settings.blocked_after_s && n.dirty_tiles.empty())
                         ? IntentState::Blocked
                         : IntentState::Moving;
        out.intents.push_back({{bits}, request.intent, state, body.path_status});
        if (body.route.size() && body.waypoint + 1 < body.route.size() &&
            (body.route[body.waypoint] - body.pose.position).length() < c.radius_m) {
          ++body.waypoint;
          body.membership.due = 0;
          request_corner(n, body);
        }
      }
    }
  }
  for (auto bits : touched) {
    auto found = n.bodies.find(bits);
    if (found == n.bodies.end())
      continue;
    auto &body = found->second;
    if (body.kinematic_start)
      n.physics.GetBodyInterface().SetPositionAndRotation(
          body.body, jv(body.kinematic_start->position), jq(body.kinematic_start->rotation),
          JPH::EActivation::Activate);
  }
  auto physics_error = n.physics.Update(seconds, 1, &n.temp, &n.jobs);
  if (physics_error != JPH::EPhysicsUpdateError::None)
    throw std::runtime_error("Jolt physics capacity exceeded");
  include_active();
  for (auto bits : touched) {
    auto found = n.bodies.find(bits);
    if (found == n.bodies.end())
      continue;
    auto &body = found->second;
    if (!identity.valid({bits}))
      continue;
    if (!body.character) {
      JPH::RVec3 position;
      JPH::Quat rotation;
      n.physics.GetBodyInterface().GetPositionAndRotation(body.body, position, rotation);
      body.pose = {av(position), aq(rotation)};
      if (body.own_geometry)
        n.physics.GetBodyInterface().SetPositionAndRotation(
            body.own_geometry->query_body, position, rotation, JPH::EActivation::DontActivate);
      if (body.kinematic_start) {
        n.physics.GetBodyInterface().SetLinearAndAngularVelocity(body.body, JPH::Vec3::sZero(),
                                                                 JPH::Vec3::sZero());
        body.kinematic_start.reset();
      }
    }
  }
  std::set<std::uint64_t> followed;
  auto follow = [&](auto &&self, Handle child) -> void {
    if (!followed.insert(child.bits).second)
      return;
    auto attachment = n.attachments.find(child.bits);
    if (attachment == n.attachments.end() || !identity.valid(attachment->second.parent))
      return;
    const auto parent = attachment->second.parent;
    self(self, parent);
    auto body = n.bodies.find(child.bits);
    auto parent_body = n.bodies.find(parent.bits);
    Pose parent_pose = parent_body == n.bodies.end() ? Pose{transforms.position[parent.slot()],
                                                            transforms.rotation[parent.slot()]}
                                                     : parent_body->second.pose;
    const auto target = compose(parent_pose, attachment->second.local_pose);
    if (body != n.bodies.end()) {
      native_pose(n, body->second, target);
    } else if (n.logical_roots.contains(child.bits)) {
      if (transforms.position[child.slot()] != target.position ||
          transforms.rotation[child.slot()] != target.rotation) {
        n.dirty_objects.insert(child.bits);
      }
      transforms.position[child.slot()] = target.position;
      transforms.rotation[child.slot()] = target.rotation;
    }
  };
  std::vector<std::uint64_t> related(touched.begin(), touched.end());
  for (std::size_t i = 0; i < related.size(); ++i) {
    auto bits = related[i];
    follow(follow, {bits});
    if (const auto *children = relations.attached_to.in.get(Handle{bits}.slot()))
      for (auto child : *children)
        if (touched.insert(child.bits).second)
          related.push_back(child.bits);
  }
  for (auto bits : touched) {
    auto found = n.bodies.find(bits);
    if (found == n.bodies.end()) {
      if (identity.valid({bits}) && n.logical_roots.contains(bits) &&
          n.dirty_objects.contains(bits))
        out.moved.push_back({bits});
      continue;
    }
    auto &body = found->second;
    if (!identity.valid({bits}))
      continue;
    auto attached_flags = n.attachments.find(bits);
    flags_for(flags, {bits}, body.spec.motion,
              attached_flags != n.attachments.end() && attached_flags->second.grasp);
    auto previous = transforms.position[Handle{bits}.slot()];
    auto previous_rotation = transforms.rotation[Handle{bits}.slot()];
    if (previous != body.pose.position || previous_rotation != body.pose.rotation) {
      auto b = n.world_bounds(body);
      auto old = n.world_bounds(body, {previous, previous_rotation});
      n.dirty_bounds.push_back(old);
      n.dirty_bounds.push_back(b);
      n.dirty_objects.insert(bits);
      transforms.position[Handle{bits}.slot()] = body.pose.position;
      transforms.rotation[Handle{bits}.slot()] = body.pose.rotation;
      const auto attached = n.attachments.find(bits);
      if (navigates(body) && !n.compound_parent.contains(bits) &&
          (attached == n.attachments.end() || !attached->second.grasp)) {
        n.dirty_navigation(old);
        n.dirty_navigation(b);
      }
    }
    if (n.dirty_objects.contains(bits))
      out.moved.push_back({bits});
    if (n.dirty_objects.contains(bits) && !n.attachments.contains(bits)) {
      Handle support;
      if (body.character) {
        auto id = body.character->GetGroundBodyID();
        auto found = n.handles.find(id.GetIndexAndSequenceNumber());
        if (body.character->IsSupported() && found != n.handles.end())
          support = found->second;
      } else if (body.spec.motion != Motion::Static) {
        for (const auto &[pair, contact] : n.listener.pairs) {
          (void)pair;
          if (contact.sensor)
            continue;
          if (contact.first.bits == bits && contact.normal.z < 0)
            support = contact.second;
          if (contact.second.bits == bits && contact.normal.z > 0)
            support = contact.first;
        }
        auto *existing = relations.supports.in.get(Handle{bits}.slot());
        if (!support && !n.physics.GetBodyInterface().IsActive(body.body) && existing &&
            !existing->empty())
          support = existing->front();
        if (!support) {
          auto b = n.world_bounds(body);
          Vec3 from{(b.min.x + b.max.x) * .5f, (b.min.y + b.max.y) * .5f,
                    b.min.z + n.settings.contact_slop_m};
          auto hit =
              raycast(space, identity, from, from + Vec3{0, 0, -n.settings.contact_slop_m * 3},
                      {{bits}, {}, false});
          if (hit)
            support = hit->object;
        }
      }
      auto *reverse = relations.supports.in.get(Handle{bits}.slot());
      auto old = reverse ? *reverse : std::vector<Handle>{};
      for (auto h : old)
        if (h != support)
          edge(relations.supports, RelationKind::Supports, h, {bits}, false, out.relations);
      if (support)
        edge(relations.supports, RelationKind::Supports, support, {bits}, true, out.relations);
    }
  }
  for (auto &volume : n.volumes)
    if (identity.valid(volume.spec.owner)) {
      auto target = compose({transforms.position[volume.spec.owner.slot()],
                             transforms.rotation[volume.spec.owner.slot()]},
                            volume.spec.local_pose);
      if (target.position != volume.world.position || target.rotation != volume.world.rotation) {
        const auto old_bounds =
            shape_bounds(n.geometries[volume.spec.geometry]->mesh, volume.world);
        n.dirty_bounds.push_back(old_bounds);
        n.wake(old_bounds);
        volume.world = target;
        ++n.volume_revision;
        n.physics.GetBodyInterface().SetPositionAndRotation(
            volume.body, jv(target.position), jq(target.rotation), JPH::EActivation::DontActivate);
        const auto bounds = shape_bounds(n.geometries[volume.spec.geometry]->mesh, volume.world);
        n.dirty_bounds.push_back(bounds);
        n.wake(bounds);
      }
    }
  membership(space, identity, parents, relations, out.relations, now, seconds);
  n.dirty_objects.clear();
  out.dirty_bounds.swap(n.dirty_bounds);
  out.contacts.swap(n.contacts);
  std::erase_if(out.contacts, [&](const Contact &c) {
    return !identity.valid(c.first) || !identity.valid(c.second);
  });
  for (auto &contact : out.contacts)
    if (contact.second.bits < contact.first.bits) {
      std::swap(contact.first, contact.second);
      contact.normal = contact.normal * -1;
    }
  // Preserve actual callback order, including start/end/start in one phase.
  // Virtual controllers and their inner rigid bodies can report the same state.
  std::map<std::pair<std::uint64_t, std::uint64_t>, bool> last_contact;
  std::erase_if(out.contacts, [&](const Contact &contact) {
    const auto key = std::pair{contact.first.bits, contact.second.bits};
    const auto state = contact.started;
    const auto found = last_contact.find(key);
    if (found != last_contact.end() && found->second == state)
      return true;
    last_contact[key] = state;
    return false;
  });
}
namespace {
MotionRow motion_row(const Space::Native &n, const Mirror &body) {
  JPH::Vec3 linear, angular;
  if (body.character) {
    linear = body.character->GetLinearVelocity();
    angular = JPH::Vec3::sZero();
  } else
    n.physics.GetBodyInterface().GetLinearAndAngularVelocity(body.body, linear, angular);
  return {body.spec.object, av(linear), av(angular),
          !body.character && !n.physics.GetBodyInterface().IsActive(body.body)};
}
} // namespace
void capture_motion(const Space &space, IdentityRead identity, std::vector<MotionRow> &out) {
  out.clear();
  auto &n = *space.native;
  for (const auto &[bits, body] : n.bodies)
    if (identity.valid({bits}) && (body.character || body.spec.motion == Motion::Dynamic))
      out.push_back(motion_row(n, body));
}
void capture_motion(const Space &space, IdentityRead identity, std::span<const Handle> selected,
                    std::vector<MotionRow> &out) {
  out.clear();
  const auto &n = *space.native;
  for (auto h : selected) {
    const auto body = n.bodies.find(h.bits);
    if (identity.valid(h) && body != n.bodies.end() &&
        (body->second.character || body->second.spec.motion == Motion::Dynamic))
      out.push_back(motion_row(n, body->second));
  }
}
std::optional<MotionRow> motion(const Space &space, IdentityRead identity, Handle h) {
  if (!identity.valid(h))
    return {};
  const auto &n = *space.native;
  const auto body = n.bodies.find(h.bits);
  if (body == n.bodies.end())
    return {};
  const auto parent = n.compound_parent.find(h.bits);
  if (parent == n.compound_parent.end())
    return motion_row(n, body->second);
  const auto &root = n.bodies.at(parent->second);
  auto row = motion_row(n, root);
  const auto offset = jv(body->second.pose.position) -
                      n.physics.GetBodyInterface().GetCenterOfMassPosition(root.body);
  row.linear_velocity_m_s =
      row.linear_velocity_m_s + av(jv(row.angular_velocity_rad_s).Cross(offset));
  row.object = h;
  return row;
}
void restore_motion(Space &space, IdentityRead identity, std::span<const MotionRow> rows) {
  auto &n = *space.native;
  for (const auto &row : rows) {
    auto *body = get(n, identity, row.object);
    if (!body)
      continue;
    if (body->character)
      body->character->SetLinearVelocity(jv(row.linear_velocity_m_s));
    else {
      n.physics.GetBodyInterface().SetLinearAndAngularVelocity(
          body->body, jv(row.linear_velocity_m_s), jv(row.angular_velocity_rad_s));
      if (row.asleep)
        n.physics.GetBodyInterface().DeactivateBody(body->body);
      else
        n.physics.GetBodyInterface().ActivateBody(body->body);
    }
    n.dirty_objects.insert(row.object.bits);
  }
}
void rebuild_attachments(Space &space, IdentityRead identity, TransformRead transforms,
                         ParentRead parents, const Edges &edges) {
  auto &n = *space.native;
  n.attachments.clear();
  for (std::size_t i = 0; i < edges.out.slot_of.size(); ++i) {
    auto slot = edges.out.slot_of[i];
    auto subject = identity.at(slot);
    if (!identity.valid(subject) || edges.out.value[i].empty())
      continue;
    auto parent = parents.parent[slot];
    if (!identity.valid(parent))
      continue;
    auto *body = get(n, identity, subject);
    if (!body && (!n.logical_roots.contains(subject.bits) || (identity.flags[slot] & Held)))
      continue;
    auto local = relative({transforms.position[parent.slot()], transforms.rotation[parent.slot()]},
                          {transforms.position[slot], transforms.rotation[slot]});
    n.attachments[subject.bits] = {subject, parent, local, (identity.flags[slot] & Held) != 0};
    if (!body)
      continue;
    if (!body->character)
      update_motion(n, *body, Motion::Kinematic);
    n.physics.GetBodyInterface().SetObjectLayer(
        body->body,
        n.attachments[subject.bits].grasp || body->character || !body->spec.collision_solid ? 4
                                                                                            : 1);
  }
}
void debug_lines(const Space &space, const DebugSelection &selection, std::vector<DebugLine> &out) {
  auto &n = *space.native;
  out.clear();
  auto wire = [&](const JPH::Shape *shape, Pose pose, std::uint32_t color) {
    JPH::Shape::GetTrianglesContext context;
    auto center = jv(pose.position) + jq(pose.rotation) * shape->GetCenterOfMass();
    shape->GetTrianglesStart(context, {jv(selection.region.min), jv(selection.region.max)}, center,
                             jq(pose.rotation), JPH::Vec3::sReplicate(1));
    JPH::Float3 vertices[192];
    for (;;) {
      const int triangles = shape->GetTrianglesNext(context, 64, vertices);
      if (!triangles)
        break;
      for (int triangle = 0; triangle < triangles; ++triangle)
        for (int edge = 0; edge < 3; ++edge) {
          const auto &a = vertices[triangle * 3 + edge];
          const auto &b = vertices[triangle * 3 + (edge + 1) % 3];
          out.push_back({{a.x, a.y, a.z}, {b.x, b.y, b.z}, color});
        }
    }
  };
  for (const auto &[key, body] : n.bodies) {
    (void)key;
    auto b = n.world_bounds(body);
    if (!overlaps(b, selection.region))
      continue;
    if (selection.collision) {
      auto shape = body.character ? body.character->GetShape()
                   : body.spec.motion == Motion::Static
                       ? n.geometries[body.spec.geometry]->mesh.GetPtr()
                       : n.geometries[body.spec.geometry]->hull.GetPtr();
      wire(shape, body.pose, 0x44ff66ff);
    }
    if (selection.paths)
      for (std::size_t i = 1; i < body.route.size(); ++i)
        out.push_back({body.route[i - 1], body.route[i], 0xffff44ff});
  }
  if (selection.volumes)
    for (const auto &volume : n.volumes) {
      auto shape = n.geometries[volume.spec.geometry]->mesh;
      auto matrix =
          JPH::RMat44::sRotationTranslation(jq(volume.world.rotation), jv(volume.world.position))
              .PreTranslated(shape->GetCenterOfMass());
      auto b = shape->GetWorldSpaceBounds(matrix, JPH::Vec3::sReplicate(1));
      Bounds bounds{av(b.mMin), av(b.mMax)};
      if (overlaps(bounds, selection.region))
        wire(shape, volume.world, 0x4488ffff);
    }
  if (selection.navigation)
    for (int i = 0; i < n.mesh->getMaxTiles(); ++i) {
      const dtMeshTile *tile = static_cast<const dtNavMesh *>(n.mesh)->getTile(i);
      if (!tile || !tile->header)
        continue;
      for (int p = 0; p < tile->header->polyCount; ++p) {
        const auto &poly = tile->polys[p];
        for (unsigned edge = 0; edge < poly.vertCount; ++edge) {
          auto a = tile->verts + poly.verts[edge] * 3,
               b = tile->verts + poly.verts[(edge + 1) % poly.vertCount] * 3;
          auto first = world({a[0], a[1], a[2]}), second = world({b[0], b[1], b[2]});
          Bounds line{{std::min(first.x, second.x), std::min(first.y, second.y),
                       std::min(first.z, second.z)},
                      {std::max(first.x, second.x), std::max(first.y, second.y),
                       std::max(first.z, second.z)}};
          if (overlaps(line, selection.region))
            out.push_back({first, second, 0x44ffffff});
        }
      }
    }
}
} // namespace amber::space
