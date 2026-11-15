#pragma once
#include "react/react.hpp"

#include <unordered_set>

namespace amber::bindings {
struct MassRoles {
  TermId own_mass, assembly_mass;
};
struct AssemblyMasses {
  struct Row {
    double own_kg, total_kg;
    std::uint64_t revision;
  };
  MassRoles roles;
  std::unordered_map<std::uint64_t, Row> rows;
  std::unordered_set<std::uint64_t> dirty;
};
void validate_mass_roles(const react::Terms &, MassRoles);
void invalidate_mass(AssemblyMasses &, IdentityRead, const Relation &declared_parts,
                     std::span<const Handle> changed);
void refresh_mass(AssemblyMasses &, const react::ValueRead &, const Relation &declared_parts,
                  react::RuntimeMeasurements &, Step, double hz, const record::Origin &,
                  react::DeferredEffects &);
void retire_mass(AssemblyMasses &, react::RuntimeMeasurements &, Handle);
struct MassReconciliation {
  AssemblyMasses &masses;
  const react::ValueRead &values;
  const Relation &declared_parts;
  react::RuntimeMeasurements &measurements;
};
void reconcile_mass(MassReconciliation &, std::span<const FactKey>, Step, double hz,
                    const record::Origin &, react::DeferredEffects &);
} // namespace amber::bindings
