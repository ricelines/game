#pragma once
#include "lifecycle/lifecycle.hpp"
#include "measurements.hpp"
#include "presentation.hpp"
#include "replication.hpp"

namespace amber::bindings {
struct SpatialRetirement {
  space::Space &space;
  IdentityRead identity;
  ParentWrite parents;
  SpaceFlags flags;
  space::PhysicalRelations &relations;
  std::vector<space::RelationChange> &changes;
  step::Intents &intents;
  std::vector<step::IntentEnding> &endings;
};
struct LightProjection {
  observe::Lights &lights;
  observe::LightSubscriptions &subscriptions;
  IdentityRead identity;
  TransformRead transforms;
  const std::vector<std::uint32_t> &params;
  const catalog::Models &models;
  const catalog::ParameterBlocks &parameters;
  const catalog::Assets &assets;
  const react::Terms &terms;
  float default_source_radius_m;
  observe::ActiveViews *views = nullptr;
};
struct ZoneProjection {
  observe::ZoneAmbients &zones;
  IdentityRead identity;
  const std::vector<std::uint32_t> &params;
  const catalog::Models &models;
  const catalog::ParameterBlocks &parameters;
  const catalog::Assets &assets;
  const react::Terms &terms;
};
struct MassProjection {
  AssemblyMasses &masses;
  react::RuntimeMeasurements &measurements;
  IdentityRead identity;
  const Relation &declared_parts;
};
struct ReplicationProjection {
  ReplicationIndex &index;
  TransformRead transforms;
  const catalog::Models &models;
  const catalog::ParameterBlocks &parameters;
  const catalog::Assets &assets;
  const react::ValueRead &values;
};
void register_light_projection(LightProjection &, std::span<const Handle>);
void move_light_projection(LightProjection &, std::span<const Handle>);
void retire_light_projection(LightProjection &, std::span<const Handle>);
void register_zone_projection(ZoneProjection &, std::span<const Handle>);
void retire_zone_projection(ZoneProjection &, std::span<const Handle>);
void invalidate_mass_projection(MassProjection &, std::span<const Handle>);
void retire_mass_projection(MassProjection &, std::span<const Handle>);
void register_replication_projection(ReplicationProjection &, std::span<const Handle>);
void move_replication_projection(ReplicationProjection &, std::span<const Handle>);
void retire_replication_projection(ReplicationProjection &, std::span<const Handle>);
} // namespace amber::bindings
