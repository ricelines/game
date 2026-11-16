#pragma once

#include "catalog/catalog.hpp"

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
record::Event birth_record(const NativeCall &, Handle, const amber::ObjectState &objects_state,
                           const std::vector<PlayerId> &, const std::vector<PlotId> &,
                           PlayerId maker, const amber::catalog::State &catalog_state);
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
ReplacementGeometry replacement_geometry(const amber::space::State &space_state,
                                         const amber::ObjectState &objects_state,
                                         const std::vector<PlotId> &, const GeometryAssets &,
                                         const amber::catalog::State &catalog_state,
                                         const react::ValueRead &, PhysicsDefaults,
                                         const NativeCall &, lifecycle::Replacement,
                                         bool preserve_defaults = false);
void rebuild_instances(amber::ObjectState &objects_state, amber::space::State &space_state,
                       const perm::Plots &, const amber::catalog::State &catalog_state,
                       std::span<ReplacementGeometry *>, const space::AssemblyEdit &,
                       PlayerId maker, const NativeCall &, NativeCompletion &);
} // namespace amber::step::structural_detail
