#pragma once
#include "structural.hpp"
#include <map>
#include <unordered_set>

namespace amber::bindings::structural_detail {
perm::Holder permission_holder(Caller);
bool declared_descendant(const Relation &, IdentityRead, Handle root, Handle child);
record::Event structural_record(const drive::StructuralCall &, record::Kind, std::string,
                                std::vector<Handle>);
void completed_effects(drive::StructuralCompletion &, react::DeferredEffects);
space::Pose relative_pose(TransformRead, Handle subject, Handle parent);
record::Event birth_record(const drive::StructuralCall &, Handle, IdentityRead,
                           const std::vector<PlayerId> &owners, const std::vector<PlotId> &plots,
                           const std::vector<std::uint32_t> &params, PlayerId maker,
                           CreditResolver);
struct PreparedReplacement {
  lifecycle::Replacement plan;
  Handle mechanical_root;
  std::vector<Handle> before, removed;
  std::map<std::uint64_t, PhysicalDefinition> retained;
  std::vector<PhysicalDefinition> added;
  std::vector<space::BodySpec> bodies;
  std::map<std::uint64_t, space::Attachment> old_internal;
  std::unordered_set<std::uint64_t> rebound;
  std::size_t sensors = 0;
  bool in_scene = false, promote = false;
  space::BodyPreflight capacity{};
};
PreparedReplacement prepare_replacement(const ReplacementEdit &, const drive::StructuralCall &,
                                        lifecycle::Replacement, ConstructionCatalog,
                                        const react::ValueRead &);
void commit_replacement(ReplacementEdit &, const drive::StructuralCall &, PreparedReplacement &,
                        const space::AssemblyEdit &, PlayerId maker, drive::StructuralCompletion &);
} // namespace amber::bindings::structural_detail
