#include "catalog.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <picosha2.h>
#include <stdexcept>

namespace amber::catalog {
TextId append_authored_text(AuthoredTexts &texts, Handle target, PlayerId author, double created_s,
                            AuthoredTextKind kind, std::string text) {
  const auto id = static_cast<TextId>(texts.rows.size());
  texts.rows.push_back({id, target, author, created_s, kind, std::move(text)});
  if (kind == AuthoredTextKind::Detail)
    texts.details_by_target[target.bits].push_back(id);
  return id;
}
void rebuild_text_index(AuthoredTexts &texts) {
  texts.details_by_target.clear();
  for (const auto &row : texts.rows)
    if (row.id && row.kind == AuthoredTextKind::Detail)
      texts.details_by_target[row.target.bits].push_back(row.id);
}
namespace {
void regular(const std::filesystem::path &path) {
  if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)))
    throw std::runtime_error("artifact is not a regular file: " + path.filename().string());
}
std::string bytes(const std::filesystem::path &path) {
  if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)))
    throw std::runtime_error("artifact is not a regular file: " + path.filename().string());
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    throw std::runtime_error("artifact cannot be opened");
  return {std::istreambuf_iterator<char>(stream), {}};
}
Vec3 vector3(const nlohmann::json &value) {
  if (!value.is_array() || value.size() != 3)
    throw std::runtime_error("expected a three-component vector");
  Vec3 result{value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
  if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z))
    throw std::runtime_error("vector must be finite");
  return result;
}
void hash_id(std::string_view hash) {
  if (!valid_hash(hash))
    throw std::runtime_error("invalid artifact hash");
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
Hash sha256(std::string_view text) {
  return picosha2::hash256_hex_string(text.begin(), text.end());
}
bool valid_hash(std::string_view hash) {
  return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
PreparedAsset read_asset(const std::filesystem::path &root, Hash expected, std::string canonical,
                         const RuntimePins &pins) {
  hash_id(expected);
  const auto directory = root / expected;
  PreparedAsset prepared;
  prepared.source = bytes(directory / "source.py");
  auto &asset = prepared.asset;
  asset.hash = expected;
  asset.source_hash = sha256(prepared.source);
  asset.canonical_params = std::move(canonical);
  asset.manifest = nlohmann::json::parse(bytes(directory / "manifest.json"));
  const auto &m = asset.manifest;
  if (m.at("version") != 1 || m.at("hash") != expected || m.at("bake_version") != pins.bake ||
      m.at("coordinates") != "z_up_meters")
    throw std::runtime_error("asset identity/version mismatch");
  auto params = nlohmann::json::parse(asset.canonical_params);
  if (!params.is_object() || params != m.at("params"))
    throw std::runtime_error("canonical parameter values do not match manifest");
  const auto &schema = m.at("parameter_schema");
  if (!schema.is_object() || schema.size() != params.size())
    throw std::runtime_error("parameter schema mismatch");
  for (const auto &[name, value] : params.items()) {
    if (!value.is_number() || !std::isfinite(value.get<double>()))
      throw std::runtime_error("parameter must be finite numeric");
    const auto &limits = schema.at(name);
    if (!limits.is_array() || limits.size() != 3)
      throw std::runtime_error("parameter limits malformed");
    double def = limits[0].get<double>(), lo = limits[1].get<double>(),
           hi = limits[2].get<double>();
    if (!std::isfinite(def) || !std::isfinite(lo) || !std::isfinite(hi) || lo > def || def > hi ||
        value.get<double>() < lo || value.get<double>() > hi)
      throw std::runtime_error("parameter outside schema bounds");
  }
  if (m.contains("parent_asset")) {
    asset.parent_asset = m.at("parent_asset").get<std::string>();
    asset.part_name = m.at("part_name").get<std::string>();
    hash_id(asset.parent_asset);
    if (asset.part_name.empty() ||
        asset.part_name.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") !=
            std::string::npos)
      throw std::runtime_error("invalid part name");
    std::string identity = asset.parent_asset;
    identity.push_back('\0');
    identity += "part";
    identity.push_back('\0');
    identity += asset.part_name;
    if (sha256(identity) != expected)
      throw std::runtime_error("part identity mismatch");
  } else {
    std::string identity = prepared.source;
    identity.push_back('\0');
    identity += asset.canonical_params;
    identity.push_back('\0');
    identity += pins.bake;
    if (sha256(identity) != expected)
      throw std::runtime_error("source/parameter identity mismatch");
  }
  asset.minimum = vector3(m.at("bounds").at("min"));
  asset.maximum = vector3(m.at("bounds").at("max"));
  if (asset.minimum.x > asset.maximum.x || asset.minimum.y > asset.maximum.y ||
      asset.minimum.z > asset.maximum.z)
    throw std::runtime_error("asset bounds reversed");
  asset.volume = m.at("volume").get<double>();
  asset.capacity = m.at("capacity").get<double>();
  if (!std::isfinite(asset.volume) || !std::isfinite(asset.capacity) || asset.volume < 0 ||
      asset.capacity < 0)
    throw std::runtime_error("asset measurements must be finite and nonnegative");
  std::set<std::string> lods;
  for (const auto &lod : m.at("lods")) {
    const auto file = lod.at("file").get<std::string>();
    if (file != "lod0.glb" && file != "lod1.glb" && file != "lod2.glb")
      throw std::runtime_error("unexpected LOD artifact");
    if (!lods.insert(file).second)
      throw std::runtime_error("duplicate LOD artifact");
    regular(directory / file);
  }
  if (lods.size() != 3)
    throw std::runtime_error("three LOD artifacts are required");
  if (m.at("collision").at("jolt") != pins.jolt)
    throw std::runtime_error("collision runtime version mismatch");
  for (const auto &[role, file] : std::array<std::pair<const char *, const char *>, 3>{
           {{"static", "collision.bin"}, {"dynamic", "convex.bin"}, {"mesh", "mesh.bin"}}}) {
    const auto &value = m.at("collision").at(role);
    if (!value.is_null()) {
      if (value != file)
        throw std::runtime_error("unexpected collision artifact");
      regular(directory / file);
    } else if (asset.volume > 0)
      throw std::runtime_error("solid geometry is missing collision artifact");
  }
  for (const auto &[name, part] : m.at("parts").items()) {
    if (name.empty() || name.find_first_not_of(
                            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") !=
                            std::string::npos)
      throw std::runtime_error("invalid child part name");
    hash_id(part.at("asset").get<std::string>());
    (void)vector3(part.at("position"));
  }
  return prepared;
}
void resolve_manifest_terms(PreparedAsset &prepared, const react::Terms &terms) {
  auto &asset = prepared.asset;
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
void import_asset(Assets &assets, PreparedAsset prepared) {
  if (auto found = assets.rows.find(prepared.asset.hash); found != assets.rows.end()) {
    if (found->second.source_hash != prepared.asset.source_hash ||
        found->second.canonical_params != prepared.asset.canonical_params)
      throw std::runtime_error("immutable asset collision");
    return;
  }
  assets.rows.emplace(prepared.asset.hash, std::move(prepared.asset));
}
ScriptId import_script(Scripts &scripts, PreparedScript prepared, PlayerId author,
                       double created_s) {
  const auto id = static_cast<ScriptId>(scripts.rows.size());
  prepared.declaration.id = id;
  prepared.declaration.author = author;
  scripts.rows.push_back({id, std::move(prepared.hash), std::move(prepared.source_hash),
                          std::move(prepared.bytecode_hash), std::move(prepared.registry_pin),
                          std::move(prepared.pins), author, created_s,
                          std::move(prepared.declaration),
                          prepared.metadata.value("tests", nlohmann::json::array())});
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
    if (script && scripts.rows[script].declaration.kind == react::ScriptDeclaration::Kind::Rule)
      throw std::runtime_error("a world rule is not a model script");
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
std::uint32_t add_variant(ParameterBlocks &blocks, Models &models, ModelTexts &texts,
                          react::ModelSettings &settings, const Assets &assets,
                          const Scripts &scripts, const react::Terms &terms, ModelId model,
                          Hash hash, std::string canonical) {
  const auto source = models.rows.at(model);
  const auto &asset = assets.rows.at(hash);
  if (!model || source.id != model || source.source_hash != asset.source_hash ||
      source.part_name != asset.part_name || canonical != asset.canonical_params)
    throw std::runtime_error("variant does not belong to model source");
  if (auto found = blocks.by_model_asset.find({model, hash}); found != blocks.by_model_asset.end())
    return found->second;
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
  blocks.by_model_asset[{model, blocks.rows.back().asset}] = id;
  return id;
}
const std::map<std::string, ModelId> &part_models(const Models &models,
                                                  const ParameterBlocks &blocks, ModelId model,
                                                  std::uint32_t params) {
  (void)resolve_asset(models, blocks, model, params);
  return params ? blocks.rows[params].parts : models.rows[model].parts;
}
EffectiveAsset resolve_asset(const Models &models, const ParameterBlocks &blocks, ModelId model,
                             std::uint32_t params) {
  if (!model || model >= models.rows.size() || models.rows[model].id != model)
    throw std::runtime_error("unknown model");
  if (!params)
    return {models.rows[model].asset, 0};
  if (params >= blocks.rows.size() || blocks.rows[params].id != params ||
      blocks.rows[params].model != model)
    throw std::runtime_error("parameter block belongs to another model");
  return {blocks.rows[params].asset, params};
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
void rebuild_parameter_index(ParameterBlocks &blocks) {
  blocks.by_model_asset.clear();
  for (const auto &block : blocks.rows)
    if (block.id)
      blocks.by_model_asset[{block.model, block.asset}] = block.id;
}
} // namespace amber::catalog
