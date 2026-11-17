#include "private.hpp"
#include <Jolt/Physics/Body/BodyLock.h>

#include <DetourNavMeshBuilder.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Recast.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace amber::space {
namespace {
template <class T, void (*Free)(T *)> using Rc = std::unique_ptr<T, decltype(Free)>;
Vec3 goal_position(const State &state, IdentityRead identity, const Goal &goal) {
  if (const auto *point = std::get_if<PointGoal>(&goal))
    return point->point;
  const auto target = bounds(state, identity, std::get<ObjectGoal>(goal).target);
  return target ? (target->min + target->max) * .5f : Vec3{};
}
void include(Bounds &bounds, Vec3 point) {
  bounds.min = {std::min(bounds.min.x, point.x), std::min(bounds.min.y, point.y),
                std::min(bounds.min.z, point.z)};
  bounds.max = {std::max(bounds.max.x, point.x), std::max(bounds.max.y, point.y),
                std::max(bounds.max.z, point.z)};
}
bool overlaps_xy(Bounds a, Bounds b) {
  return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y;
}
Bounds instance_bounds(const NavInstance &entry) {
  const auto &b = entry.geometry->bounds;
  Bounds result{{INFINITY, INFINITY, INFINITY}, {-INFINITY, -INFINITY, -INFINITY}};
  for (unsigned i = 0; i < 8; ++i) {
    Vec3 p{(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z};
    p = compose(entry.pose, {p, {}}).position;
    result.min = {std::min(result.min.x, p.x), std::min(result.min.y, p.y),
                  std::min(result.min.z, p.z)};
    result.max = {std::max(result.max.x, p.x), std::max(result.max.y, p.y),
                  std::max(result.max.z, p.z)};
  }
  return result;
}
Path route(const Space::Native &n, Vec3 from, Vec3 to) {
  Path result{PathStatus::NoSurface, {}, 0};
  auto a = nav(from), b = nav(to);
  const auto &s = n.settings.navigation;
  float extents[]{s.radius_m + s.cell_m, s.height_m, s.radius_m + s.cell_m};
  dtPolyRef start = 0, end = 0;
  float p[3], q[3];
  dtQueryFilter filter;
  n.query->findNearestPoly(&a.x, extents, &filter, &start, p);
  n.query->findNearestPoly(&b.x, extents, &filter, &end, q);
  if (!start || !end)
    return result;
  std::vector<dtPolyRef> polys(s.polygons_per_path);
  int count = 0;
  auto status =
      n.query->findPath(start, end, p, q, &filter, polys.data(), &count, int(polys.size()));
  if (dtStatusFailed(status) || !count || polys[count - 1] != end ||
      dtStatusDetail(status, DT_PARTIAL_RESULT) || dtStatusDetail(status, DT_BUFFER_TOO_SMALL)) {
    result.status = PathStatus::Unreachable;
    return result;
  }
  std::vector<float> points(polys.size() * 3);
  Vec3 previous = from;
  // The pinned Crowd corridor contains 256 polygons. Intermediate goals use half
  // that range, while the global Detour query can span the full configured path.
  for (int begin = 0; begin < count;) {
    const int finish = std::min(begin + 127, count - 1);
    float target[3]{};
    if (finish == count - 1)
      std::copy_n(q, 3, target);
    else {
      const dtMeshTile *tile;
      const dtPoly *poly;
      n.mesh->getTileAndPolyByRefUnsafe(polys[finish], &tile, &poly);
      for (unsigned vertex = 0; vertex < poly->vertCount; ++vertex)
        for (int axis = 0; axis < 3; ++axis)
          target[axis] += tile->verts[poly->verts[vertex] * 3 + axis] / poly->vertCount;
    }
    int corners = 0;
    status =
        n.query->findStraightPath(p, target, polys.data() + begin, finish - begin + 1,
                                  points.data(), nullptr, nullptr, &corners, int(polys.size()));
    if (dtStatusFailed(status)) {
      result.status = PathStatus::Unreachable;
      return result;
    }
    for (int i = 0; i < corners; ++i) {
      auto point = world({points[i * 3], points[i * 3 + 1], points[i * 3 + 2]});
      result.length_m += (point - previous).length();
      previous = point;
      if (result.corners.empty() || result.corners.back() != point)
        result.corners.push_back(point);
    }
    if (finish == count - 1)
      break;
    begin = finish;
    std::copy_n(target, 3, p);
  }
  result.status = PathStatus::Ready;
  return result;
}
} // namespace

NavResult build_tile(const NavJob &job, const NavSettings &s) {
  NavResult result{job.tile, job.revision, {}, {}};
  if (job.instances.empty())
    return result;
  std::vector<float> vertices;
  std::vector<int> indices;
  std::vector<unsigned char> areas;
  float low = INFINITY, high = -INFINITY;
  for (const auto &entry : job.instances) {
    const int offset = int(vertices.size() / 3);
    for (auto p : entry.geometry->vertices) {
      p = nav(compose(entry.pose, {p, {}}).position);
      vertices.insert(vertices.end(), {p.x, p.y, p.z});
      low = std::min(low, p.y);
      high = std::max(high, p.y);
    }
    auto begin = indices.size();
    for (int index : entry.geometry->indices)
      indices.push_back(index + offset);
    areas.resize(indices.size() / 3, RC_NULL_AREA);
    if (entry.walkable)
      rcMarkWalkableTriangles(nullptr, s.slope_degrees, vertices.data(), int(vertices.size() / 3),
                              indices.data() + begin, int((indices.size() - begin) / 3),
                              areas.data() + begin / 3);
  }
  if (indices.empty())
    return result;
  rcConfig cfg{};
  cfg.cs = s.cell_m;
  cfg.ch = s.cell_height_m;
  cfg.walkableSlopeAngle = s.slope_degrees;
  cfg.walkableHeight = int(std::ceil(s.height_m / s.cell_height_m));
  cfg.walkableClimb = int(std::floor(s.climb_m / s.cell_height_m));
  cfg.walkableRadius = int(std::ceil(s.radius_m / s.cell_m));
  cfg.maxEdgeLen = int(s.max_edge_m / s.cell_m);
  cfg.maxSimplificationError = s.contour_error_m / s.cell_m;
  cfg.minRegionArea = int(s.min_region_cells);
  cfg.mergeRegionArea = int(s.merge_region_cells);
  cfg.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
  cfg.tileSize = int(s.tile_cells);
  cfg.borderSize = cfg.walkableRadius + 3;
  cfg.width = cfg.tileSize + cfg.borderSize * 2;
  cfg.height = cfg.width;
  cfg.detailSampleDist = s.detail_sample_m;
  cfg.detailSampleMaxError = s.detail_error_m;
  const float width = s.cell_m * s.tile_cells, pad = cfg.borderSize * s.cell_m;
  cfg.bmin[0] = job.tile.x * width - pad;
  cfg.bmin[1] = low - s.cell_height_m * 2;
  cfg.bmin[2] = job.tile.y * width - pad;
  cfg.bmax[0] = (job.tile.x + 1) * width + pad;
  cfg.bmax[1] = high + s.height_m + s.climb_m;
  cfg.bmax[2] = (job.tile.y + 1) * width + pad;
  rcContext context(false);
  Rc<rcHeightfield, rcFreeHeightField> height(rcAllocHeightfield(), rcFreeHeightField);
  Rc<rcCompactHeightfield, rcFreeCompactHeightfield> compact(rcAllocCompactHeightfield(),
                                                             rcFreeCompactHeightfield);
  Rc<rcContourSet, rcFreeContourSet> contours(rcAllocContourSet(), rcFreeContourSet);
  Rc<rcPolyMesh, rcFreePolyMesh> mesh(rcAllocPolyMesh(), rcFreePolyMesh);
  Rc<rcPolyMeshDetail, rcFreePolyMeshDetail> detail(rcAllocPolyMeshDetail(), rcFreePolyMeshDetail);
  auto fail = [&](const char *stage) {
    result.error = stage;
    return result;
  };
  if (!rcCreateHeightfield(&context, *height, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs,
                           cfg.ch))
    return fail("create heightfield");
  if (!rcRasterizeTriangles(&context, vertices.data(), int(vertices.size() / 3), indices.data(),
                            areas.data(), int(indices.size() / 3), *height, cfg.walkableClimb))
    return fail("rasterize");
  rcFilterLowHangingWalkableObstacles(&context, cfg.walkableClimb, *height);
  rcFilterLedgeSpans(&context, cfg.walkableHeight, cfg.walkableClimb, *height);
  rcFilterWalkableLowHeightSpans(&context, cfg.walkableHeight, *height);
  if (!rcBuildCompactHeightfield(&context, cfg.walkableHeight, cfg.walkableClimb, *height,
                                 *compact))
    return fail("compact heightfield");
  if (!rcErodeWalkableArea(&context, cfg.walkableRadius, *compact))
    return fail("erode");
  if (!rcBuildDistanceField(&context, *compact))
    return fail("distance field");
  if (!rcBuildRegions(&context, *compact, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea))
    return fail("regions");
  if (!rcBuildContours(&context, *compact, cfg.maxSimplificationError, cfg.maxEdgeLen, *contours))
    return fail("contours");
  if (!rcBuildPolyMesh(&context, *contours, cfg.maxVertsPerPoly, *mesh))
    return fail("poly mesh");
  if (!mesh->npolys)
    return result;
  if (!rcBuildPolyMeshDetail(&context, *mesh, *compact, cfg.detailSampleDist,
                             cfg.detailSampleMaxError, *detail))
    return fail("detail mesh");
  for (int i = 0; i < mesh->npolys; ++i) {
    mesh->flags[i] = 1;
    mesh->areas[i] = 0;
  }
  dtNavMeshCreateParams params{};
  params.verts = mesh->verts;
  params.vertCount = mesh->nverts;
  params.polys = mesh->polys;
  params.polyAreas = mesh->areas;
  params.polyFlags = mesh->flags;
  params.polyCount = mesh->npolys;
  params.nvp = mesh->nvp;
  params.detailMeshes = detail->meshes;
  params.detailVerts = detail->verts;
  params.detailVertsCount = detail->nverts;
  params.detailTris = detail->tris;
  params.detailTriCount = detail->ntris;
  params.walkableHeight = s.height_m;
  params.walkableRadius = s.radius_m;
  params.walkableClimb = s.climb_m;
  params.tileX = job.tile.x;
  params.tileY = job.tile.y;
  std::copy_n(mesh->bmin, 3, params.bmin);
  std::copy_n(mesh->bmax, 3, params.bmax);
  params.cs = s.cell_m;
  params.ch = s.cell_height_m;
  params.buildBvTree = true;
  unsigned char *data = nullptr;
  int size = 0;
  if (!dtCreateNavMeshData(&params, &data, &size))
    return fail("Detour tile");
  result.bytes.assign(data, data + size);
  dtFree(data);
  return result;
}

void start_navigation_workers(Space::Native &n) {
  for (std::uint32_t i = 0; i < n.settings.navigation.workers; ++i)
    n.nav_workers.emplace_back([&n] {
      for (;;) {
        NavJob job;
        {
          std::unique_lock lock(n.nav_mutex);
          n.nav_condition.wait(lock, [&] { return n.stopping || !n.nav_jobs.empty(); });
          if (n.stopping)
            return;
          job = std::move(n.nav_jobs.front());
          n.nav_jobs.pop_front();
        }
        auto result = build_tile(job, n.settings.navigation);
        {
          std::lock_guard lock(n.nav_mutex);
          n.nav_results.push_back(std::move(result));
        }
      }
    });
}
void stop_navigation_workers(Space::Native &n) {
  {
    std::lock_guard lock(n.nav_mutex);
    n.stopping = true;
  }
  n.nav_condition.notify_all();
  for (auto &worker : n.nav_workers)
    worker.join();
}
void Space::Native::dirty_navigation(Bounds b) {
  const auto &s = settings.navigation;
  float width = s.cell_m * s.tile_cells;
  float pad = s.radius_m + s.cell_m * 3;
  auto a = nav({b.min.x - pad, b.max.y + pad, b.min.z}),
       c = nav({b.max.x + pad, b.min.y - pad, b.max.z});
  for (int x = int(std::floor(a.x / width)); x <= int(std::floor(c.x / width)); ++x)
    for (int y = int(std::floor(a.z / width)); y <= int(std::floor(c.z / width)); ++y)
      dirty_tiles[{x, y}] = ++geometry_revision;
}
NavigationInstall install_ready_navigation(Space &space) {
  auto &n = *space.native;
  NavigationInstall output{n.revision, {}, {}, {}};
  for (;;) {
    NavResult result;
    {
      std::lock_guard lock(n.nav_mutex);
      if (n.nav_results.empty())
        break;
      result = std::move(n.nav_results.front());
      n.nav_results.pop_front();
    }
    n.scheduled_tiles.erase(result.tile);
    auto current = n.dirty_tiles.find(result.tile);
    if (current == n.dirty_tiles.end() || current->second != result.revision)
      continue;
    if (!result.error.empty()) {
      output.errors.push_back(result.error);
      n.dirty_tiles.erase(current);
      continue;
    }
    auto old = n.mesh->getTileRefAt(result.tile.x, result.tile.y, 0);
    if (old)
      n.mesh->removeTile(old, nullptr, nullptr);
    if (!result.bytes.empty()) {
      auto *bytes = static_cast<unsigned char *>(dtAlloc(result.bytes.size(), DT_ALLOC_PERM));
      std::memcpy(bytes, result.bytes.data(), result.bytes.size());
      if (dtStatusFailed(
              n.mesh->addTile(bytes, int(result.bytes.size()), DT_TILE_FREE_DATA, 0, nullptr))) {
        dtFree(bytes);
        output.errors.push_back("install Detour tile");
      }
    }
    n.dirty_tiles.erase(current);
    output.installed.push_back(result.tile);
    ++n.revision;
    const float width = n.settings.navigation.tile_cells * n.settings.navigation.cell_m;
    const Bounds region{{result.tile.x * width, -(result.tile.y + 1) * width, 0},
                        {(result.tile.x + 1) * width, -result.tile.y * width, 0}};
    for (auto bits : n.character_handles) {
      auto &body = n.bodies.at(bits);
      if (body.intent && overlaps_xy(body.route_bounds, region))
        body.route_dirty = true;
    }
  }
  for (const auto &[tile, revision] : n.dirty_tiles) {
    output.pending.push_back(tile);
    if (n.scheduled_tiles.contains(tile))
      continue;
    NavJob job{tile, revision, {}};
    const float width = n.settings.navigation.tile_cells * n.settings.navigation.cell_m;
    const float pad = n.settings.navigation.radius_m + n.settings.navigation.cell_m * 3;
    Bounds region{{tile.x * width - pad, -(tile.y + 1) * width - pad, -INFINITY},
                  {(tile.x + 1) * width + pad, -tile.y * width + pad, INFINITY}};
    JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> candidates;
    n.physics.GetBroadPhaseQuery().CollideAABox({jv(region.min), jv(region.max)}, candidates);
    std::set<std::uint64_t> copied;
    for (auto id : candidates.mHits) {
      auto owner = n.handles.find(id.GetIndexAndSequenceNumber());
      if (owner == n.handles.end()) {
        JPH::BodyLockRead lock(n.physics.GetBodyLockInterface(), id);
        if (lock.Succeeded() && lock.GetBody().GetUserData() == 0 && n.terrain_geometry) {
          const auto center = av(lock.GetBody().GetPosition());
          const auto size = n.settings.terrain_tile_m;
          NavInstance tile{
              n.terrain_geometry, {{center.x - size / 2, center.y - size / 2, 0}, {}}, true};
          if (overlaps(instance_bounds(tile), region))
            job.instances.push_back(std::move(tile));
        }
        continue;
      }
      if (!copied.insert(owner->second.bits).second)
        continue;
      auto found = n.bodies.find(owner->second.bits);
      if (found == n.bodies.end())
        continue;
      const auto &body = found->second;
      auto attachment = n.attachments.find(body.spec.object.bits);
      if (!body.spec.collision_solid || n.compound_parent.contains(body.spec.object.bits) ||
          body.spec.motion == Motion::Character ||
          (attachment != n.attachments.end() && attachment->second.grasp) ||
          !(body.spec.navigation_surface || body.spec.navigation_obstacle))
        continue;
      NavInstance instance{n.geometries[body.spec.geometry], body.pose,
                           body.spec.navigation_surface};
      if (overlaps(instance_bounds(instance), region))
        job.instances.push_back(std::move(instance));
    }
    n.scheduled_tiles[tile] = revision;
    {
      std::lock_guard lock(n.nav_mutex);
      n.nav_jobs.push_back(std::move(job));
    }
    n.nav_condition.notify_one();
  }
  output.revision = n.revision;
  return output;
}
std::uint64_t navigation_revision(const Space &space) {
  return space.native->revision;
}

bool needs_plan(const State &state, IdentityRead identity, const MoveRequest &request) {
  const auto &n = *state.space.native;
  const auto found = n.bodies.find(request.body.bits);
  if (!identity.valid(request.body) || found == n.bodies.end() || !found->second.character ||
      n.attachments.contains(request.body.bits))
    return false;
  if (const auto *target = std::get_if<ObjectGoal>(&request.goal);
      target && !identity.valid(target->target))
    return false;
  const auto &body = found->second;
  if (body.intent != request.intent)
    return true;
  if (body.character->GetGroundState() != JPH::CharacterBase::EGroundState::OnGround)
    return false;
  const auto position = body.pose.position;
  return body.route_dirty ||
         (body.last_goal - goal_position(state, identity, request.goal)).length() >
             n.settings.crowd_correction_m ||
         position.x < body.route_bounds.min.x || position.x > body.route_bounds.max.x ||
         position.y < body.route_bounds.min.y || position.y > body.route_bounds.max.y ||
         (body.path_status == PathStatus::PendingTiles && n.dirty_tiles.empty());
}
void request_corner(Space::Native &n, Mirror &body) {
  if (body.agent < 0)
    return;
  if (body.waypoint + 1 < body.route.size()) {
    // Intermediate corners are through points; only the final goal slows the walk.
    auto velocity = nav(body.route[body.waypoint] - body.pose.position);
    velocity.y = 0;
    const float distance = velocity.length();
    if (distance > 0)
      velocity = velocity * (n.crowd->getAgent(body.agent)->params.maxSpeed / distance);
    n.crowd->requestMoveVelocity(body.agent, &velocity.x);
    return;
  }
  auto target = nav(body.route[body.waypoint]);
  dtPolyRef ref = 0;
  float nearest[3], extents[]{n.settings.navigation.radius_m, n.settings.navigation.height_m,
                              n.settings.navigation.radius_m};
  dtQueryFilter filter;
  n.query->findNearestPoly(&target.x, extents, &filter, &ref, nearest);
  if (ref)
    n.crowd->requestMoveTarget(body.agent, ref, nearest);
}
void plan_move(State &state, IdentityRead identity, const MoveRequest &request,
               std::span<const TravelBoundary> forbidden) {
  auto &n = *state.space.native;
  auto &body = n.bodies.at(request.body.bits);
  if (body.intent != request.intent)
    body.stuck_s = 0;
  auto result = path(state, identity, request.body, request.goal, forbidden);
  body.route = std::move(result.corners);
  body.waypoint = 0;
  body.path_status = result.status;
  body.intent = request.intent;
  body.route_dirty = false;
  body.last_goal = goal_position(state, identity, request.goal);
  body.membership.due = 0;

  body.route_bounds = {body.pose.position, body.pose.position};
  include(body.route_bounds, body.last_goal);
  for (auto point : body.route)
    include(body.route_bounds, point);
  // Plot boundaries and navigation tiles are vertical columns. A point-only
  // box has zero width on a straight walk, and Detour's floor Z differs from
  // Jolt's. Use the character's footprint so ordinary crowd/floor motion fits.
  const auto &c = *body.spec.character;
  body.route_bounds.min.x -= c.radius_m;
  body.route_bounds.min.y -= c.radius_m;
  body.route_bounds.max.x += c.radius_m;
  body.route_bounds.max.y += c.radius_m;
  body.forbidden.clear();
  for (const auto &boundary : forbidden) {
    Bounds region{{INFINITY, INFINITY, 0}, {-INFINITY, -INFINITY, 0}};
    for (auto point : boundary.polygon)
      include(region, point);
    if (overlaps_xy(body.route_bounds, region))
      body.forbidden.push_back(boundary);
  }
  if (body.path_status != PathStatus::Ready) {
    if (body.agent >= 0)
      n.crowd->resetMoveTarget(body.agent);
    return;
  }
  if (body.agent < 0) {
    auto position = nav(body.pose.position);
    dtCrowdAgentParams params{};
    params.radius = c.radius_m;
    params.height = c.height_m;
    params.maxAcceleration = c.acceleration_m_s2;
    params.maxSpeed = c.max_speed_m_s;
    params.collisionQueryRange = 0;
    params.pathOptimizationRange = c.radius_m * n.settings.crowd_optimize_radius_multiplier;
    params.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO;
    body.agent = n.crowd->addAgent(&position.x, &params);
  }
  while (body.waypoint + 1 < body.route.size() &&
         (body.route[body.waypoint] - body.pose.position).length() < c.radius_m)
    ++body.waypoint;
  request_corner(n, body);
}

bool travel_blocked(Vec3 a, Vec3 b, std::span<const TravelBoundary> regions) {
  for (const auto &region : regions) {
    if (region.polygon.size() < 3)
      continue;
    float orientation = 0;
    for (std::size_t i = 0; i < region.polygon.size(); ++i) {
      auto p = region.polygon[i], q = region.polygon[(i + 1) % region.polygon.size()];
      orientation += p.x * q.y - q.x * p.y;
    }
    const float sign = orientation >= 0 ? 1.f : -1.f;
    float enter = 0, leave = 1;
    auto clip = [&](float value, float delta) {
      if (delta == 0)
        return value >= 0;
      if (delta > 0)
        enter = std::max(enter, -value / delta);
      else
        leave = std::min(leave, -value / delta);
      return enter <= leave;
    };
    bool hit = clip(a.z - region.min_z, b.z - a.z) && clip(region.max_z - a.z, a.z - b.z);
    for (std::size_t i = 0; hit && i < region.polygon.size(); ++i) {
      auto p = region.polygon[i], q = region.polygon[(i + 1) % region.polygon.size()];
      auto e = q - p;
      hit = clip(sign * (e.x * (a.y - p.y) - e.y * (a.x - p.x)),
                 sign * (e.x * (b.y - a.y) - e.y * (b.x - a.x)));
    }
    if (hit && leave >= 0 && enter <= 1)
      return true;
  }
  return false;
}
Path path(const State &state, IdentityRead identity, Handle body, const Goal &goal,
          std::span<const TravelBoundary> forbidden) {
  const auto &n = *state.space.native;
  Path result{PathStatus::NoSurface, {}, 0};
  auto found = n.bodies.find(body.bits);
  if (!identity.valid(body) || found == n.bodies.end())
    return result;
  const Vec3 from = found->second.pose.position;
  std::vector<Vec3> candidates;
  const auto *object = std::get_if<ObjectGoal>(&goal);
  std::optional<Bounds> target;
  if (const auto *point = std::get_if<PointGoal>(&goal))
    candidates.push_back(point->point);
  else {
    target = bounds(state, identity, object->target);
    if (!target)
      return result;
    const auto b = *target;
    float pad = n.settings.navigation.radius_m + n.settings.sweep_skin_m;
    const float xs[]{b.min.x - pad, (b.min.x + b.max.x) * .5f, b.max.x + pad};
    const float ys[]{b.min.y - pad, (b.min.y + b.max.y) * .5f, b.max.y + pad};
    for (int x = 0; x < 3; ++x)
      for (int y = 0; y < 3; ++y)
        if (x != 1 || y != 1)
          candidates.push_back({xs[x], ys[y], b.min.z});
    candidates.push_back(from);
  }
  float shortest = INFINITY;
  bool denied = false;
  for (auto to : candidates) {
    auto attempt = route(n, from, to);
    if (attempt.status != PathStatus::Ready)
      continue;
    if (object) {
      const auto &b = *target;
      auto p = attempt.corners.back();
      Vec3 nearest{std::clamp(p.x, b.min.x, b.max.x), std::clamp(p.y, b.min.y, b.max.y),
                   std::clamp(p.z, b.min.z, b.max.z)};
      if ((p - nearest).length() > object->reach_m)
        continue;
    }
    bool blocked = false;
    Vec3 previous = from;
    for (auto p : attempt.corners) {
      if (travel_blocked(previous, p, forbidden)) {
        blocked = true;
        break;
      }
      previous = p;
    }
    if (blocked) {
      denied = true;
      continue;
    }
    if (attempt.length_m < shortest) {
      shortest = attempt.length_m;
      result = std::move(attempt);
    }
  }
  if (!std::isfinite(shortest))
    result.status =
        denied ? PathStatus::AccessDenied
               : (!n.dirty_tiles.empty() ? PathStatus::PendingTiles : PathStatus::Unreachable);
  return result;
}

NavigationReport diagnose_navigation(const Space &space, std::span<const Bounds> regions,
                                     Vec3 reference) {
  auto &n = *space.native;
  NavigationReport result{n.revision, false, false, {}};
  if (!n.dirty_tiles.empty())
    return result;
  auto &cache = n.connectivity;
  if (cache.revision != n.revision) {
    cache = {};
    cache.revision = n.revision;
  }
  // Labels are derived from the current tile revision. Keep the traversal cursor
  // so a large rebuild can finish over several bounded writer turns.
  while (!cache.complete) {
    if (!cache.frontier.empty()) {
      const auto next = cache.frontier.front();
      cache.frontier.pop_front();
      const dtMeshTile *tile;
      const dtPoly *poly;
      n.mesh->getTileAndPolyByRefUnsafe(next, &tile, &poly);
      for (auto link = poly->firstLink; link != DT_NULL_LINK; link = tile->links[link].next) {
        const auto neighbor = tile->links[link].ref;
        if (neighbor && !cache.labels.contains(neighbor)) {
          cache.labels[neighbor] = cache.component;
          cache.frontier.push_back(neighbor);
        }
      }
      continue;
    }
    if (cache.tile == n.mesh->getMaxTiles()) {
      cache.complete = true;
      break;
    }
    const auto *tile = static_cast<const dtNavMesh *>(n.mesh)->getTile(cache.tile);
    if (!tile || !tile->header || cache.polygon == tile->header->polyCount) {
      ++cache.tile;
      cache.polygon = 0;
      continue;
    }
    const auto ref = n.mesh->getPolyRefBase(tile) | dtPolyRef(cache.polygon++);
    if (!cache.labels.contains(ref)) {
      cache.labels[ref] = ++cache.component;
      cache.frontier.push_back(ref);
    }
  }
  const auto &s = n.settings.navigation;
  const auto at = nav(reference);
  const float reference_extents[]{s.radius_m + s.cell_m, s.height_m, s.radius_m + s.cell_m};
  dtQueryFilter filter;
  dtPolyRef start = 0;
  float nearest[3];
  n.query->findNearestPoly(&at.x, reference_extents, &filter, &start, nearest);
  result.reference_navigable = start != 0;
  const auto reference_component = start ? cache.labels.at(start) : 0;
  struct Areas final : dtPolyQuery {
    const Space::Native &native;
    std::uint32_t reference;
    std::map<std::uint32_t, NavigationArea> rows;
    Vec3 center;
    Areas(const Space::Native &n, std::uint32_t component) : native(n), reference(component) {}
    void process(const dtMeshTile *, dtPoly **, dtPolyRef *refs, int count) override {
      for (int i = 0; i < count; ++i) {
        const auto component = native.connectivity.labels.at(refs[i]);
        if (!rows.contains(component)) {
          float point[3];
          native.query->closestPointOnPoly(refs[i], &center.x, point, nullptr);
          rows.emplace(component, NavigationArea{world({point[0], point[1], point[2]}), component,
                                                 component == reference});
        }
      }
    }
  } areas(n, reference_component);
  for (const auto &region : regions) {
    areas.center = nav((region.min + region.max) * .5f);
    const auto extent = (region.max - region.min) * .5f;
    const float pad = s.radius_m + s.cell_m;
    const float extents[]{extent.x + pad, extent.z + s.cell_height_m * 2, extent.y + pad};
    if (dtStatusFailed(n.query->queryPolygons(&areas.center.x, extents, &filter, &areas)))
      return result;
  }
  for (const auto &[component, area] : areas.rows) {
    (void)component;
    result.areas.push_back(area);
  }
  result.complete = true;
  return result;
}
} // namespace amber::space
