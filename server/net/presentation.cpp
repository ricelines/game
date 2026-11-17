#include "catalog/catalog.hpp"
#include "net.hpp"
#include "react/react.hpp"

#include <algorithm>
#include <stdexcept>

namespace amber::net {
namespace {
using Json = nlohmann::json;
Json vec(Vec3 p) {
  return {p.x, p.y, p.z};
}
Json text(const catalog::Text &t) {
  return {{"name", t.name}, {"description", t.description}, {"writing", t.writing}};
}
Json scalar_value(Value value) {
  switch (value.kind) {
  case Value::Kind::None:
    return nullptr;
  case Value::Kind::Number:
    return value.number;
  case Value::Kind::Integer:
    return value.integer;
  case Value::Kind::Boolean:
    return value.boolean;
  case Value::Kind::Object:
    return to_string(value.object);
  }
  return nullptr;
}
const char *kind(react::Kind k) {
  constexpr const char *names[]{"quantity", "flag", "event", "relation", "action", "material"};
  return names[static_cast<unsigned>(k)];
}
Vec3 vector(const Json &j) {
  return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}
ScalarBinding binding_scalar(const Json &j, const react::Terms &terms) {
  if (j.is_number())
    return j.get<double>();
  return TermRef{terms.by_name.at(j.get<std::string>())};
}
} // namespace
const catalog::Text &object_text(const catalog::State &catalog, Handle object, ModelId model) {
  if (auto it = catalog.instance_text.rows.find(object.bits);
      it != catalog.instance_text.rows.end())
    return it->second;
  if (auto it = catalog.model_text.rows.find(model); it != catalog.model_text.rows.end())
    return it->second;
  static const catalog::Text empty;
  return empty;
}
double binding_value(const ScalarBinding &s, Handle h, const react::ValueRead &values, Step now,
                     double hz) {
  if (const auto *constant = std::get_if<double>(&s))
    return *constant;
  auto value = react::read(values, h, std::get<TermRef>(s).id, now, hz);
  return value.present ? value.value.numeric() : 0;
}
ObjectDescription describe_object(Handle h, IdentityRead identity,
                                  const amber::catalog::State &catalog_state,
                                  const react::ValueRead &values, std::span<const TermId> inspected,
                                  std::span<const std::string> actions, Step now, double hz) {
  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;
  auto &assets = catalog_state.assets;

  auto model = identity.model[h.slot()], material = identity.material[h.slot()];
  auto effective = catalog::resolve_asset(models, parameters, model, values.params[h.slot()]);
  const auto &asset = assets.rows.at(effective.hash);
  const auto capacity = react::read(values, h, values.capacity_term, now, hz);
  Json measured{{"asset", asset.hash},
                {"params", asset.manifest.at("params")},
                {"size_m", vec(asset.maximum - asset.minimum)},
                {"volume_m3", asset.volume},
                {"capacity_l", capacity.present ? capacity.value.numeric() * 1000 : 0},
                {"material", values.terms.rows.at(material).name},
                {"values", Json::object()}};
  if (values.density) {
    const auto density =
        react::read_intrinsic(values.materials, material, values.terms.rows.at(values.density));
    if (density.present)
      measured["mass_kg"] = react::read_mass(asset.volume, density.value.numeric());
  }
  for (auto term : inspected) {
    auto value = react::read(values, h, term, now, hz);
    if (value.present)
      measured["values"][values.terms.rows.at(term).name] = scalar_value(value.value);
  }
  return {h,
          "m:" + std::to_string(model),
          std::move(measured),
          object_text(catalog_state, h, model),
          {actions.begin(), actions.end()}};
}
Json object_metadata(IdentityRead identity, AuthorityRead authority, ParentRead parents,
                     const catalog::State &catalog, std::span<const Handle> handles) {
  Json rows = Json::array();
  for (auto h : handles)
    if (identity.valid(h)) {
      auto model = identity.model[h.slot()];
      auto parent = parents.parent[h.slot()];
      rows.push_back({{"h", to_string(h)},
                      {"model", model},
                      {"material", identity.material[h.slot()]},
                      {"owner", "p:" + std::to_string(authority.owner[h.slot()])},
                      {"parent", parent ? Json(to_string(parent)) : Json(nullptr)},
                      {"text", text(object_text(catalog, h, model))}});
    }
  return rows;
}
Json model_metadata(const catalog::Models &models, const catalog::ModelTexts &texts,
                    std::span<const ModelId> ids) {
  Json rows = Json::array();
  for (auto id : ids) {
    const auto &model = models.rows.at(id);
    auto it = texts.rows.find(id);
    rows.push_back({{"id", id},
                    {"asset", model.asset},
                    {"material", model.material},
                    {"text", text(it == texts.rows.end() ? catalog::Text{} : it->second)},
                    {"bindings", Json::object()}});
  }
  return rows;
}
Json term_metadata(const react::Terms &terms, std::span<const TermId> ids) {
  Json rows = Json::array();
  for (auto id : ids) {
    const auto &term = terms.rows.at(id);
    rows.push_back({{"id", id},
                    {"name", term.name},
                    {"kind", kind(term.kind)},
                    {"unit", react::format_unit(term.dimensions)},
                    {"dimensions", term.dimensions}});
  }
  return rows;
}
Json material_metadata(const react::Terms &terms, const react::Materials &materials,
                       MaterialAppearanceRoles roles, std::span<const TermId> ids) {
  for (auto id : {roles.red, roles.green, roles.blue, roles.alpha, roles.metallic, roles.roughness})
    if (id) {
      const auto &t = terms.rows.at(id);
      if (t.kind != react::Kind::Quantity || t.source != react::Source::Intrinsic ||
          t.dimensions != std::array<std::int8_t, 8>{})
        throw std::invalid_argument(
            "material appearance role requires dimensionless intrinsic quantity");
    }
  auto read = [&](TermId material, TermId role, double fallback) {
    if (!role)
      return fallback;
    auto answer = react::read_intrinsic(materials, material, terms.rows.at(role));
    return answer.present ? std::clamp(answer.value.numeric(), 0., 1.) : fallback;
  };
  Json rows = Json::array();
  for (auto id : ids)
    rows.push_back({{"id", id},
                    {"name", terms.rows.at(id).name},
                    {"color",
                     {read(id, roles.red, .65), read(id, roles.green, .65),
                      read(id, roles.blue, .65), read(id, roles.alpha, 1)}},
                    {"metallic", read(id, roles.metallic, 0)},
                    {"roughness", read(id, roles.roughness, .8)}});
  return rows;
}
std::optional<LightRole> light_binding(const catalog::Asset &asset, const react::Terms &terms,
                                       Handle source, float source_radius) {
  const auto &all = asset.manifest.at("bindings");
  if (!all.contains("light"))
    return {};
  const auto &j = all.at("light");
  return LightRole{source, vector(j.at("site")), binding_scalar(j.at("intensity"), terms),
                   j.at("range").get<float>(), j.value("source_radius_m", source_radius)};
}
std::optional<ZoneBinding> zone_binding(const catalog::Asset &asset, const react::Terms &terms) {
  const auto &all = asset.manifest.at("bindings");
  if (!all.contains("zone"))
    return {};
  const auto &j = all.at("zone");
  ZoneBinding result{binding_scalar(j.at("ambient"), terms), j.value("priority", 0), {}};
  if (j.contains("shape"))
    result.box = BoxSensor{vector(j.at("shape").at("size")), vector(j.at("shape").at("offset"))};
  return result;
}
std::optional<ObserverBinding> observer_binding(const catalog::Asset &asset,
                                                const react::Terms &terms) {
  const auto &all = asset.manifest.at("bindings");
  if (!all.contains("observer"))
    return {};
  const auto &j = all.at("observer");
  return ObserverBinding{vector(j.at("eye")), vector(j.at("forward")),
                         binding_scalar(j.at("dim_lux"), terms)};
}
observe::Observer observer(const ObserverBinding &binding, Handle body, TransformRead transforms,
                           const react::ValueRead &values, Step now, double hz) {
  space::Pose pose{transforms.position[body.slot()], transforms.rotation[body.slot()]};
  auto eye = space::transform_point(pose, binding.eye),
       forward = space::transform_point({{}, pose.rotation}, binding.forward);
  return {body, eye, forward, binding_value(binding.dim_lux, body, values, now, hz)};
}
} // namespace amber::net
