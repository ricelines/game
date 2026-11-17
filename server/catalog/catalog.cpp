#include "catalog.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace amber::catalog {
namespace {
Vec3 vector3(const nlohmann::json &value) {
  if (!value.is_array() || value.size() != 3)
    throw std::runtime_error("expected a three-component vector");
  Vec3 result{value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
  if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z))
    throw std::runtime_error("vector must be finite");
  return result;
}
TermId named(const react::Terms &terms, const nlohmann::json &value) {
  if (value.is_number_unsigned() || value.is_number_integer()) {
    auto id = value.get<TermId>();
    if (id && id < terms.rows.size())
      return id;
  }
  if (value.is_string()) {
    auto found = terms.by_name.find(value.get<std::string>());
    if (found != terms.by_name.end())
      return found->second;
  }
  throw std::runtime_error("unknown manifest term");
}
ScriptId script_default(const Scripts &scripts, const nlohmann::json &manifest) {
  if (!manifest.contains("default_script") || manifest["default_script"].is_null())
    return 0;
  const auto &value = manifest["default_script"];
  if (value.is_number_unsigned() || value.is_number_integer()) {
    const auto id = value.get<ScriptId>();
    if (id && id < scripts.rows.size())
      return id;
  }
  if (value.is_string()) {
    const auto hash = value.get<std::string>();
    for (const auto &script : scripts.rows)
      if (script.id && script.artifact == hash)
        return script.id;
  }
  throw std::runtime_error("unknown default script");
}
} // namespace
void resolve_manifest_terms(Asset &asset, const react::Terms &terms) {
  asset.replicated.clear();
  const auto &bindings = asset.manifest.at("bindings");
  if (!bindings.is_object())
    throw std::runtime_error("bindings must be an object");
  auto scalar = [&](const nlohmann::json &reference,
                    std::optional<std::array<std::int8_t, 8>> dimensions = {}) {
    if (reference.is_number()) {
      if (!std::isfinite(reference.get<double>()) || (dimensions && reference.get<double>() < 0))
        throw std::runtime_error("binding constant must be finite");
      return;
    }
    const auto id = named(terms, reference);
    const auto &term = terms.rows[id];
    if (term.kind != react::Kind::Quantity && term.kind != react::Kind::Flag)
      throw std::runtime_error("binding requires a scalar term");
    if (dimensions && term.kind != react::Kind::Quantity)
      throw std::runtime_error("mechanical scalar binding requires a quantity");
    if (dimensions && term.dimensions != *dimensions)
      throw std::runtime_error("mechanical binding dimensions mismatch");
  };
  if (bindings.contains("render"))
    for (const auto &binding : bindings["render"]) {
      scalar(binding.at("term"));
      const auto property = binding.at("property").get<std::string>();
      if (property != "color" && property != "emission" && property != "opacity" &&
          property != "visibility" && property != "animation_weight" &&
          property != "animation_rate")
        throw std::runtime_error("unknown render binding property");
    }
  if (bindings.contains("light")) {
    const auto &light = bindings["light"];
    scalar(light.at("intensity"), std::array<std::int8_t, 8>{0, 0, 0, 0, 0, 0, 1, 0});
    (void)vector3(light.at("site"));
    const double range = light.at("range").get<double>();
    if (!std::isfinite(range) || range <= 0)
      throw std::runtime_error("light range must be positive finite");
    if (light.contains("source_radius_m")) {
      const auto radius = light["source_radius_m"].get<double>();
      if (!std::isfinite(radius) || radius <= 0)
        throw std::runtime_error("light source radius must be positive finite");
    }
    (void)vector3(light.at("color"));
  }
  if (bindings.contains("zone")) {
    const auto &zone = bindings["zone"];
    scalar(zone.at("ambient"), std::array<std::int8_t, 8>{-2, 0, 0, 0, 0, 0, 1, 0});
    if (!zone.at("priority").is_number_integer())
      throw std::runtime_error("zone priority must be an integer");
    const auto &shape = zone.at("shape");
    if (shape.at("kind") != "box")
      throw std::runtime_error("zone shape must be a box");
    const auto size = vector3(shape.at("size"));
    if (size.x <= 0 || size.y <= 0 || size.z <= 0)
      throw std::runtime_error("zone size must be positive");
    (void)vector3(shape.at("offset"));
  }
  if (bindings.contains("observer")) {
    const auto &observer = bindings["observer"];
    (void)vector3(observer.at("eye"));
    const auto forward = vector3(observer.at("forward"));
    if (forward.x == 0 && forward.y == 0)
      throw std::runtime_error("observer forward needs nonzero XY projection");
    scalar(observer.at("dim_lux"), std::array<std::int8_t, 8>{-2, 0, 0, 0, 0, 0, 1, 0});
  }

  const auto &replicated = asset.manifest.at("replicated");
  if (!replicated.is_array() || replicated.size() > 255)
    throw std::runtime_error("replication count exceeds wire range");
  for (const auto &name : replicated) {
    const auto id = named(terms, name);
    const auto kind = terms.rows[id].kind;
    if (id > 65535 || (kind != react::Kind::Quantity && kind != react::Kind::Flag))
      throw std::runtime_error("replicated term outside scalar wire range");
    if (std::find(asset.replicated.begin(), asset.replicated.end(), id) != asset.replicated.end())
      throw std::runtime_error("duplicate replicated term");
    asset.replicated.push_back(id);
  }
}
ScriptId import_script(Scripts &scripts, PreparedScript prepared, PlayerId author,
                       double created_s) {
  const auto id = static_cast<ScriptId>(scripts.rows.size());
  prepared.declaration.id = id;
  prepared.declaration.author = author;
  scripts.rows.push_back(
      {id, std::move(prepared.hash), std::move(prepared.source_hash),
       std::move(prepared.bytecode_hash), std::move(prepared.registry_pin),
       std::move(prepared.pins), author, created_s, std::move(prepared.declaration),
       prepared.metadata.value("tests", nlohmann::json::array()), std::move(prepared.source)});
  return id;
}
void project_settings(react::ModelSettings &settings, const react::Terms &terms, ModelId model,
                      const react::Values &values) {
  for (const auto &[id, value] : values) {
    (void)value;
    if (!id || id >= terms.rows.size() || terms.rows[id].source != react::Source::Setting)
      throw std::runtime_error("model defaults require setting terms");
  }
  settings.rows[model] = values;
}
ModelRegistration submit_model(Models &models, ModelTexts &texts, react::ModelSettings &settings,
                               const Assets &assets, const Scripts &scripts,
                               const react::Terms &terms, const ModelSubmission &submission) {
  const auto &root = assets.rows.at(submission.asset);
  struct Pending {
    const Asset *asset;
    TermId material;
    ScriptId script;
    Text text;
    std::string name;
  };
  std::vector<Pending> pending;
  auto defaults = [&](const Asset &asset, TermId material, ScriptId script, Text text,
                      std::string name) {
    if (!material)
      material = named(terms, asset.manifest.at("default_material"));
    if (terms.rows.at(material).kind != react::Kind::Material)
      throw std::runtime_error("model material is not a material term");
    if (!script)
      script = script_default(scripts, asset.manifest);
    if (script >= scripts.rows.size() || (script && scripts.rows[script].id != script))
      throw std::runtime_error("unknown model script");
    if (script && (scripts.rows[script].declaration.kind == react::ScriptDeclaration::Kind::Rule ||
                   scripts.rows[script].declaration.kind == react::ScriptDeclaration::Kind::World))
      throw std::runtime_error("a world script or rule is not a model script");
    pending.push_back({&asset, material, script, std::move(text), std::move(name)});
  };
  defaults(root, submission.material, submission.script, submission.text, {});
  for (const auto &[name, part] : root.manifest.at("parts").items()) {
    const auto &asset = assets.rows.at(part.at("asset").get<std::string>());
    if (asset.parent_asset != root.hash || asset.part_name != name)
      throw std::runtime_error("child asset identity does not match parent");
    defaults(asset, 0, 0, Text{name, "Part of " + submission.text.name, {}}, name);
  }
  for (const auto &[id, value] : submission.settings) {
    (void)value;
    if (!id || id >= terms.rows.size() || terms.rows[id].source != react::Source::Setting)
      throw std::runtime_error("model default is not a setting");
  }
  ModelRegistration result;
  result.root = static_cast<ModelId>(models.rows.size());
  for (auto &entry : pending) {
    const auto id = static_cast<ModelId>(models.rows.size());
    models.rows.push_back({id,
                           entry.asset->hash,
                           entry.asset->source_hash,
                           entry.asset->part_name,
                           entry.asset->canonical_params,
                           entry.material,
                           submission.author,
                           submission.created_s,
                           entry.script,
                           {}});
    texts.rows.emplace(id, std::move(entry.text));
    if (!entry.name.empty())
      result.parts[entry.name] = id;
  }
  models.rows[result.root].parts = result.parts;
  project_settings(settings, terms, result.root, submission.settings);
  return result;
}
std::uint32_t add_variant(State &family, const State &catalog_state, react::ModelSettings &settings,
                          const react::Terms &terms, ModelId model, Hash hash,
                          std::string canonical) {
  auto &blocks = family.parameters;
  auto &models = family.models;
  auto &texts = family.model_text;
  auto &assets = catalog_state.assets;
  auto &scripts = catalog_state.scripts;

  const auto source = models.rows.at(model);
  const auto &asset = assets.rows.at(hash);
  if (!model || source.id != model || source.source_hash != asset.source_hash ||
      source.part_name != asset.part_name || canonical != asset.canonical_params)
    throw std::runtime_error("variant does not belong to model source");
  for (const auto &block : blocks.rows)
    if (block.id && block.model == model && block.asset == hash)
      return block.id;
  // Preflight all child references/defaults before allocating any model identity.
  for (const auto &[name, part] : asset.manifest.at("parts").items()) {
    const auto &child = assets.rows.at(part.at("asset").get<std::string>());
    if (child.parent_asset != asset.hash || child.part_name != name)
      throw std::runtime_error("variant part identity mismatch");
    const auto material = named(terms, child.manifest.at("default_material"));
    if (terms.rows[material].kind != react::Kind::Material)
      throw std::runtime_error("part material is not a material");
    (void)script_default(scripts, child.manifest);
  }
  const auto parent_name = texts.rows.at(model).name;
  std::map<std::string, ModelId> children;
  for (const auto &[name, part] : asset.manifest.at("parts").items()) {
    ModelSubmission child;
    child.asset = part.at("asset");
    child.author = source.author;
    child.created_s = source.created_s;
    child.text = {name, "Part of " + parent_name, {}};
    children[name] = submit_model(models, texts, settings, assets, scripts, terms, child).root;
  }
  const auto id = static_cast<std::uint32_t>(blocks.rows.size());
  blocks.rows.push_back({id, model, std::move(hash), std::move(canonical), std::move(children)});
  return id;
}
void project_measurements(react::AssetMeasurements &measurements, ModelId model,
                          std::uint32_t params, const Asset &asset,
                          const MeasurementBindings &bindings) {
  react::AssetMeasurements::Row row;
  row.volume = asset.volume;
  if (bindings.volume)
    row.values[bindings.volume] = Value(asset.volume);
  if (bindings.capacity)
    row.values[bindings.capacity] = Value(asset.capacity);
  if (params)
    measurements.parameter_rows[params] = std::move(row);
  else
    measurements.rows[model] = std::move(row);
}
void inherit_definition_settings(react::ModelSettings &settings, const ModelUpdate &update) {
  for (const auto &[model, original] : update.inherited_settings)
    if (const auto found = settings.rows.find(original); found != settings.rows.end())
      settings.rows.emplace(model, found->second);
}
void project_definition_measurements(react::AssetMeasurements &measurements, const Models &models,
                                     const ParameterBlocks &parameters, const Assets &assets,
                                     const ModelUpdate &update, MeasurementBindings roles) {
  for (auto id : update.models)
    project_measurements(measurements, id, 0, assets.rows.at(models.rows[id].asset), roles);
  if (update.models.empty())
    return;
  for (const auto &variant : update.parameters)
    if (variant.after)
      project_measurements(measurements, update.model, variant.after,
                           assets.rows.at(parameters.rows[variant.after].asset), roles);
}
} // namespace amber::catalog
