#pragma once
#include <Jolt/Jolt.h>

#include "space/space.hpp"
#include <Jolt/Core/Reference.h>
#include <Jolt/Core/StreamOut.h>
#include <Jolt/Core/StreamUtils.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <thread>

using namespace amber;
namespace sp = amber::space;
namespace testspace {
inline void require(bool value, const char *text) {
  if (!value)
    throw std::runtime_error(text);
}
inline sp::Settings settings() {
  auto config = toml::parse_file(AMBER_CONFIG_PATH);
  auto p = config["space"], n = config["navigation"];
  auto f = [](toml::node_view<toml::node> v) { return float(v.value<double>().value()); };
  auto i = [](toml::node_view<toml::node> v) {
    return std::uint32_t(v.value<std::int64_t>().value());
  };
  auto *gravity = p["GRAVITY_M_S2"].as_array();
  return {
      {float((*gravity)[0].value<double>().value()), float((*gravity)[1].value<double>().value()),
       float((*gravity)[2].value<double>().value())},
      {f(n["CELL_M"]), f(n["CELL_HEIGHT_M"]), i(n["TILE_CELLS"]), f(n["RADIUS_M"]),
       f(n["HEIGHT_M"]), f(n["CLIMB_M"]), f(n["SLOPE_DEGREES"]), f(n["MAX_EDGE_M"]),
       f(n["CONTOUR_ERROR_M"]), f(n["DETAIL_SAMPLE_M"]), f(n["DETAIL_ERROR_M"]),
       i(n["MIN_REGION_CELLS"]), i(n["MERGE_REGION_CELLS"]), i(n["NODES_PER_QUERY"]),
       i(n["POLYGONS_PER_PATH"]), i(n["WORKERS"]), i(n["MAX_TILES"]), i(n["POLYGONS_PER_TILE"])},
      i(p["MAX_BODIES"]),
      i(p["MAX_BODY_PAIRS"]),
      i(p["MAX_CONTACTS"]),
      i(p["TEMP_BYTES"]),
      f(p["CONTACT_SLOP_M"]),
      f(p["SWEEP_SKIN_M"]),
      f(p["CROWD_CORRECTION_M"]),
      f(p["BLOCKED_AFTER_S"]),
      i(p["MAX_CHARACTERS"]),
      f(n["CROWD_QUERY_RADIUS_MULTIPLIER"]),
      f(n["CROWD_OPTIMIZE_RADIUS_MULTIPLIER"]),
      f(n["CROWD_SEPARATION_WEIGHT"])};
}
class Output final : public JPH::StreamOut {
public:
  std::vector<std::byte> bytes;
  void WriteBytes(const void *data, std::size_t count) override {
    auto first = static_cast<const std::byte *>(data);
    bytes.insert(bytes.end(), first, first + count);
  }
  bool IsFailed() const override { return false; }
};
std::vector<std::byte> saved(const JPH::Shape *shape) {
  Output stream;
  JPH::Shape::ShapeToIDMap shapes;
  JPH::Shape::MaterialToIDMap materials;
  shape->SaveWithChildren(stream, shapes, materials);
  return stream.bytes;
}
struct Box {
  std::vector<std::byte> mesh, hull, triangles;
  sp::Bounds bounds;
  explicit Box(Vec3 size)
      : bounds{{-size.x * .5f, -size.y * .5f, 0}, {size.x * .5f, size.y * .5f, size.z}} {
    std::vector<Vec3> vertices{
        {bounds.min.x, bounds.min.y, 0},      {bounds.max.x, bounds.min.y, 0},
        {bounds.max.x, bounds.max.y, 0},      {bounds.min.x, bounds.max.y, 0},
        {bounds.min.x, bounds.min.y, size.z}, {bounds.max.x, bounds.min.y, size.z},
        {bounds.max.x, bounds.max.y, size.z}, {bounds.min.x, bounds.max.y, size.z}};
    std::vector<std::uint32_t> indices{0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                       1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
    JPH::VertexList points;
    JPH::Array<JPH::Vec3> convex;
    for (auto p : vertices) {
      points.emplace_back(p.x, p.y, p.z);
      convex.emplace_back(p.x, p.y, p.z);
    }
    JPH::IndexedTriangleList faces;
    for (std::size_t k = 0; k < indices.size(); k += 3)
      faces.emplace_back(indices[k], indices[k + 1], indices[k + 2]);
    auto static_shape = JPH::MeshShapeSettings(points, faces).Create();
    auto dynamic_shape = JPH::ConvexHullShapeSettings(convex).Create();
    require(!static_shape.HasError() && !dynamic_shape.HasError(), "fixture shapes failed");
    mesh = saved(static_shape.Get());
    hull = saved(dynamic_shape.Get());
    triangles.resize(16 + vertices.size() * 12 + indices.size() * 4);
    std::memcpy(triangles.data(), "AMBMESH1", 8);
    auto vc = std::uint32_t(vertices.size()), ic = std::uint32_t(indices.size());
    std::memcpy(triangles.data() + 8, &vc, 4);
    std::memcpy(triangles.data() + 12, &ic, 4);
    std::memcpy(triangles.data() + 16, vertices.data(), vertices.size() * 12);
    std::memcpy(triangles.data() + 16 + vertices.size() * 12, indices.data(), indices.size() * 4);
  }
  sp::GeometrySource source() const { return {mesh, hull, triangles, bounds}; }
};
struct Scene {
  amber::ObjectState objects_state;

  sp::Settings config;
  sp::Space space;
  explicit Scene(sp::Settings value = settings()) : config(value), space(config) {}
  Objects &objects = objects_state.objects;
  sp::State space_state{float(
      toml::parse_file(AMBER_CONFIG_PATH)["perception"]["LIGHT_CELL_M"].value<double>().value())};
  sp::PhysicalRelations &relations = space_state.physical_relations;
  sp::StepResult result;
  Step clock = 0;
  float dt =
      float(1. / toml::parse_file(AMBER_CONFIG_PATH)["clock"]["STEP_HZ"].value<double>().value());
  Handle box(Vec3 size, Vec3 position, sp::Motion motion = sp::Motion::Static,
             float transmission = 0, bool nav = true, bool solid = true) {
    Box shape(size);
    auto imported = sp::import_geometry(space, shape.source());
    require(imported.geometry != 0, imported.error.c_str());
    ObjectInit init;
    init.model = 1;
    init.material = 1;
    init.position = position;
    init.flags = motion == sp::Motion::Dynamic     ? Dynamic
                 : motion == sp::Motion::Kinematic ? ScriptedMotion
                                                   : Static;
    auto h = create(objects, init);
    require(sp::add(space, identity(objects), transforms(objects),
                    {h, imported.geometry, motion, {.6f, 0, transmission}, 10, nav, nav, {}, solid})
                    .status == sp::Status::Ok,
            "box add failed");
    return h;
  }
  Handle character(Vec3 position) {
    Box shape({.6f, .6f, 1.8f});
    auto imported = sp::import_geometry(space, shape.source());
    ObjectInit init;
    init.model = 2;
    init.flags = Body;
    init.position = position;
    auto h = create(objects, init);
    sp::CharacterSpec profile{.3f, 1.8f, 70, 100, 1.8f, 8, .24f, std::numbers::pi_v<float> / 4,
                              .3f};
    require(
        sp::add(
            space, identity(objects), transforms(objects),
            {h, imported.geometry, sp::Motion::Character, {.6f, 0, 0}, 70, false, false, profile})
                .status == sp::Status::Ok,
        "character add failed");
    return h;
  }
  void tick(std::span<const sp::MoveRequest> moves = {}) {
    sp::step(space, objects_state, relations, ++clock, dt, moves, result);
  }
  void wait_navigation() {
    auto start = std::chrono::steady_clock::now();
    for (;;) {
      auto status = sp::install_ready_navigation(space);
      require(status.errors.empty(), status.errors.empty() ? "" : status.errors.front().c_str());
      if (status.pending.empty())
        break;
      require(std::chrono::steady_clock::now() - start < std::chrono::seconds(10),
              "navigation worker timeout");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
};
} // namespace testspace
