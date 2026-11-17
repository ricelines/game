#pragma once

#include "catalog/catalog.hpp"
#include "step.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace amber::step::start {
void install_catalog(const Json &, amber::react::State &react_state, const catalog::Hash &,
                     const catalog::RuntimePins &, react::VmSettings,
                     const std::filesystem::path &asset_root,
                     const std::filesystem::path &script_root, amber::catalog::State &catalog_state,
                     Symbols &);
void bind_relation_reads(react::RelationsRead &, react::AuthoredRelations &, const react::Terms &,
                         const space::PhysicalRelations &, std::array<TermId, 4> physical_terms);
void record_seed(record::Journal &journal, const react::Terms &terms, const catalog::Models &models,
                 const catalog::Scripts &scripts, std::span<const Handle> initial, IdentityRead ids,
                 AuthorityRead auth, AttributionRead makers);
void provision_body_grants(const Json &, const Symbols &,
                           const amber::catalog::State &catalog_state,
                           const amber::ObjectState &objects_state, amber::perm::State &perm_state,
                           const net::Profiles &, Step, double);
} // namespace amber::step::start

namespace amber::step::start::detail {
inline TermId term(const react::Terms &terms, const Json &value) {
  if (value.is_string())
    return terms.by_name.at(value.get<std::string>());
  const auto id = value.get<TermId>();
  if (!id || id >= terms.rows.size())
    throw std::invalid_argument("unknown initial term");
  return id;
}
inline Vec3 vector(const Json &value) {
  if (!value.is_array() || value.size() != 3)
    throw std::invalid_argument("initial position requires three coordinates");
  return {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
}
inline Quat quaternion(const Json &value) {
  if (!value.is_array() || value.size() != 4)
    throw std::invalid_argument("initial rotation requires quaternion array");
  return {value[0].get<float>(), value[1].get<float>(), value[2].get<float>(),
          value[3].get<float>()};
}
inline Value scalar(const Json &value, const Symbols &symbols) {
  if (value.is_object() && value.size() == 1 && value.contains("object"))
    return Value(symbols.objects.at(value.at("object").get<std::string>()));
  if (value.is_boolean())
    return Value(value.get<bool>());
  if (value.is_number_integer())
    return Value(value.get<std::int64_t>());
  if (value.is_number_float())
    return Value(value.get<double>());
  throw std::invalid_argument("initial property needs a scalar or typed object reference");
}
inline perm::Rights rights(const Json &values, perm::ScopeKind scope = perm::ScopeKind::Object) {
  return step::rights(values, scope);
}
inline PlayerId account(const Json &value) {
  return step::identifier(value, "p:");
}
inline Step steps(double seconds, double hz) {
  return static_cast<Step>(std::ceil(seconds * hz));
}
} // namespace amber::step::start::detail
