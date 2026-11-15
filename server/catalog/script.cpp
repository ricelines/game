#include "catalog.hpp"

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
} // namespace amber::catalog
