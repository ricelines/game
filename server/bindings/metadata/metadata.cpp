#include "metadata.hpp"
#include "bindings/arguments/arguments.hpp"
#include "bindings/geometry/geometry.hpp"
#include "bindings/illumination.hpp"
#include "bindings/viewing/viewing.hpp"
#include "control/control.hpp"
#include "drive/drive.hpp"

#include <algorithm>
#include <lua.h>
#include <lualib.h>
#include <map>
#include <numeric>

namespace amber::bindings {
void invalidate_metadata(MetadataChanges &changes, std::span<const Handle> objects) {
  viewing::invalidate(changes.views, objects);
  changes.objects.insert(changes.objects.end(), objects.begin(), objects.end());
}
void metadata_changed(MetadataChanges &changes, std::span<const Handle> objects,
                      std::span<const ModelId> models) {
  invalidate_metadata(changes, objects);
  sync_illuminance_reads(changes.illuminance, objects);
  changes.models.insert(changes.models.end(), models.begin(), models.end());
}

CatalogPublication::CatalogPublication(
    const catalog::Models &models, const catalog::Scripts &scripts, const react::Terms &terms,
    const react::Materials &materials, const catalog::ModelTexts &model_text,
    const catalog::InstanceTexts &instance_text, const react::ValueRead &values,
    AuthorityRead authority, ParentRead parents, react::Triggers &triggers,
    react::PriorityLinks &priorities, MetadataChanges &changes, MaterialAppearanceRoles appearance)
    : models_(models), scripts_(scripts), terms_(terms), materials_(materials),
      model_text_(model_text), instance_text_(instance_text), values_(values),
      authority_(authority), parents_(parents), triggers_(triggers), priorities_(priorities),
      changes_(changes), appearance_(appearance), model_ids_(models.rows.size() - 1),
      term_ids_(terms.rows.size() - 1) {
  std::iota(model_ids_.begin(), model_ids_.end(), 1);
  std::iota(term_ids_.begin(), term_ids_.end(), 1);
  for (const auto &term : terms.rows) {
    if (term.kind == react::Kind::Material)
      material_ids_.push_back(term.id);
    if (term.id && (term.kind == react::Kind::Quantity || term.kind == react::Kind::Flag))
      inspection_terms_.push_back(term.id);
  }
  catalog_ = {{"t", "catalog"},
              {"models", model_metadata(models_, model_text_, model_ids_)},
              {"terms", term_metadata(terms_, term_ids_)},
              {"materials", material_metadata(terms_, materials_, appearance_, material_ids_)},
              {"world", {{"ambient_lux", 0}}}};
}
void CatalogPublication::definitions_changed(std::span<const ModelId> changed) {
  while (model_ids_.size() + 1 < models_.rows.size())
    model_ids_.push_back(static_cast<ModelId>(model_ids_.size() + 1));
  for (auto model : changed)
    install::project_model(triggers_, models_, scripts_, terms_, model);
  metadata_changed(changes_, {}, changed);
}
void CatalogPublication::registry_changed(std::span<const TermId> terms,
                                          std::span<const TermId> materials) {
  added_terms_.insert(added_terms_.end(), terms.begin(), terms.end());
  changed_materials_.insert(changed_materials_.end(), materials.begin(), materials.end());
}
void CatalogPublication::flush(control::Sessions &sessions) {
  const auto deduplicate = [](auto &rows) {
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
  };
  deduplicate(added_terms_);
  deduplicate(changed_materials_);
  if (!added_terms_.empty()) {
    for (auto id : added_terms_) {
      term_ids_.push_back(id);
      const auto &term = terms_.rows[id];
      if (term.kind == react::Kind::Quantity || term.kind == react::Kind::Flag)
        inspection_terms_.push_back(id);
      if (term.kind == react::Kind::Material)
        material_ids_.push_back(id);
    }
    react::rebuild_priorities(priorities_, values_);
    catalog_["terms"] = term_metadata(terms_, term_ids_);
    sessions.broadcast({{"t", "catalog"}, {"terms", term_metadata(terms_, added_terms_)}});
    added_terms_.clear();
  }
  if (!changed_materials_.empty()) {
    catalog_["materials"] = material_metadata(terms_, materials_, appearance_, material_ids_);
    sessions.broadcast(
        {{"t", "catalog"},
         {"materials", material_metadata(terms_, materials_, appearance_, changed_materials_)}});
    changed_materials_.clear();
  }
  if (!changes_.models.empty()) {
    auto &changed = changes_.models;
    deduplicate(changed);
    catalog_["models"] = model_metadata(models_, model_text_, model_ids_);
    sessions.broadcast(
        {{"t", "catalog"}, {"models", model_metadata(models_, model_text_, changed)}});
    changed.clear();
  }
  if (!changes_.objects.empty()) {
    auto &changed = changes_.objects;
    deduplicate(changed);
    sessions.broadcast({{"t", "metadata"},
                        {"objects", object_metadata(values_.identity, authority_, parents_,
                                                    model_text_, instance_text_, changed)}});
    changed.clear();
  }
}
catalog::Hash commit_registry(void *local, std::span<const TermId> terms,
                              std::span<const TermId> materials) {
  auto &state = *static_cast<RegistryPersistence *>(local);
  const auto physics =
      prepare_material_refresh(state.physics, materials, state.clock.step, state.hz);
  const auto hash = catalog::save_registry(state.directory, state.terms, state.materials,
                                           state.rules, state.mechanical);
  state.document = catalog::read_registry(state.directory, hash);
  apply_material_refresh(state.physics.space, state.physics.values.identity, physics);
  state.publication.registry_changed(terms, materials);
  return hash;
}
void publish_rule_registry(void *local, ScriptId script, bool approved, bool active, Step now,
                           double, const record::Origin &, react::DeferredEffects &effects) {
  auto &state = *static_cast<RegistryPersistence *>(local);
  const auto pin = catalog::save_registry(state.directory, state.terms, state.materials,
                                          state.rules, state.mechanical);
  state.document = catalog::read_registry(state.directory, pin);
  const auto &source = state.scripts.rows.at(script);
  record::Event event;
  event.step = now;
  event.kind = record::Kind::Registry;
  event.actor = record::Actor::script(script);
  event.script_author = source.author;
  event.name = "rule_activation";
  event.changed = true;
  event.result = record::Result::Applied;
  event.detail = {{"script", "s:" + std::to_string(script)},
                  {"approved", approved},
                  {"active", active},
                  {"registry", pin}};
  event.credits = {{source.author, source.created_s, false}};
  effects.ordered.emplace_back(std::move(event));
}
namespace {
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
                       const std::map<TermId, double> &masses, Vec3 minimum, Vec3 maximum) {
  lua_createtable(L, 0, 6);
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
  lua_createtable(L, 0, 2);
  vector_field(L, "min", minimum);
  vector_field(L, "max", maximum);
  lua_setreadonly(L, -1, true);
  lua_setfield(L, -2, "bounds");
  lua_setreadonly(L, -1, true);
}
} // namespace
int context_binding(lua_State *L) {
  const auto &b = *static_cast<ContextBinding *>(react::binding_data(L));
  const auto &inv = react::invocation(L);
  lua_createtable(L, 0, 9);
  number_field(L, "script", inv.self.script);
  handle_field(L, "object", inv.self.object);
  handle_field(L, "sender", inv.sender);
  text(L, "p:" + std::to_string(b.authority.owner[inv.self.object.slot()]));
  lua_setfield(L, -2, "owner");
  if (inv.action_caller) {
    if (const auto *player = std::get_if<PlayerId>(&*inv.action_caller)) {
      text(L, "p:" + std::to_string(*player));
      lua_setfield(L, -2, "actor");
      handle_field(L, "body", inv.action_body);
    } else {
      const auto &script = std::get<ScriptHolder>(*inv.action_caller);
      number_field(L, "caller_script", script.script);
      handle_field(L, "caller_object", script.object);
    }
    handle_field(L, "selected", inv.action_target);
  }
  if (inv.direct_body)
    handle_field(L, "held", inv.direct_body->held);
  lua_setreadonly(L, -1, true);
  return 1;
}
int object_binding(lua_State *L) {
  const auto &b = *static_cast<ObjectBinding *>(react::binding_data(L));
  const auto object = object_arg(L, 2, b.identity);
  const auto s = object.slot();
  const auto script = b.models.rows[b.identity.model[s]].script;
  lua_createtable(L, 0, 9);
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
    json_value(L, b.assets.rows.at(effective.hash).manifest);
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
    std::map<TermId, double> masses;
    masses[root_material] += mass(b.terms, b.materials, b.density, asset, root_material);
    for (const auto &[name, child] : catalog::part_models(b.models, b.parameters, model, params)) {
      (void)name;
      const auto &part = b.models.rows[child];
      masses[part.material] +=
          mass(b.terms, b.materials, b.density, b.assets.rows.at(part.asset), part.material);
    }
    auto minimum = asset.minimum, maximum = asset.maximum;
    if (const auto assembly = asset.manifest.find("assembly"); assembly != asset.manifest.end()) {
      const auto &bounds = assembly->at("bounds");
      const auto min = bounds.at("min").get<std::array<float, 3>>();
      const auto max = bounds.at("max").get<std::array<float, 3>>();
      minimum = {min[0], min[1], min[2]};
      maximum = {max[0], max[1], max[2]};
    }
    push_measurements(L, model, params, root_material, masses, minimum, maximum);
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
    std::map<TermId, double> masses;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto h = rows[i];
      react::consult_object(react::invocation(L), h);
      const auto effective = catalog::resolve_asset(b.models, b.parameters,
                                                    b.identity.model[h.slot()], b.params[h.slot()]);
      const auto material = b.identity.material[h.slot()];
      masses[material] +=
          mass(b.terms, b.materials, b.density, b.assets.rows.at(effective.hash), material);
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
    push_measurements(L, b.identity.model[object.slot()], b.params[object.slot()],
                      b.identity.material[object.slot()], masses, minimum, maximum);
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
  const auto id = lua_isnoneornil(L, 2) ? b.authority.plot[react::invocation(L).self.object.slot()]
                                        : unsigned_arg(L, 2);
  if (!id || id >= b.plots.rows.size() || b.plots.rows[id].id != id)
    luaL_error(L, "unknown plot");
  const auto &plot = b.plots.rows[id];
  lua_createtable(L, 0, 5);
  number_field(L, "id", id);
  text(L, "p:" + std::to_string(plot.owner));
  lua_setfield(L, -2, "owner");
  number_field(L, "group", plot.group);
  number_field(L, "preset", plot.preset);
  lua_createtable(L, 0, 2);
  for (int corner = 0; corner < 2; ++corner) {
    lua_createtable(L, 2, 0);
    for (int axis = 0; axis < 2; ++axis) {
      lua_pushnumber(L, plot.bounds[corner * 2 + axis]);
      lua_rawseti(L, -2, axis + 1);
    }
    lua_setreadonly(L, -1, true);
    lua_setfield(L, -2, corner ? "max" : "min");
  }
  lua_setreadonly(L, -1, true);
  lua_setfield(L, -2, "bounds");
  lua_setreadonly(L, -1, true);
  return 1;
}
} // namespace amber::bindings
