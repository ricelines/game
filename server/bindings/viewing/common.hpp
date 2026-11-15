#pragma once
#include "viewing.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace amber::bindings::viewing::detail {
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
inline control::Reply error(std::string problem, std::string kind = "bad_input") {
  return control::Reply::result({{"ok", false}, {"error", kind}, {"problem", problem}});
}
inline Handle target(const CatalogRead &catalog, const control::Invocation &call) {
  const auto name = call.request.fields.value("target", std::string("self"));
  const auto handle =
      name == "self" ? std::optional<Handle>(call.principal.body) : parse_handle(name);
  if (!handle || !catalog.values.identity.valid(*handle))
    throw std::invalid_argument("Unknown or stale object handle");
  return *handle;
}
inline space::Deadline deadline(double milliseconds) {
  return std::chrono::steady_clock::now() +
         std::chrono::duration_cast<std::chrono::steady_clock::duration>(
             std::chrono::duration<double, std::milli>(milliseconds));
}
inline const catalog::Asset &asset(const CatalogRead &catalog, Handle object) {
  const auto selected = catalog::resolve_asset(catalog.models, catalog.parameters,
                                               catalog.values.identity.model[object.slot()],
                                               catalog.values.params[object.slot()]);
  return catalog.assets.rows.at(selected.hash);
}
inline Json text(const catalog::Text &claims) {
  return {{"name", claims.name}, {"description", claims.description}, {"writing", claims.writing}};
}
inline Json description(const observe::ObjectDescription &d) {
  return {{"h", to_string(d.object)}, {"model", d.model},           {"measured", d.measured},
          {"says", text(d.says)},     {"elaborated", d.elaborated}, {"actions", d.actions}};
}
inline observe::ObjectDescription describe(const CatalogRead &catalog, Handle h, Step now,
                                           double hz, std::span<const TermId> inspected) {
  const auto actions = object_actions(catalog, h);
  auto result = describe_object(h, catalog.values.identity, catalog.models, catalog.parameters,
                                catalog.assets, catalog.model_text, catalog.instance_text,
                                catalog.values, inspected, actions, now, hz);
  if (const auto entries = catalog.authored_texts.details_by_target.find(h.bits);
      entries != catalog.authored_texts.details_by_target.end())
    for (auto id : entries->second) {
      const auto &entry = catalog.authored_texts.rows[id];
      result.elaborated.push_back({{"id", id},
                                   {"elaborated", true},
                                   {"text", entry.text},
                                   {"author", "p:" + std::to_string(entry.author)},
                                   {"created_s", entry.created_s}});
    }
  return result;
}
inline observe::ObjectDescription describe(const CatalogRead &catalog, Handle h, Step now,
                                           double hz) {
  return describe(catalog, h, now, hz, catalog.inspection_terms);
}
} // namespace amber::bindings::viewing::detail
