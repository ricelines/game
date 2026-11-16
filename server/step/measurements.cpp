#include "step.hpp"

#include <algorithm>
#include <stdexcept>

namespace amber::step {
void validate_mass_roles(const react::Terms &terms, MassRoles roles) {
  if (!roles.own_mass || !roles.assembly_mass || roles.own_mass == roles.assembly_mass ||
      roles.own_mass >= terms.rows.size() || roles.assembly_mass >= terms.rows.size())
    throw std::invalid_argument("mass roles require two distinct registered terms");
  const std::array<std::int8_t, 8> kilograms{0, 1, 0, 0, 0, 0, 0, 0};
  for (auto [id, role] : {std::pair{roles.own_mass, react::MeasurementRole::Mass},
                          std::pair{roles.assembly_mass, react::MeasurementRole::Runtime}}) {
    const auto &term = terms.rows[id];
    if (term.kind != react::Kind::Quantity || term.source != react::Source::Measured ||
        term.combine != react::Combine::None || term.measurement != role ||
        term.dimensions != kilograms)
      throw std::invalid_argument("mass roles require measured own/runtime kg quantities");
  }
}
void invalidate_mass(AssemblyMasses &masses, IdentityRead identity, const Relation &parts,
                     std::span<const Handle> changed) {
  std::vector<Handle> pending(changed.begin(), changed.end());
  std::unordered_set<std::uint64_t> visited;
  for (std::size_t i = 0; i < pending.size(); ++i) {
    auto h = pending[i];
    if (!identity.valid(h) || !visited.insert(h.bits).second)
      continue;
    masses.dirty.insert(h.bits);
    if (const auto *parents = parts.in.get(h.slot()))
      pending.insert(pending.end(), parents->begin(), parents->end());
  }
}
void refresh_mass(AssemblyMasses &masses, const react::ValueRead &values, const Relation &parts,
                  react::RuntimeMeasurements &measurements, Step now, double hz,
                  const amber::Origin &origin, react::DeferredEffects &effects) {
  if (masses.dirty.empty())
    return;
  std::vector<std::uint64_t> dirty(masses.dirty.begin(), masses.dirty.end());
  std::sort(dirty.begin(), dirty.end());
  std::unordered_set<std::uint64_t> computed;
  std::vector<react::DerivedFactInput> updates;
  auto total = [&](auto &&self, Handle h) -> double {
    if (auto it = masses.rows.find(h.bits);
        it != masses.rows.end() && (!masses.dirty.contains(h.bits) || computed.contains(h.bits)))
      return it->second.total_kg;
    masses.dirty.insert(h.bits);
    auto value = react::read(values, h, masses.roles.own_mass, now, hz);
    if (!value.present || !value.value.is_numeric() || !std::isfinite(value.value.numeric()) ||
        value.value.numeric() < 0)
      throw std::invalid_argument("assembly mass requires finite nonnegative own mass");
    const double own = value.value.numeric();
    double sum = own;
    if (const auto *children = parts.out.get(h.slot()))
      for (auto child : *children)
        sum += self(self, child);
    if (!std::isfinite(sum))
      throw std::invalid_argument("assembly mass overflow");
    masses.rows[h.bits] = {own, sum};
    computed.insert(h.bits);
    updates.push_back({{h, masses.roles.assembly_mass}, Value(sum), origin});
    return sum;
  };
  // All input values are evaluated before publishing any runtime fact. A malformed
  // material cannot leave a half-published derived tree; dirty rows remain retryable.
  for (auto bits : dirty)
    if (values.identity.valid({bits}))
      total(total, Handle{bits});
  for (const auto &update : updates)
    react::apply_derived_fact(measurements, update, effects);
  masses.dirty.clear();
}
void retire_mass(AssemblyMasses &masses, react::RuntimeMeasurements &measurements, Handle h) {
  masses.rows.erase(h.bits);
  masses.dirty.erase(h.bits);
  measurements.values.erase({h, masses.roles.assembly_mass});
}
void reconcile_mass(AssemblyMasses &masses, const react::ValueRead &values,
                    const Relation &declared_parts, react::RuntimeMeasurements &measurements,
                    std::span<const FactKey> facts, const amber::step::Clock &clock,
                    const amber::Origin &origin, react::DeferredEffects &effects) {
  const auto now = clock.step;
  const auto hz = clock.hz;

  std::vector<Handle> changed;
  for (const auto key : facts) {
    if (!values.identity.valid(key.subject) || !key.term || key.term >= values.terms.rows.size())
      continue;
    const auto &term = values.terms.rows[key.term];
    if (key.term == masses.roles.own_mass || key.term == values.density ||
        (term.source == react::Source::Measured &&
         term.measurement == react::MeasurementRole::Volume))
      changed.push_back(key.subject);
  }
  if (!changed.empty())
    invalidate_mass(masses, values.identity, declared_parts, changed);
  refresh_mass(masses, values, declared_parts, measurements, now, hz, origin, effects);
}
} // namespace amber::step
