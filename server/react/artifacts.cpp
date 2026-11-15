#include "react.hpp"

#include <fstream>
#include <stdexcept>

namespace amber::catalog {
namespace {
std::string read(const std::filesystem::path &path) {
  if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)))
    throw std::runtime_error("script artifact must be a regular file");
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("script artifact missing");
  return {std::istreambuf_iterator<char>(file), {}};
}
void write(const std::filesystem::path &path, const std::string &text) {
  std::ofstream file(path, std::ios::binary);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!file)
    throw std::runtime_error("script artifact write failed");
}
Hash identity(std::string_view source, const Hash &registry, const RuntimePins &pins) {
  std::string bytes(source);
  bytes.push_back('\0');
  bytes += registry;
  bytes.push_back('\0');
  bytes += pins.luau;
  bytes.push_back('\0');
  bytes += pins.host_abi;
  return sha256(bytes);
}
} // namespace
PreparedScript prepare_script(std::string_view source, const react::Terms &terms,
                              const react::RightsDeclaration &rights, Hash registry_pin,
                              const RuntimePins &pins, react::VmSettings limits) {
  if (!valid_hash(registry_pin) || pins.luau != "0.747" || pins.host_abi != "ambergris-host-1")
    throw std::runtime_error("script validator pin mismatch");
  auto analysis = react::analyze_script(source, terms, rights, limits);
  if (!analysis.errors.empty()) {
    std::string errors;
    for (const auto &error : analysis.errors) {
      if (!errors.empty())
        errors += '\n';
      errors += error;
    }
    throw std::runtime_error(errors);
  }
  PreparedScript result;
  result.source = source;
  result.source_hash = sha256(source);
  result.bytecode_hash = sha256(analysis.compiled.bytecode);
  result.registry_pin = std::move(registry_pin);
  result.pins = pins;
  result.hash = identity(source, result.registry_pin, pins);
  result.compiled = std::move(analysis.compiled);
  result.declaration = std::move(analysis.declaration);
  result.metadata = std::move(analysis.metadata);
  return result;
}
void write_script(const std::filesystem::path &directory, const PreparedScript &script) {
  std::filesystem::create_directories(directory);
  const nlohmann::json manifest = {
      {"version", 1},
      {"hash", script.hash},
      {"source_hash", script.source_hash},
      {"bytecode_hash", script.bytecode_hash},
      {"registry_pin", script.registry_pin},
      {"pins", {{"luau", script.pins.luau}, {"host_abi", script.pins.host_abi}}},
      {"declaration", declaration_json(script.declaration)},
      {"metadata", script.metadata}};
  write(directory / "source.luau", script.source);
  write(directory / "bytecode.luau", script.compiled.bytecode);
  write(directory / "script.json", manifest.dump(2));
}
PreparedScript read_script(const std::filesystem::path &root, Hash expected,
                           const RuntimePins &pins, Hash registry_pin, const react::Terms &terms,
                           const react::RightsDeclaration &rights) {
  if (!valid_hash(expected))
    throw std::runtime_error("invalid script hash");
  const auto directory = root / expected;
  auto data = nlohmann::json::parse(read(directory / "script.json"));
  PreparedScript script;
  script.hash = expected;
  script.source = read(directory / "source.luau");
  script.compiled.bytecode = read(directory / "bytecode.luau");
  script.source_hash = sha256(script.source);
  script.bytecode_hash = sha256(script.compiled.bytecode);
  script.registry_pin = registry_pin;
  script.pins = pins;
  if (data.at("version") != 1 || data.at("hash") != expected ||
      data.at("source_hash") != script.source_hash ||
      data.at("bytecode_hash") != script.bytecode_hash || data.at("registry_pin") != registry_pin ||
      data.at("pins").at("luau") != pins.luau || data.at("pins").at("host_abi") != pins.host_abi ||
      identity(script.source, registry_pin, pins) != expected)
    throw std::runtime_error("script artifact digest/runtime pin mismatch");
  script.declaration = declaration_from_json(data.at("declaration"));
  script.metadata = data.at("metadata");
  script.declaration.approved = false;
  script.declaration.active = script.declaration.kind != react::ScriptDeclaration::Kind::Rule;
  auto validation = react::validate_script(script.source, terms, script.declaration, rights);
  if (!validation)
    throw std::runtime_error(validation.errors.front());
  return script;
}
void restore_scripts(Scripts &scripts, react::Vm &vm, react::RuleVersions &rules,
                     const RegistryDocument &registry, const react::Terms &terms,
                     const RuntimePins &pins, const std::filesystem::path &source_root,
                     const std::filesystem::path &destination_root) {
  load_rules(registry, scripts, rules);
  const react::RightsDeclaration rights{
      {"use", "take", "move", "edit", "copy", "transfer", "destroy"},
      {"spend", "build", "enter", "run_scripts", "animate", "camera", "issue"}};
  for (auto &script : scripts.rows) {
    if (!script.id)
      continue;
    auto prepared =
        read_script(source_root, script.artifact, pins, script.registry_pin, terms, rights);
    const auto loaded = react::load_script(vm, prepared.compiled, script.declaration);
    if (loaded.status != react::RunOutcome::Status::Ok)
      throw std::runtime_error(loaded.error);
    script.source = prepared.source;
    write_script(destination_root / prepared.hash, prepared);
  }
}
void restore_assets(Assets &assets, const Models &models, const ParameterBlocks &parameters,
                    const react::Terms &terms, const RuntimePins &pins,
                    const std::filesystem::path &root) {
  const auto load = [&](const Hash &hash, const std::string &canonical) {
    if (assets.rows.contains(hash))
      return;
    auto prepared = read_asset(root, hash, canonical, pins);
    resolve_manifest_terms(prepared, terms);
    import_asset(assets, std::move(prepared));
  };
  for (const auto &model : models.rows)
    if (model.id)
      load(model.asset, model.canonical_params);
  for (const auto &block : parameters.rows)
    if (block.id)
      load(block.asset, block.canonical_params);
}
} // namespace amber::catalog
