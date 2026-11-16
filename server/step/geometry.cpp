#include "step.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace amber::step {
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
                                    PhysicsDefaults defaults, const amber::step::Clock &clock) {
  const auto now = clock.step;
  const auto hz = clock.hz;

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
  const auto installed = spec.geometry ? space::add(space, identity, transforms, spec)
                                       : space::register_logical_root(space, identity, object);
  if (installed.status != space::Status::Ok)
    throw std::invalid_argument(space::status_text(installed.status));
  for (const auto &volume : *definition.volumes) {
    const auto added = space::add_volume(space, identity, transforms,
                                         {object, volume.geometry, volume.local, volume.kind});
    if (added.status != space::Status::Ok)
      throw std::invalid_argument(space::status_text(added.status));
  }
}
std::vector<Handle>
initialize_physics(amber::step::State &step_state, const amber::catalog::State &catalog_state,
                   const amber::react::State &react_state, amber::ObjectState &objects_state,
                   const perm::Players &players, amber::space::State &space_state,
                   std::span<const Handle> initial, const checkpoint::Decoded &decoded,
                   PhysicsDefaults defaults, bool restored) {
  const auto values = react::value_read(objects_state, catalog_state, react_state);
  const auto &clock = step_state.clock;
  const auto &restored_motion = decoded.motion;
  const auto &restored_causes = decoded.causes;
  const auto &physics = catalog_state.mechanical.values.at("physics");
  const PhysicsRoles roles{
      physics.at("density").get<TermId>(), physics.at("transmission").get<TermId>(),
      physics.value("friction", TermId{}), physics.value("restitution", TermId{})};

  auto &space = space_state.space;

  auto &geometry = step_state.geometry;
  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;
  auto &assets = catalog_state.assets;
  auto transforms = amber::transforms(objects_state.objects);
  auto parents = amber::parents(objects_state.objects);
  auto flags = amber::space_flags(objects_state.objects);
  auto &parts = objects_state.parts;
  auto &declared_parts = objects_state.declared_parts;
  auto &physical = space_state.physical_relations;

  auto &causes = step_state.mechanical_causes;

  const auto identity = values.identity;
  std::unordered_set<std::uint64_t> stored;
  for (const auto &player : players.rows)
    for (const auto *goods : {&player.inventory, &player.mailbox})
      for (auto root : *goods)
        for (auto object : lifecycle::collect(parts, identity, root))
          stored.insert(object.bits);
  std::vector<Handle> visible;
  for (auto object : initial) {
    if (stored.contains(object.bits))
      continue;
    const auto slot = object.slot();
    const auto asset_hash =
        catalog::resolve_asset(models, parameters, identity.model[slot], values.params[slot]).hash;
    const auto &asset = assets.rows.at(asset_hash);
    auto definition = describe_physics(geometry, asset, values,
                                       {identity.model[slot], values.params[slot],
                                        identity.material[slot], values.plots[slot], object},
                                       identity.flags[slot], roles, defaults, clock);
    if (declared_parts.in.get(slot)) {
      definition.body.motion = space::Motion::Kinematic;
      definition.mode_flags = ScriptedMotion;
    }
    install_physics(space, identity, transforms, flags, object, definition);
    visible.push_back(object);
  }
  if (!restored)
    install_declared_attachments(objects_state, space_state, initial);
  if (restored) {
    space::rebuild_attachments(space, identity, transforms, parents, physical.attached_to);
    for (auto root : initial)
      if (!stored.contains(root.bits) && !(identity.flags[root.slot()] & Body) &&
          (identity.flags[root.slot()] & (Dynamic | Held)) && parts.get(root.slot()) &&
          !declared_parts.in.get(root.slot())) {
        const auto assembly = lifecycle::collect(parts, identity, root);
        const auto result =
            space::assemble(space, identity, transforms, root,
                            std::span<const Handle>(assembly).subspan(1), space::Motion::Dynamic);
        if (result.status != space::Status::Ok)
          throw std::runtime_error(space::status_text(result.status));
      }
    space::restore_motion(space, identity, restored_motion);
    restore_motion_causes(causes, space, identity, restored_causes);
  }
  for (;;) {
    const auto navigation = space::install_ready_navigation(space);
    for (const auto &error : navigation.errors)
      std::cerr << "navigation: " << error << '\n';
    if (navigation.pending.empty())
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return visible;
}
} // namespace amber::step
