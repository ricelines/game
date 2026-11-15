#pragma once
#include "structural.hpp"
#include <map>
#include <unordered_set>

namespace amber::bindings::structural_detail {
perm::Holder permission_holder(Caller);
InvocationInput native_input(const drive::NativeCall &);
lifecycle::Binding instance_binding(const nlohmann::json &, const ConstructionCatalog &,
                                    const react::Terms &);
drive::NativeCompletion structural_denied(std::string need, std::string on, std::string why,
                                          std::string fix);
void created_objects(drive::NativeCompletion &, std::span<const Handle>);
bool declared_descendant(const Relation &, IdentityRead, Handle root, Handle child);
record::Event structural_record(const drive::NativeCall &, record::Kind, std::string,
                                std::vector<Handle>);
void completed_effects(drive::NativeCompletion &, react::DeferredEffects);
space::Pose relative_pose(TransformRead, Handle subject, Handle parent);
record::Event birth_record(const drive::NativeCall &, Handle, IdentityRead,
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
PreparedReplacement prepare_replacement(const ReplacementEdit &, const drive::NativeCall &,
                                        lifecycle::Replacement, ConstructionCatalog,
                                        const react::ValueRead &, bool preserve_defaults = false);
void commit_replacements(ReplacementEdit &, const drive::NativeCall &,
                         std::span<PreparedReplacement *>, const space::AssemblyEdit &,
                         PlayerId maker, drive::NativeCompletion &);
void commit_replacement(ReplacementEdit &, const drive::NativeCall &, PreparedReplacement &,
                        const space::AssemblyEdit &, PlayerId maker, drive::NativeCompletion &);
} // namespace amber::bindings::structural_detail
