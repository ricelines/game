#pragma once

#include "catalog/catalog.hpp"
#include "space/space.hpp"

namespace amber::bindings {
struct VolumeGeometry {
  space::GeometryId geometry;
  space::Pose local;
  space::VolumeKind kind;
};
struct GeometryAsset {
  space::GeometryId collision = 0;
  std::vector<VolumeGeometry> volumes;
};
struct GeometryAssets {
  std::unordered_map<catalog::Hash, GeometryAsset> rows;
};
// Run when importing immutable artifacts, before admitting a placement request.
void import_geometry_asset(GeometryAssets &, space::Space &, const catalog::Asset &,
                           const std::filesystem::path &asset_root);
struct PhysicsRoles {
  TermId density, transmission;
  TermId friction = 0, restitution = 0;
};
struct PhysicsDefaults {
  float friction, restitution;
};
struct PhysicalSubject {
  ModelId model;
  std::uint32_t params;
  TermId material;
  PlotId plot;
  Handle instance;
};
struct PhysicalDefinition {
  space::BodySpec body;
  std::uint32_t mode_flags;
  const std::vector<VolumeGeometry> *volumes;
};
PhysicalDefinition describe_physics(const GeometryAssets &, const catalog::Asset &,
                                    const react::ValueRead &, PhysicalSubject, std::uint32_t flags,
                                    PhysicsRoles, PhysicsDefaults, Step, double hz);
// Geometry has already been imported and can_add has checked capacity. Any
// failure here is an integration error rather than a rejected game operation.
void install_physics(space::Space &, IdentityRead, TransformRead, SpaceFlags, Handle,
                     const PhysicalDefinition &);
struct MaterialRefresh {
  space::Space &space;
  const GeometryAssets &geometry;
  const catalog::Models &models;
  const catalog::ParameterBlocks &parameters;
  const catalog::Assets &assets;
  const react::ValueRead &values;
  PhysicsRoles roles;
  PhysicsDefaults defaults;
};
struct MaterialRefreshPlan {
  struct Update {
    Handle object;
    space::BodyProperties properties;
  };
  std::vector<Handle> affected;
  std::vector<Update> physical;
};
// Rare edit-time scan. Values may reference prospective material rows so all
// finite/range checks finish before committing the catalog edit.
MaterialRefreshPlan prepare_material_refresh(const MaterialRefresh &, std::span<const TermId>, Step,
                                             double hz);
void apply_material_refresh(space::Space &, IdentityRead, const MaterialRefreshPlan &);
} // namespace amber::bindings
