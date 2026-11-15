#include "geometry.hpp"

#include <fstream>
#include <stdexcept>

namespace amber::bindings {
namespace {
std::vector<std::byte> bytes(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream)
    throw std::runtime_error("cannot read verified geometry artifact: " + path.string());
  const auto size = stream.tellg();
  std::vector<std::byte> result(static_cast<std::size_t>(size));
  stream.seekg(0);
  stream.read(reinterpret_cast<char *>(result.data()), static_cast<std::streamsize>(result.size()));
  if (!stream)
    throw std::runtime_error("cannot read complete geometry artifact");
  return result;
}
Vec3 vector(const nlohmann::json &value) {
  const auto v = value.get<std::array<float, 3>>();
  return {v[0], v[1], v[2]};
}
void check(space::OperationResult result) {
  if (result.status != space::Status::Ok)
    throw std::logic_error("preflighted geometry installation failed: " +
                           std::string(space::status_text(result.status)));
}
} // namespace
void import_geometry_asset(GeometryAssets &geometries, space::Space &space,
                           const catalog::Asset &asset, const std::filesystem::path &root) {
  if (geometries.rows.contains(asset.hash))
    return;
  GeometryAsset prepared;
  const auto &collision = asset.manifest.at("collision");
  if (!collision.at("static").is_null()) {
    const auto directory = root / asset.hash;
    const auto mesh = bytes(directory / "collision.bin");
    const auto convex = bytes(directory / "convex.bin");
    const auto triangles = bytes(directory / "mesh.bin");
    const auto imported =
        space::import_geometry(space, {mesh, convex, triangles, {asset.minimum, asset.maximum}});
    if (!imported.geometry)
      throw std::runtime_error(imported.error);
    prepared.collision = imported.geometry;
  }
  const auto &bindings = asset.manifest.at("bindings");
  for (const auto &[name, kind] :
       {std::pair{"zone", space::VolumeKind::Zone}, {"container", space::VolumeKind::Container}}) {
    if (!bindings.contains(name) || !bindings.at(name).contains("shape"))
      continue;
    const auto &shape = bindings.at(name).at("shape");
    if (shape.at("kind") != "box")
      throw std::invalid_argument("unknown authored volume shape");
    const auto size = vector(shape.at("size"));
    const auto offset = vector(shape.at("offset"));
    const auto geometry = space::sensor_box_geometry(space, size);
    prepared.volumes.push_back({geometry, {offset, {}}, kind});
  }
  geometries.rows.emplace(asset.hash, std::move(prepared));
}
PhysicalDefinition describe_physics(const GeometryAssets &geometries, const catalog::Asset &asset,
                                    const react::ValueRead &values, PhysicalSubject subject,
                                    std::uint32_t flags, PhysicsRoles roles,
                                    PhysicsDefaults defaults, Step now, double hz) {
  const auto &geometry = geometries.rows.at(asset.hash);
  auto intrinsic = [&](TermId id) {
    const auto &term = values.terms.rows.at(id);
    if (term.source != react::Source::Intrinsic || term.kind != react::Kind::Quantity)
      throw std::invalid_argument("physical material role needs an intrinsic quantity");
    const auto value = react::read_intrinsic(values.materials, subject.material, term);
    if (!value.present || !value.value.is_numeric() || !std::isfinite(value.value.numeric()))
      throw std::invalid_argument("physical material role has no finite value");
    return value.value.numeric();
  };
  const double density = intrinsic(roles.density);
  if (!(density > 0))
    throw std::invalid_argument("physical material density must be positive");
  auto scalar = [&](const nlohmann::json &binding) -> float {
    if (binding.is_number())
      return binding.get<float>();
    const auto id = values.terms.by_name.at(binding.get<std::string>());
    const auto &term = values.terms.rows.at(id);
    react::Answer answer;
    if (term.source == react::Source::Intrinsic)
      answer = react::read_intrinsic(values.materials, subject.material, term);
    else if (term.source == react::Source::Measured) {
      double value;
      switch (term.measurement) {
      case react::MeasurementRole::Mass:
        value = asset.volume * density;
        break;
      case react::MeasurementRole::Volume:
        value = asset.volume;
        break;
      case react::MeasurementRole::Baked:
        answer = react::read_baked(values.assets, subject.model, term, subject.params);
        value = 0;
        break;
      case react::MeasurementRole::Runtime:
        if (!subject.instance || !values.identity.valid(subject.instance))
          throw std::invalid_argument("new physical object requires an available runtime value");
        answer = react::read(values, subject.instance, id, now, hz);
        value = 0;
        break;
      default:
        throw std::invalid_argument("new physical object requires an available measured value");
      }
      if (term.measurement == react::MeasurementRole::Mass ||
          term.measurement == react::MeasurementRole::Volume)
        answer = {true, Value(value)};
    } else if (term.source == react::Source::Setting)
      answer = react::read_setting(values.instance_settings, values.model_settings,
                                   values.plot_settings, values.world_settings, subject.instance,
                                   subject.settings_model ? subject.settings_model : subject.model,
                                   subject.plot, id);
    else if (subject.instance && values.identity.valid(subject.instance))
      answer = react::read(values, subject.instance, id, now, hz);
    if (!answer.present || !answer.value.is_numeric() || !std::isfinite(answer.value.numeric()))
      throw std::invalid_argument("physical binding has no finite scalar answer");
    return static_cast<float>(answer.value.numeric());
  };
  auto motion = flags & Body             ? space::Motion::Character
                : flags & Dynamic        ? space::Motion::Dynamic
                : flags & ScriptedMotion ? space::Motion::Kinematic
                                         : space::Motion::Static;
  const auto &bindings = asset.manifest.at("bindings");
  if (!(flags & Body) && bindings.contains("motion") && bindings.at("motion").contains("mode")) {
    const auto mode = bindings.at("motion").at("mode").get<std::string>();
    if (mode == "kinematic")
      motion = space::Motion::Kinematic;
    else if (mode == "dynamic")
      motion = space::Motion::Dynamic;
    else if (mode == "static")
      motion = space::Motion::Static;
    else
      throw std::invalid_argument("unknown authored motion mode");
  }
  const auto mode_flags = motion == space::Motion::Dynamic     ? Dynamic
                          : motion == space::Motion::Kinematic ? ScriptedMotion
                                                               : Static;
  const float transmission = static_cast<float>(intrinsic(roles.transmission));
  space::BodySpec body{
      subject.instance,
      geometry.collision,
      motion,
      {roles.friction ? static_cast<float>(intrinsic(roles.friction)) : defaults.friction,
       roles.restitution ? static_cast<float>(intrinsic(roles.restitution)) : defaults.restitution,
       transmission},
      static_cast<float>(asset.volume * density),
      motion == space::Motion::Static,
      motion != space::Motion::Character,
      {}};
  if (bindings.contains("physics")) {
    const auto &physics = bindings.at("physics");
    body.collision_solid = physics.value("collision_solid", true);
    body.navigation_surface = physics.value("navigation_surface", body.navigation_surface);
    body.navigation_obstacle = physics.value("navigation_obstacle", body.navigation_obstacle);
  }
  if (motion == space::Motion::Character) {
    const auto &c = bindings.at("character");
    body.character = space::CharacterSpec{
        scalar(c.at("radius")),         scalar(c.at("height")),    scalar(c.at("mass")),
        scalar(c.at("max_push_force")), scalar(c.at("max_speed")), scalar(c.at("acceleration")),
        scalar(c.at("max_step")),       scalar(c.at("max_slope")), scalar(c.at("floor_snap"))};
    body.mass_kg = body.character->mass_kg;
  }
  return {body, mode_flags, &geometry.volumes};
}
void install_physics(space::Space &space, IdentityRead identity, TransformRead transforms,
                     SpaceFlags flags, Handle object, const PhysicalDefinition &definition) {
  auto spec = definition.body;
  spec.object = object;
  flags.set(object.slot(), (flags.get(object.slot()) & Held) | definition.mode_flags);
  if (spec.geometry)
    check(space::add(space, identity, transforms, spec));
  else
    check(space::register_logical_root(space, identity, object));
  for (const auto &volume : *definition.volumes)
    check(space::add_volume(space, identity, transforms,
                            {object, volume.geometry, volume.local, volume.kind}));
}
} // namespace amber::bindings
