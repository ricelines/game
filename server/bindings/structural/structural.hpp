#pragma once

#include "bindings/geometry/geometry.hpp"
#include "drive/drive.hpp"
#include "lifecycle/lifecycle.hpp"

namespace amber::bindings {
class ObjectLifecycle;
struct StructuralRights {
  IdentityRead identity;
  AuthorityRead authority;
  TransformRead transforms;
  const Relation &declared_parts;
  const perm::OwnerLimits &limits;
  const perm::Players &players;
  const perm::Grants &grants;
  const perm::Plots &plots;
  const space::Space &space;
  float reach_m;
};
struct ConstructionCatalog {
  const catalog::Models &models;
  const catalog::ParameterBlocks &parameters;
  const catalog::Assets &assets;
};
struct GeometryInstallation {
  space::Space &space;
  const GeometryAssets &geometry;
  const react::ValueRead &values;
  TransformRead transforms;
  SpaceFlags flags;
  PhysicsRoles roles;
  PhysicsDefaults defaults;
};
struct AssemblyPlacement {
  space::Space &space;
  IdentityRead identity;
  TransformWrite transforms;
  ParentWrite parents;
  SpaceFlags flags;
  const Relation &declared_parts;
  space::PhysicalRelations &physical;
  std::vector<space::RelationChange> &changes;
};
void install_declared_attachments(AssemblyPlacement &, std::span<const Handle>);
struct CreateOperation {
  lifecycle::Instances &instances;
  ConstructionCatalog catalog;
  GeometryInstallation &geometry;
  AssemblyPlacement &placement;
  const StructuralRights &rights;
  ObjectLifecycle &lifecycle;
  PlotWrite membership;
  CreditResolver credits{};
};
struct RemoveOperation {
  lifecycle::Instances &instances;
  const StructuralRights &rights;
  ObjectLifecycle &lifecycle;
  CreditResolver credits{};
};
struct ReplaceOperation {
  lifecycle::Instances &instances;
  const lifecycle::Parts &parts;
  ConstructionCatalog catalog;
  GeometryInstallation &geometry;
  TransformWrite transforms;
  ParentWrite parents;
  space::PhysicalRelations &physical;
  std::vector<space::RelationChange> &changes;
  const StructuralRights &rights;
  ObjectLifecycle &lifecycle;
  PlotWrite membership;
  CreditResolver credits{};
};
// Mechanical/lifecycle view shared by ordinary authorized edits and definition
// edits. It carries no permission decision tables or reach/actor authority.
struct ReplacementEdit {
  lifecycle::Instances &instances;
  const lifecycle::Parts &parts;
  IdentityRead identity;
  const std::vector<PlayerId> &owners;
  const std::vector<PlotId> &plots;
  GeometryInstallation &geometry;
  TransformWrite transforms;
  ParentWrite parents;
  space::PhysicalRelations &physical;
  std::vector<space::RelationChange> &changes;
  ObjectLifecycle &lifecycle;
  PlotWrite membership;
  const perm::Plots &land;
  CreditResolver credits{};
};
ReplacementEdit replacement_edit(ReplaceOperation &);
drive::NativeCompletion create_instance(void *, const drive::NativeCall &);
drive::NativeCompletion remove_instance(void *, const drive::NativeCall &);
drive::NativeCompletion replace_instance(void *, const drive::NativeCall &);
} // namespace amber::bindings
