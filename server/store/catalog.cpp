#include "store.hpp"
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
  return id;
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
} // namespace
Hash sha256(std::string_view text) {
  return picosha2::hash256_hex_string(text.begin(), text.end());
}
bool valid_hash(std::string_view hash) {
  return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
Asset read_asset(const std::filesystem::path &root, Hash expected, std::string canonical,
                 const RuntimePins &pins) {
  hash_id(expected);
  const auto directory = root / expected;
  Asset asset;
  asset.source = bytes(directory / "source.py");
  asset.hash = expected;
  asset.source_hash = sha256(asset.source);
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
    std::string identity = asset.source;
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
  return asset;
}
void import_asset(Assets &assets, Asset asset) {
  if (auto found = assets.rows.find(asset.hash); found != assets.rows.end()) {
    if (found->second.source_hash != asset.source_hash ||
        found->second.canonical_params != asset.canonical_params)
      throw std::runtime_error("immutable asset collision");
    return;
  }
  assets.rows.emplace(asset.hash, std::move(asset));
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
} // namespace amber::catalog
