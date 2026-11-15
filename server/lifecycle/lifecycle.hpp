#pragma once

#include "catalog/catalog.hpp"

namespace amber::lifecycle {
struct NamedPart {
  std::string name;
  Handle object;
  bool operator==(const NamedPart &) const = default;
};
using Parts = SparseColumn<std::vector<NamedPart>>;
struct PartTemplate {
  std::string name;
  ModelId model = 0;
  TermId material = 0;
  Vec3 position;
  Quat rotation;
};
struct AssemblyTemplate {
  ModelId model = 0;
  std::uint32_t params = 0;
  TermId material = 0;
  std::vector<PartTemplate> parts;
};
AssemblyTemplate expand(const catalog::Models &, const catalog::ParameterBlocks &,
                        const catalog::Assets &, ModelId, std::uint32_t params = 0);
struct Created {
  Handle root;
  std::vector<Handle> objects;
};
Created instantiate(Objects &, Parts &, Relation &declared_parts, const AssemblyTemplate &,
                    ObjectInit root);
std::vector<Handle> collect(const Parts &, IdentityRead, std::span<const Handle> roots);
std::vector<Handle> collect(const Parts &, IdentityRead, Handle root);
// Owner cleanup is coordinated by bindings::ObjectLifecycle before row release.
// The input is the already collected full closure, not an independent root list.
void release_rows(Objects &, Parts &, Relation &declared_parts, std::span<const Handle>);
struct Binding {
  ModelId model = 0;
  TermId material = 0;
  std::uint32_t params = 0;
  bool operator==(const Binding &) const = default;
};
struct Rebinding {
  Handle object;
  Binding before, after;
  Vec3 position;
  Quat rotation;
};
struct Replacement {
  Handle root;
  Binding original;
  std::vector<NamedPart> original_parts;
  std::vector<Rebinding> retained;
  std::vector<PartTemplate> added;
  std::vector<Handle> removed;
};
// Binding/name/material decisions only; retained poses are filled by plan_replace.
Replacement plan_bindings(const catalog::Models &, const Parts &, IdentityRead,
                          const std::vector<std::uint32_t> &params, Handle,
                          const AssemblyTemplate &, std::optional<TermId> material = {});
Replacement plan_replace(const catalog::Models &, const Parts &, IdentityRead, TransformRead,
                         const std::vector<std::uint32_t> &params, Handle, const AssemblyTemplate &,
                         std::optional<TermId> material = {});
// Retire removed subtrees first, without yielding the single writer. The returned
// created rows and retained bindings go to separate module installation adapters.
Created apply_replacement(Objects &, Parts &, Relation &declared_parts, const Replacement &,
                          PlayerId edit_maker, Step now);
void rebuild_declared_parts(const Parts &, IdentityRead, Relation &);

// The structural adapters receive allocation operations, never the object table.
// Authority and subsystem cleanup remain with their respective table owners.
class Instances {
public:
  Instances(Objects &objects, Parts &parts, Relation &declared)
      : objects_(objects), parts_(parts), declared_(declared) {}
  Created create(const AssemblyTemplate &, ObjectInit);
  std::vector<Handle> closure(std::span<const Handle> roots) const;
  void release_rows(std::span<const Handle>);
  Created replace(const Replacement &, PlayerId maker, Step);

private:
  Objects &objects_;
  Parts &parts_;
  Relation &declared_;
};
} // namespace amber::lifecycle
