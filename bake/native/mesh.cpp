#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/StreamWrapper.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/RegisterTypes.h>

#ifndef AMB_BAKE_LIBRARY
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#endif
#include <meshoptimizer.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <queue>
#include <stdexcept>
#include <vector>

using Json = nlohmann::json;
using Point = std::array<float, 3>;

static void save_shape(const JPH::Shape *shape, const std::filesystem::path &path) {
  std::ofstream file(path, std::ios::binary);
  JPH::StreamOutWrapper stream(file);
  JPH::Shape::ShapeToIDMap shapes;
  JPH::Shape::MaterialToIDMap materials;
  shape->SaveWithChildren(stream, shapes, materials);
  if (stream.IsFailed())
    throw std::runtime_error("Cannot save collision shape");
}

#ifndef AMB_BAKE_LIBRARY
static Json inspect_shape(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  JPH::StreamInWrapper stream(file);
  JPH::Shape::IDToShapeMap shapes;
  JPH::Shape::IDToMaterialMap materials;
  auto result = JPH::Shape::sRestoreWithChildren(stream, shapes, materials);
  if (result.HasError())
    throw std::runtime_error(result.GetError().c_str());
  const auto shape = result.Get();
  auto box = shape->GetLocalBounds();
  Json info{{"volume", shape->GetVolume()},
            {"min", {box.mMin.GetX(), box.mMin.GetY(), box.mMin.GetZ()}},
            {"max", {box.mMax.GetX(), box.mMax.GetY(), box.mMax.GetZ()}}};
  if (shape->GetSubType() == JPH::EShapeSubType::ConvexHull)
    info["points"] = static_cast<const JPH::ConvexHullShape *>(shape.GetPtr())->GetNumPoints();
  return info;
}

static Json validate_glb(const std::filesystem::path &path) {
  cgltf_options options{};
  cgltf_data *data = nullptr;
  if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success)
    throw std::runtime_error("Cannot parse glTF");
  for (size_t i = 0; i < data->buffers_count; ++i)
    if (data->buffers[i].uri) {
      cgltf_free(data);
      throw std::runtime_error("External glTF buffer URIs are not permitted");
    }
  auto result = cgltf_load_buffers(&options, data, path.c_str());
  if (result == cgltf_result_success)
    result = cgltf_validate(data);
  size_t triangles = 0;
  for (size_t i = 0; i < data->meshes_count; ++i)
    for (size_t p = 0; p < data->meshes[i].primitives_count; ++p) {
      const auto &primitive = data->meshes[i].primitives[p];
      if (primitive.type != cgltf_primitive_type_triangles || !primitive.indices) {
        cgltf_free(data);
        throw std::runtime_error("Expected indexed triangle geometry");
      }
      triangles += primitive.indices->count / 3;
    }
  Json info{{"nodes", data->nodes_count}, {"meshes", data->meshes_count}, {"triangles", triangles}};
  cgltf_free(data);
  if (result != cgltf_result_success)
    throw std::runtime_error("Invalid glTF buffers/accessors");
  return info;
}
#endif

static std::vector<unsigned> simplify(const std::vector<Point> &vertices,
                                      const std::vector<unsigned> &indices, float ratio,
                                      float error) {
  std::vector<unsigned> result(indices.size());
  auto target =
      std::min(indices.size(), std::max(size_t(36), (size_t(indices.size() * ratio) / 3) * 3));
  auto count = meshopt_simplify(result.data(), indices.data(), indices.size(), vertices[0].data(),
                                vertices.size(), sizeof(Point), target, error, 0, nullptr);
  result.resize(count);
  meshopt_optimizeVertexCache(result.data(), result.data(), result.size(), vertices.size());
  return result;
}

// Keep the stored/runtime mesh compact; surface-cell classification needs more
// precision than float centers provide at small scales or away from the origin.
using VoxelPoint = std::array<double, 3>;
static VoxelPoint voxel_point(const Point &point) {
  return {point[0], point[1], point[2]};
}

static VoxelPoint subtract(const VoxelPoint &a, const VoxelPoint &b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

static VoxelPoint cross(const VoxelPoint &a, const VoxelPoint &b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

static bool triangle_box(const std::array<VoxelPoint, 3> &triangle, const VoxelPoint &center,
                         double half) {
  std::array<VoxelPoint, 3> v;
  for (int i = 0; i < 3; ++i)
    v[i] = subtract(triangle[i], center);
  auto separated = [&](const VoxelPoint &axis) {
    double low = std::numeric_limits<double>::infinity(), high = -low;
    for (const auto &point : v) {
      const double projection = point[0] * axis[0] + point[1] * axis[1] + point[2] * axis[2];
      low = std::min(low, projection);
      high = std::max(high, projection);
    }
    const double radius = half * (std::abs(axis[0]) + std::abs(axis[1]) + std::abs(axis[2]));
    return low > radius || high < -radius;
  };
  const std::array<VoxelPoint, 3> axes = {VoxelPoint{1, 0, 0}, VoxelPoint{0, 1, 0},
                                          VoxelPoint{0, 0, 1}};
  for (const auto &axis : axes)
    if (separated(axis))
      return false;
  for (int i = 0; i < 3; ++i) {
    const auto edge = subtract(v[(i + 1) % 3], v[i]);
    for (const auto &axis : axes)
      if (separated(cross(edge, axis)))
        return false;
  }
  return !separated(cross(subtract(v[1], v[0]), subtract(v[2], v[0])));
}

static double capacity(const std::vector<Point> &vertices, const std::vector<unsigned> &indices,
                       unsigned grid) {
  VoxelPoint low = voxel_point(vertices[0]), high = low;
  for (const auto &v : vertices)
    for (int axis = 0; axis < 3; ++axis) {
      low[axis] = std::min(low[axis], double(v[axis]));
      high[axis] = std::max(high[axis], double(v[axis]));
    }
  const double h = std::max({high[0] - low[0], high[1] - low[1], high[2] - low[2]}) / double(grid);
  const int nx = int(std::ceil((high[0] - low[0]) / h)) + 2;
  const int ny = int(std::ceil((high[1] - low[1]) / h)) + 2;
  const int nz = int(std::ceil((high[2] - low[2]) / h)) + 2;
  const auto count = size_t(nx) * ny * nz;
  std::vector<uint8_t> solid(count, 0), visited(count, 0);
  auto index = [&](int x, int y, int z) { return size_t((z * ny + y) * nx + x); };
  auto height = [&](int z) { return low[2] + (double(z) - 0.5) * h; };
  for (size_t i = 0; i < indices.size(); i += 3) {
    std::array<VoxelPoint, 3> triangle{voxel_point(vertices[indices[i]]),
                                       voxel_point(vertices[indices[i + 1]]),
                                       voxel_point(vertices[indices[i + 2]])};
    std::array<int, 3> first, last;
    for (int axis = 0; axis < 3; ++axis) {
      const double a = std::min({triangle[0][axis], triangle[1][axis], triangle[2][axis]});
      const double b = std::max({triangle[0][axis], triangle[1][axis], triangle[2][axis]});
      first[axis] = int(std::floor((a - low[axis]) / h - 1e-5)) + 1;
      last[axis] = int(std::floor((b - low[axis]) / h + 1e-5)) + 1;
    }
    for (int z = std::max(1, first[2]); z <= std::min(nz - 2, last[2]); ++z)
      for (int y = std::max(1, first[1]); y <= std::min(ny - 2, last[1]); ++y)
        for (int x = std::max(1, first[0]); x <= std::min(nx - 2, last[0]); ++x) {
          const VoxelPoint center{low[0] + (x - 0.5) * h, low[1] + (y - 0.5) * h, height(z)};
          if (triangle_box(triangle, center, h * 0.50001))
            solid[index(x, y, z)] = 1;
        }
  }
  using Cell = std::pair<double, size_t>;
  std::priority_queue<Cell, std::vector<Cell>, std::greater<>> heap;
  for (int z = 0; z < nz; ++z)
    for (int y = 0; y < ny; ++y)
      for (int x = 0; x < nx; ++x)
        if (x == 0 || y == 0 || z == 0 || x == nx - 1 || y == ny - 1 || z == nz - 1) {
          auto at = index(x, y, z);
          if (!solid[at]) {
            visited[at] = 1;
            heap.emplace(height(z), at);
          }
        }
  size_t retained = 0;
  constexpr int offsets[6][3] = {{-1, 0, 0}, {1, 0, 0},  {0, -1, 0},
                                 {0, 1, 0},  {0, 0, -1}, {0, 0, 1}};
  while (!heap.empty()) {
    auto [level, at] = heap.top();
    heap.pop();
    const int x = int(at % nx), y = int((at / nx) % ny), z = int(at / (size_t(nx) * ny));
    if (level > height(z) + h * 0.01)
      ++retained;
    for (const auto &offset : offsets) {
      const int a = x + offset[0], b = y + offset[1], c = z + offset[2];
      if (a < 0 || a >= nx || b < 0 || b >= ny || c < 0 || c >= nz)
        continue;
      const auto next = index(a, b, c);
      if (!visited[next] && !solid[next]) {
        visited[next] = 1;
        heap.emplace(std::max(level, height(c)), next);
      }
    }
  }
  return double(retained) * h * h * h;
}

static void compile_mesh(const Json &request, const std::filesystem::path &out,
                         unsigned max_points) {
  const auto vertices = request.at("vertices").get<std::vector<Point>>();
  const auto indices = request.at("indices").get<std::vector<unsigned>>();
  if (vertices.size() < 4 || indices.size() < 12 || indices.size() % 3 != 0)
    throw std::runtime_error("Expected closed nonempty triangle mesh");
  for (const auto &v : vertices)
    for (float value : v)
      if (!std::isfinite(value))
        throw std::runtime_error("Non-finite vertex");
  for (unsigned index : indices)
    if (index >= vertices.size())
      throw std::runtime_error("Triangle index out of range");

  const unsigned grid = request.value("capacity_grid", 0u);
  if (grid > 512)
    throw std::runtime_error("Capacity grid too large");
  const double measured_capacity = grid ? capacity(vertices, indices, grid) : 0.0;

  std::filesystem::create_directories(out);
  const auto ratios = request.at("lod_ratios").get<std::array<float, 3>>();
  const float relative_error = request.at("lod_relative_error").get<float>();
  // OCCT duplicates positions at face boundaries. Weld and compact them;
  // leaving unused duplicates in the buffer still makes them attribute seams.
  std::vector<unsigned> remap(vertices.size()), welded(indices.size());
  meshopt_generatePositionRemap(remap.data(), vertices[0].data(), vertices.size(), sizeof(Point));
  std::transform(indices.begin(), indices.end(), welded.begin(),
                 [&](unsigned index) { return remap[index]; });
  const auto vertex_count =
      meshopt_optimizeVertexFetchRemap(remap.data(), welded.data(), welded.size(), vertices.size());
  std::vector<Point> compact(vertex_count);
  std::vector<unsigned> original(vertex_count);
  meshopt_remapVertexBuffer(compact.data(), vertices.data(), vertices.size(), sizeof(Point),
                            remap.data());
  meshopt_remapIndexBuffer(welded.data(), welded.data(), welded.size(), remap.data());
  for (unsigned i = 0; i < vertices.size(); ++i)
    if (remap[i] != ~0u)
      original[remap[i]] = i;
  auto lod1 = simplify(compact, welded, ratios[1], relative_error);
  // LOD2 is distant rendering only. Aim for its specified triangle budget
  // instead of retaining tiny details because of the collider's error ceiling.
  auto lod2 = simplify(compact, welded, ratios[2], std::numeric_limits<float>::max());
  for (auto *lod : {&lod1, &lod2})
    for (auto &index : *lod)
      index = original[index];
  std::ofstream(out / "lod-indices.json") << Json{{"lod1", lod1}, {"lod2", lod2}};

  JPH::VertexList points;
  for (const auto &v : vertices)
    points.emplace_back(v[0], v[1], v[2]);
  JPH::IndexedTriangleList triangles;
  for (size_t i = 0; i < lod1.size(); i += 3)
    triangles.emplace_back(lod1[i], lod1[i + 1], lod1[i + 2], 0);
  auto mesh = JPH::MeshShapeSettings(points, triangles).Create();
  if (mesh.HasError())
    throw std::runtime_error(mesh.GetError().c_str());
  save_shape(mesh.Get().GetPtr(), out / "collision.bin");

  // Support points in evenly distributed directions retain a bounded geometric hull.
  // Axis directions preserve the exact authored bounds; no model identity is involved.
  if (max_points < 6 || max_points > 64)
    throw std::runtime_error("Convex point limit must be between 6 and 64");
  JPH::Array<JPH::Vec3> hull_points;
  auto support = [&](float x, float y, float z) {
    const auto best =
        std::max_element(vertices.begin(), vertices.end(), [&](const Point &a, const Point &b) {
          return a[0] * x + a[1] * y + a[2] * z < b[0] * x + b[1] * y + b[2] * z;
        });
    const JPH::Vec3 point((*best)[0], (*best)[1], (*best)[2]);
    for (const auto &existing : hull_points)
      if (existing == point)
        return;
    hull_points.push_back(point);
  };
  support(1, 0, 0);
  support(-1, 0, 0);
  support(0, 1, 0);
  support(0, -1, 0);
  support(0, 0, 1);
  support(0, 0, -1);
  const auto count = max_points - 6;
  for (unsigned i = 0; i < count; ++i) {
    float z = 1.0f - 2.0f * (float(i) + 0.5f) / float(count);
    float r = std::sqrt(1.0f - z * z), angle = float(i) * 2.39996323f;
    support(r * std::cos(angle), r * std::sin(angle), z);
  }
  auto hull = JPH::ConvexHullShapeSettings(hull_points, 0.0f).Create();
  if (hull.HasError())
    throw std::runtime_error(hull.GetError().c_str());
  save_shape(hull.Get().GetPtr(), out / "convex.bin");

  std::ofstream nav(out / "mesh.bin", std::ios::binary);
  nav.write("AMBMESH1", 8);
  const auto vc = uint32_t(vertices.size()), ic = uint32_t(lod1.size());
  nav.write(reinterpret_cast<const char *>(&vc), sizeof(vc));
  nav.write(reinterpret_cast<const char *>(&ic), sizeof(ic));
  nav.write(reinterpret_cast<const char *>(vertices.data()),
            std::streamsize(vertices.size() * sizeof(Point)));
  nav.write(reinterpret_cast<const char *>(lod1.data()),
            std::streamsize(lod1.size() * sizeof(unsigned)));
  std::ofstream(out / "native-measured.json") << Json{{"capacity", measured_capacity}};
}

#ifdef AMB_BAKE_LIBRARY
extern "C" int amb_bake_compile(const char *input_path, const char *output_path,
                                unsigned max_points, char *error, size_t error_size) {
  JPH::RegisterDefaultAllocator();
  JPH::Factory::sInstance = new JPH::Factory();
  JPH::RegisterTypes();
  int status = 0;
  try {
    std::ifstream input(input_path);
    compile_mesh(Json::parse(input), output_path, max_points);
  } catch (const std::exception &exception) {
    if (error_size) {
      std::strncpy(error, exception.what(), error_size - 1);
      error[error_size - 1] = 0;
    }
    status = 1;
  }
  JPH::UnregisterTypes();
  delete JPH::Factory::sInstance;
  JPH::Factory::sInstance = nullptr;
  return status;
}
#else
int main(int argc, char **argv) {
  JPH::RegisterDefaultAllocator();
  JPH::Factory::sInstance = new JPH::Factory();
  JPH::RegisterTypes();
  int status = 0;
  try {
    if (argc == 3 && std::string(argv[1]) == "--inspect")
      std::cout << inspect_shape(argv[2]) << '\n';
    else if (argc == 3 && std::string(argv[1]) == "--validate-glb")
      std::cout << validate_glb(argv[2]) << '\n';
    else if (argc == 4) {
      std::ifstream input(argv[1]);
      compile_mesh(Json::parse(input), argv[2], unsigned(std::stoul(argv[3])));
      std::cout << "{\"ok\":true}\n";
    } else
      throw std::runtime_error("Usage: amb_bake_native mesh.json out max_points | --inspect shape "
                               "| --validate-glb file");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    status = 1;
  }
  JPH::UnregisterTypes();
  delete JPH::Factory::sInstance;
  JPH::Factory::sInstance = nullptr;
  return status;
}
#endif
