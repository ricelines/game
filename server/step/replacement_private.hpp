#pragma once

#include "step.hpp"

#include <map>
#include <unordered_set>

namespace amber::step::structural_detail {
perm::Holder permission_holder(Caller);
react::InvocationInput native_input(const NativeCall &);
lifecycle::Binding instance_binding(const nlohmann::json &, const catalog::Models &,
                                    const catalog::ParameterBlocks &, const react::Terms &);
NativeCompletion structural_denied(std::string need, std::string on, std::string why,
                                   std::string fix);
void created_objects(NativeCompletion &, std::span<const Handle>);
bool declared_descendant(const Relation &, IdentityRead, Handle root, Handle child);
record::Event structural_record(const NativeCall &, record::Kind, std::string, std::vector<Handle>);
void completed_effects(NativeCompletion &, react::DeferredEffects);
space::Pose relative_pose(TransformRead, Handle subject, Handle parent);
record::Event birth_record(const NativeCall &, Handle, IdentityRead, const std::vector<PlayerId> &,
                           const std::vector<PlotId> &, const std::vector<std::uint32_t> &,
                           PlayerId maker, AttributionRead, const catalog::Models &,
                           const catalog::Scripts &);
// Scratch geometry for this one writer invocation; no owner references or callbacks.
struct ReplacementGeometry {
  lifecycle::Replacement rows;
  Handle mechanical_root;
  std::vector<Handle> before, removed;
  std::map<std::uint64_t, PhysicalDefinition> retained;
  std::vector<PhysicalDefinition> added;
  std::vector<space::BodySpec> bodies;
  std::map<std::uint64_t, space::Attachment> old_internal;
  std::unordered_set<std::uint64_t> rebound;
  std::size_t sensors = 0;
  bool in_scene = false, promote = false;
};
ReplacementGeometry replacement_geometry(const space::Space &, const lifecycle::Parts &,
                                         IdentityRead, TransformRead, ParentRead,
                                         const std::vector<PlotId> &, const GeometryAssets &,
                                         const catalog::Models &, const catalog::ParameterBlocks &,
                                         const catalog::Assets &, const react::ValueRead &,
                                         PhysicsRoles, PhysicsDefaults, const NativeCall &,
                                         lifecycle::Replacement, bool preserve_defaults = false);
void rebuild_instances(Objects &, lifecycle::Parts &, Relation &, space::Space &, TransformWrite,
                       ParentWrite, SpaceFlags, space::PhysicalRelations &,
                       std::vector<space::RelationChange> &, PlotWrite, const perm::Plots &,
                       AuthorityRead, AttributionRead, const catalog::Models &,
                       const catalog::Scripts &, const react::ValueRead &,
                       std::span<ReplacementGeometry *>, const space::AssemblyEdit &,
                       PlayerId maker, const NativeCall &, NativeCompletion &);
} // namespace amber::step::structural_detail
