#pragma once

#include "catalog/catalog.hpp"
#include "net.hpp"
#include "react/react.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace amber::net::view_detail {
bool has_action_space(const react::State &, std::string_view);
Json action_spaces(const react::State &);
std::vector<TermId> space_action_ids(const catalog::State &, const react::State &,
                                     std::string_view);
PlotId plot_id(std::string_view, const perm::Plots &);
Json inspect_land(const catalog::State &, const react::State &, const ObjectState &,
                  const perm::State &, const step::Clock &, const amber::Settings &,
                  const Invocation &);

inline Json vector(Vec3 p) {
  return {p.x, p.y, p.z};
}
inline Json scalar(Value v) {
  switch (v.kind) {
  case Value::Kind::None:
    return nullptr;
  case Value::Kind::Number:
    return v.number;
  case Value::Kind::Integer:
    return v.integer;
  case Value::Kind::Boolean:
    return v.boolean;
  case Value::Kind::Object:
    return to_string(v.object);
  }
  return nullptr;
}
inline net::Reply error(std::string problem, std::string kind = "bad_input") {
  return net::Reply::result({{"ok", false}, {"error", kind}, {"problem", problem}});
}
inline Handle target(IdentityRead identity, const net::Invocation &call) {
  const auto name = call.request.fields.value("target", std::string("self"));
  const auto handle =
      name == "self" ? std::optional<Handle>(call.principal.body) : parse_handle(name);
  if (!handle || !identity.valid(*handle))
    throw std::invalid_argument("Unknown or stale object handle");
  return *handle;
}
inline const catalog::Asset &asset(const catalog::Models &models,
                                   const catalog::ParameterBlocks &parameters,
                                   const catalog::Assets &assets, const react::ValueRead &values,
                                   Handle object) {
  const auto selected = catalog::resolve_asset(
      models, parameters, values.identity.model[object.slot()], values.params[object.slot()]);
  return assets.rows.at(selected.hash);
}
inline Json text(const catalog::Text &claims) {
  return {{"name", claims.name}, {"description", claims.description}, {"writing", claims.writing}};
}
inline Json description(const ObjectDescription &d) {
  return {{"h", to_string(d.object)}, {"model", d.model},           {"measured", d.measured},
          {"says", text(d.says)},     {"elaborated", d.elaborated}, {"actions", d.actions}};
}
inline ObjectDescription describe(const amber::catalog::State &catalog_state,
                                  const amber::react::State &react_state,
                                  const react::ValueRead &values, Handle h, Step now, double hz,
                                  std::span<const TermId> inspected) {
  auto &models = catalog_state.models;
  auto &scripts = catalog_state.scripts;
  auto &instances = react_state.installations;
  auto &native = react_state.native_declarations;
  auto &authored_texts = catalog_state.authored_text;

  const auto actions = object_actions(models, scripts, instances, native, values, h);
  auto result =
      describe_object(h, values.identity, catalog_state, values, inspected, actions, now, hz);
  for (const auto &entry : authored_texts.rows)
    if (entry.id && entry.target == h && entry.kind == catalog::AuthoredTextKind::Detail)
      result.elaborated.push_back({{"id", entry.id},
                                   {"elaborated", true},
                                   {"text", entry.text},
                                   {"author", "p:" + std::to_string(entry.author)},
                                   {"created_s", entry.created_s}});
  return result;
}

} // namespace amber::net::view_detail
