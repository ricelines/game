#include "catalog/catalog.hpp"
#include "perm/perm.hpp"
#include "react/react.hpp"
#include "step/step.hpp"
#include "store/store.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <lua.h>
#include <lualib.h>
#include <map>

namespace amber::step {
namespace {
std::uint32_t unsigned_arg(lua_State *L, int index, bool allow_zero = false) {
  if (lua_type(L, index) == LUA_TNUMBER) {
    const auto value = lua_tonumber(L, index);
    if (std::isfinite(value) && value >= (allow_zero ? 0 : 1) && value <= UINT32_MAX &&
        std::trunc(value) == value)
      return static_cast<std::uint32_t>(value);
  }
  luaL_error(L, "expected a %s integral id", allow_zero ? "nonnegative" : "positive");
}
std::uint32_t tagged_id(lua_State *L, int index, char prefix) {
  if (lua_type(L, index) == LUA_TNUMBER)
    return unsigned_arg(L, index);
  std::size_t size = 0;
  const auto *value = luaL_checklstring(L, index, &size);
  std::uint32_t id = 0;
  if (size > 2 && value[0] == prefix && value[1] == ':') {
    const auto parsed = std::from_chars(value + 2, value + size, id);
    if (parsed.ec == std::errc{} && parsed.ptr == value + size && id)
      return id;
  }
  luaL_error(L, "expected a positive id or %c:N", prefix);
}
ModelId model_arg(lua_State *L, int index) {
  return tagged_id(L, index, 'm');
}
TermId term_arg(lua_State *L, int index, const react::Terms &terms) {
  if (lua_type(L, index) == LUA_TSTRING) {
    std::size_t size;
    const auto *name = lua_tolstring(L, index, &size);
    const auto found = terms.by_name.find(std::string(name, size));
    if (found != terms.by_name.end())
      return found->second;
  } else if (lua_type(L, index) == LUA_TNUMBER) {
    const auto id = unsigned_arg(L, index);
    if (id < terms.rows.size())
      return id;
  }
  luaL_error(L, "unknown registered term");
}
struct PlotObjectsBinding {
  const ObjectState &objects;
  const perm::Plots &plots;
};
struct ObjectBinding {
  IdentityRead identity;
  TransformRead transforms;
  AuthorityRead authority;
  ParentRead parents;
  const std::vector<std::uint32_t> &params;
  const catalog::Models &models;
  const SparseColumn<std::vector<ScriptId>> &instances;
};
struct AssetReadBinding {
  IdentityRead identity;
  const std::vector<std::uint32_t> &params;
  const catalog::Models &models;
  const catalog::ParameterBlocks &parameters;
  const catalog::Assets &assets;
};
struct MaterialReadBinding {
  const react::Terms &terms;
  const react::Materials &materials;
};
struct ModelMeasureBinding {
  const react::Terms &terms;
  const react::Materials &materials;
  const catalog::Models &models;
  const catalog::ParameterBlocks &parameters;
  const catalog::Assets &assets;
  TermId density;
};
struct InstanceMeasureBinding {
  react::ValueRead values;
  IdentityRead identity;
  TransformRead transforms;
  const std::vector<std::uint32_t> &params;
  const Relation &declared_parts;
  const react::Terms &terms;
  const react::Materials &materials;
  const catalog::Models &models;
  const catalog::ParameterBlocks &parameters;
  const catalog::Assets &assets;
  TermId density;
};
struct MeterReadBinding {
  const react::MeterRows &meters;
  const react::Terms &terms;
  IdentityRead identity;
};
struct PlotReadBinding {
  const perm::State &permissions;
  float port_spacing_m;
  AuthorityRead authority;
};
void text(lua_State *L, const std::string &text) {
  lua_pushlstring(L, text.data(), text.size());
}
void handle_field(lua_State *L, const char *key, Handle handle) {
  if (handle)
    react::push_handle(L, handle);
  else
    lua_pushnil(L);
  lua_setfield(L, -2, key);
}
void number_field(lua_State *L, const char *key, double value) {
  lua_pushnumber(L, value);
  lua_setfield(L, -2, key);
}
void vector_field(lua_State *L, const char *key, Vec3 value) {
  lua_pushvector(L, value.x, value.y, value.z);
  lua_setfield(L, -2, key);
}
bool acquired(const react::Invocation &inv, Handle object) {
  return object == inv.self.object ||
         std::find(inv.acquired.begin(), inv.acquired.end(), object) != inv.acquired.end() ||
         std::find(inv.parts.begin(), inv.parts.end(), object) != inv.parts.end();
}
Handle object_arg(lua_State *L, int index, IdentityRead identity) {
  const auto object =
      lua_isnoneornil(L, index) ? react::invocation(L).self.object : react::check_handle(L, index);
  if (!identity.valid(object) || !acquired(react::invocation(L), object))
    luaL_error(L, "metadata needs a live acquired object");
  react::consult_object(react::invocation(L), object);
  return object;
}
void json_value(lua_State *L, const nlohmann::json &value) {
  if (value.is_null())
    lua_pushnil(L);
  else if (value.is_boolean())
    lua_pushboolean(L, value.get<bool>());
  else if (value.is_number())
    lua_pushnumber(L, value.get<double>());
  else if (value.is_string())
    text(L, value.get_ref<const std::string &>());
  else if (value.is_array()) {
    lua_createtable(L, static_cast<int>(value.size()), 0);
    int index = 1;
    for (const auto &entry : value) {
      json_value(L, entry);
      lua_rawseti(L, -2, index++);
    }
    lua_setreadonly(L, -1, true);
  } else {
    lua_createtable(L, 0, static_cast<int>(value.size()));
    for (const auto &[key, entry] : value.items()) {
      json_value(L, entry);
      lua_setfield(L, -2, key.c_str());
    }
    lua_setreadonly(L, -1, true);
  }
}
void scalar(lua_State *L, Value value) {
  switch (value.kind) {
  case Value::Kind::None:
    lua_pushnil(L);
    break;
  case Value::Kind::Number:
    lua_pushnumber(L, value.number);
    break;
  case Value::Kind::Integer:
    lua_pushnumber(L, static_cast<double>(value.integer));
    break;
  case Value::Kind::Boolean:
    lua_pushboolean(L, value.boolean);
    break;
  case Value::Kind::Object:
    react::push_handle(L, value.object);
    break;
  }
}
double mass(const react::Terms &terms, const react::Materials &materials, TermId density_term,
            const catalog::Asset &asset, TermId material) {
  if (!material || material >= terms.rows.size() ||
      terms.rows[material].kind != react::Kind::Material)
    throw std::invalid_argument("unknown model material");
  const auto density = react::read_intrinsic(materials, material, terms.rows.at(density_term));
  if (!density.present || !density.value.is_numeric() || !std::isfinite(density.value.numeric()) ||
      density.value.numeric() <= 0)
    throw std::invalid_argument("material has no positive measured density");
  return asset.volume * density.value.numeric();
}
void push_measurements(lua_State *L, ModelId model, std::uint32_t params, TermId material,
                       const std::map<TermId, double> &masses, space::Bounds bounds,
                       double volume) {
  lua_createtable(L, 0, 8);
  number_field(L, "model", model);
  number_field(L, "params", params);
  number_field(L, "material", material);
  double total = 0;
  lua_createtable(L, static_cast<int>(masses.size()), 0);
  int index = 1;
  for (const auto &[mat, amount] : masses) {
    total += amount;
    lua_createtable(L, 0, 2);
    number_field(L, "material", mat);
    number_field(L, "mass_kg", amount);
    lua_setreadonly(L, -1, true);
    lua_rawseti(L, -2, index++);
  }
  lua_setreadonly(L, -1, true);
  lua_setfield(L, -2, "materials");
  number_field(L, "mass_kg", total);
  number_field(L, "volume_m3", volume);
  lua_createtable(L, 0, 2);
  vector_field(L, "min", bounds.min);
  vector_field(L, "max", bounds.max);
  lua_setreadonly(L, -1, true);
  lua_setfield(L, -2, "bounds");
}
int plot_objects_binding(lua_State *L) {
  const auto &b = *static_cast<const PlotObjectsBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  auto plot = unsigned_arg(L, 2);
  if (plot >= b.plots.rows.size() || b.plots.rows[plot].id != plot)
    luaL_error(L, "unknown plot");
  lua_newtable(L);
  int index = 0;
  const auto things = plot_things(b.objects, plot, &i.deadline);
  if (i.deadline.passed())
    luaL_error(L, "handler deadline exceeded");
  for (auto object : things) {
    react::push_handle(L, object);
    lua_rawseti(L, -2, ++index);
    i.acquired.push_back(object);
  }
  return 1;
}
int object_binding(lua_State *L) {
  const auto &b = *static_cast<ObjectBinding *>(react::binding_data(L));
  const auto object = object_arg(L, 2, b.identity);
  const auto s = object.slot();
  const auto script = b.models.rows[b.identity.model[s]].script;
  lua_createtable(L, 0, 8);
  number_field(L, "model", b.identity.model[s]);
  number_field(L, "params", b.params[s]);
  number_field(L, "material", b.identity.material[s]);
  text(L, "p:" + std::to_string(b.authority.owner[s]));
  lua_setfield(L, -2, "owner");
  handle_field(L, "parent", b.parents.parent[s]);
  vector_field(L, "position", b.transforms.position[s]);
  const auto q = b.transforms.rotation[s];
  lua_createtable(L, 4, 0);
  const std::array rotation{q.x, q.y, q.z, q.w};
  for (int i = 0; i < 4; ++i) {
    lua_pushnumber(L, rotation[i]);
    lua_rawseti(L, -2, i + 1);
  }
  lua_setreadonly(L, -1, true);
  lua_setfield(L, -2, "rotation");
  if (script)
    number_field(L, "script", script);
  lua_newtable(L);
  int index = 1;
  if (script) {
    lua_pushnumber(L, script);
    lua_rawseti(L, -2, index++);
  }
  if (const auto *instances = b.instances.get(s))
    for (auto id : *instances) {
      lua_pushnumber(L, id);
      lua_rawseti(L, -2, index++);
    }
  lua_setreadonly(L, -1, true);
  lua_setfield(L, -2, "scripts");
  lua_setreadonly(L, -1, true);
  return 1;
}
int asset_binding(lua_State *L) {
  const auto &b = *static_cast<AssetReadBinding *>(react::binding_data(L));
  const auto object = object_arg(L, 2, b.identity);
  try {
    const auto effective = catalog::resolve_asset(
        b.models, b.parameters, b.identity.model[object.slot()], b.params[object.slot()]);
    auto metadata = b.assets.rows.at(effective.hash).manifest;
    for (const auto *field :
         {"bounds", "size", "volume", "supports", "capacity", "capacity_method", "assembly"})
      metadata.erase(field);
    json_value(L, metadata);
  } catch (const std::exception &error) {
    luaL_error(L, "%s", error.what());
  }
  return 1;
}
int material_binding(lua_State *L) {
  const auto &b = *static_cast<MaterialReadBinding *>(react::binding_data(L));
  const auto material = term_arg(L, 2, b.terms), term = term_arg(L, 3, b.terms);
  if (b.terms.rows[material].kind != react::Kind::Material ||
      b.terms.rows[term].source != react::Source::Intrinsic)
    luaL_error(L, "material reads require a material and intrinsic term");
  scalar(L, react::read_intrinsic(b.materials, material, b.terms.rows[term]).value);
  return 1;
}
int measure_model_binding(lua_State *L) {
  const auto &b = *static_cast<ModelMeasureBinding *>(react::binding_data(L));
  const auto model = model_arg(L, 2);
  const auto params = lua_isnoneornil(L, 3) ? 0 : unsigned_arg(L, 3, true);
  const auto override_material = lua_isnoneornil(L, 4) ? 0 : term_arg(L, 4, b.terms);
  try {
    const auto effective = catalog::resolve_asset(b.models, b.parameters, model, params);
    const auto &asset = b.assets.rows.at(effective.hash);
    const auto root_material =
        override_material ? override_material : b.models.rows[model].material;
    double volume = asset.volume;
    std::map<TermId, double> masses;
    masses[root_material] += mass(b.terms, b.materials, b.density, asset, root_material);
    for (const auto &[name, child] : catalog::part_models(b.models, b.parameters, model, params)) {
      (void)name;
      const auto &part = b.models.rows[child];
      const auto &part_asset = b.assets.rows.at(part.asset);
      volume += part_asset.volume;
      masses[part.material] += mass(b.terms, b.materials, b.density, part_asset, part.material);
    }
    auto minimum = asset.minimum, maximum = asset.maximum;
    if (const auto assembly = asset.manifest.find("assembly"); assembly != asset.manifest.end()) {
      const auto &bounds = assembly->at("bounds");
      const auto min = bounds.at("min").get<std::array<float, 3>>();
      const auto max = bounds.at("max").get<std::array<float, 3>>();
      minimum = {min[0], min[1], min[2]};
      maximum = {max[0], max[1], max[2]};
    }
    const auto supports = asset.manifest.value("supports", nlohmann::json::array());
    push_measurements(L, model, params, root_material, masses, {minimum, maximum}, volume);
    json_value(L, supports);
    lua_setfield(L, -2, "supports");
    lua_setreadonly(L, -1, true);
  } catch (const std::exception &error) {
    luaL_error(L, "%s", error.what());
  }
  return 1;
}
int measure_instance_binding(lua_State *L) {
  const auto &b = *static_cast<InstanceMeasureBinding *>(react::binding_data(L));
  const auto object = object_arg(L, 2, b.identity);
  try {
    std::vector<Handle> rows{object};
    double volume = 0;
    std::map<TermId, double> masses;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto h = rows[i];
      react::consult_object(react::invocation(L), h);
      const auto effective = catalog::resolve_asset(b.models, b.parameters,
                                                    b.identity.model[h.slot()], b.params[h.slot()]);
      const auto material = b.identity.material[h.slot()];
      const auto &asset = b.assets.rows.at(effective.hash);
      volume += asset.volume;
      masses[material] += mass(b.terms, b.materials, b.density, asset, material);
      if (const auto *children = b.declared_parts.out.get(h.slot()))
        for (auto child : *children)
          if (b.identity.valid(child))
            rows.push_back(child);
    }
    // Bounds are world-space and include actual moved declared children.
    Vec3 minimum{INFINITY, INFINITY, INFINITY}, maximum{-INFINITY, -INFINITY, -INFINITY};
    for (auto h : rows) {
      const auto effective = catalog::resolve_asset(b.models, b.parameters,
                                                    b.identity.model[h.slot()], b.params[h.slot()]);
      const auto &asset = b.assets.rows.at(effective.hash);
      for (unsigned corner = 0; corner < 8; ++corner) {
        const Vec3 local{corner & 1 ? asset.maximum.x : asset.minimum.x,
                         corner & 2 ? asset.maximum.y : asset.minimum.y,
                         corner & 4 ? asset.maximum.z : asset.minimum.z};
        const auto p =
            b.transforms.position[h.slot()] + rotate(b.transforms.rotation[h.slot()], local);
        minimum = {std::min(minimum.x, p.x), std::min(minimum.y, p.y), std::min(minimum.z, p.z)};
        maximum = {std::max(maximum.x, p.x), std::max(maximum.y, p.y), std::max(maximum.z, p.z)};
      }
    }
    const auto &inv = react::invocation(L);
    const auto capacity = react::read(b.values, object, b.values.capacity_term, inv.step, inv.hz);
    push_measurements(L, b.identity.model[object.slot()], b.params[object.slot()],
                      b.identity.material[object.slot()], masses, {minimum, maximum}, volume);
    number_field(L, "capacity_m3", capacity.present ? capacity.value.numeric() : 0);
    lua_setreadonly(L, -1, true);
  } catch (const std::exception &error) {
    luaL_error(L, "%s", error.what());
  }
  return 1;
}
int meter_binding(lua_State *L) {
  const auto &b = *static_cast<MeterReadBinding *>(react::binding_data(L));
  const auto subject = object_arg(L, 2, b.identity);
  const auto term = term_arg(L, 3, b.terms);
  const auto contributor = object_arg(L, 4, b.identity);
  const bool supplier = lua_toboolean(L, 5);
  const auto found = b.meters.find({subject, contributor, term, supplier});
  const auto &inv = react::invocation(L);
  lua_pushnumber(L, found == b.meters.end() ? 0 : react::metered(found->second, inv.step, inv.hz));
  return 1;
}
int plot_binding(lua_State *L) {
  const auto &b = *static_cast<PlotReadBinding *>(react::binding_data(L));
  const auto &plots = b.permissions.plots;
  const auto &inv = react::invocation(L);
  const auto id =
      lua_isnoneornil(L, 2) ? b.authority.plot[inv.self.object.slot()] : unsigned_arg(L, 2);
  if (!id || id >= plots.rows.size() || plots.rows[id].id != id)
    luaL_error(L, "unknown plot");
  json_value(L, perm::plot_record(plots, b.permissions.grants, id, b.port_spacing_m, inv.step,
                                  b.permissions.offers, 0));
  return 1;
}
int plots_binding(lua_State *L) {
  const auto &b = *static_cast<PlotReadBinding *>(react::binding_data(L));
  auto &inv = react::invocation(L);
  const bool all = lua_isnoneornil(L, 2);
  lua_newtable(L);
  int index = 0;
  const auto append = [&](PlotId id) {
    lua_pushnumber(L, id);
    lua_rawseti(L, -2, ++index);
  };
  if (all) {
    for (const auto &plot : b.permissions.plots.rows) {
      if (inv.deadline.passed())
        luaL_error(L, "handler deadline exceeded");
      if (plot.id)
        append(plot.id);
    }
  } else {
    std::size_t size = 0;
    const auto *text = luaL_checklstring(L, 2, &size);
    PlayerId owner = 0;
    if (size < 3 || text[0] != 'p' || text[1] != ':')
      luaL_error(L, "plot owner must be p:N");
    const auto parsed = std::from_chars(text + 2, text + size, owner);
    if (parsed.ec != std::errc{} || parsed.ptr != text + size)
      luaL_error(L, "plot owner must be p:N");
    const auto found = b.permissions.plots.by_owner.find(owner);
    if (found != b.permissions.plots.by_owner.end())
      for (auto id : found->second) {
        if (inv.deadline.passed())
          luaL_error(L, "handler deadline exceeded");
        append(id);
      }
  }
  if (inv.deadline.passed())
    luaL_error(L, "handler deadline exceeded");
  lua_setreadonly(L, -1, true);
  return 1;
}
} // namespace
std::vector<Handle> plot_things(const ObjectState &state, PlotId id, const RunDeadline *deadline) {
  std::vector<Handle> result;
  const auto found = state.objects.on_plot.find(id);
  if (found == state.objects.on_plot.end())
    return result;
  const auto identity = amber::identity(state.objects);
  auto budget = deadline ? *deadline : RunDeadline{};
  for (auto slot : found->second) {
    if (deadline && budget.passed())
      break;
    if (identity.flags[slot] & (Body | Held))
      continue;
    const auto *part_of = state.declared_parts.in.get(slot);
    if (!part_of || part_of->empty())
      result.push_back(identity.at(slot));
  }
  return result;
}
void bind_metadata(const amber::ObjectState &objects_state,
                   const amber::catalog::State &catalog_state, amber::react::State &react_state,
                   const perm::State &permissions, float port_spacing_m) {
  const auto &plots = permissions.plots;
  const auto values = amber::react::value_read(objects_state, catalog_state, react_state);

  auto &vm = react_state.vm;

  auto transforms = amber::transforms(std::as_const(objects_state.objects));
  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto parents = amber::parents(std::as_const(objects_state.objects));
  auto &models = catalog_state.models;
  auto &instances = react_state.installations;
  auto &parameters = catalog_state.parameters;
  auto &assets = catalog_state.assets;
  auto &declared_parts = objects_state.declared_parts;
  auto &meters = react_state.meters;

  const auto identity = values.identity;
  const auto &params = values.params;
  const auto &terms = values.terms;
  const auto &materials = values.materials;
  const auto density = values.density;
  react::register_binding(
      vm, "object", object_binding,
      std::make_shared<ObjectBinding>(
          ObjectBinding{identity, transforms, authority, parents, params, models, instances}));
  react::register_binding(vm, "asset", asset_binding,
                          std::make_shared<AssetReadBinding>(
                              AssetReadBinding{identity, params, models, parameters, assets}));
  react::register_binding(
      vm, "material", material_binding,
      std::make_shared<MaterialReadBinding>(MaterialReadBinding{terms, materials}));
  react::register_binding(vm, "measure_model", measure_model_binding,
                          std::make_shared<ModelMeasureBinding>(ModelMeasureBinding{
                              terms, materials, models, parameters, assets, density}));
  react::register_binding(vm, "measure_instance", measure_instance_binding,
                          std::make_shared<InstanceMeasureBinding>(InstanceMeasureBinding{
                              values, identity, transforms, params, declared_parts, terms,
                              materials, models, parameters, assets, density}));
  react::register_binding(
      vm, "meter", meter_binding,
      std::make_shared<MeterReadBinding>(MeterReadBinding{meters, terms, identity}));
  react::register_binding(
      vm, "plot", plot_binding,
      std::make_shared<PlotReadBinding>(PlotReadBinding{permissions, port_spacing_m, authority}));
  react::register_binding(
      vm, "plots", plots_binding,
      std::make_shared<PlotReadBinding>(PlotReadBinding{permissions, port_spacing_m, authority}));
  react::register_binding(
      vm, "plot_objects", plot_objects_binding,
      std::make_shared<PlotObjectsBinding>(PlotObjectsBinding{objects_state, plots}));
}
} // namespace amber::step
