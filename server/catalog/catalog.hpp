#pragma once

#include "react/react.hpp"

#include <filesystem>
#include <set>

namespace amber::catalog {
using Hash = std::string;
struct RuntimePins {
  std::string bake, luau = "0.747", host_abi = "ambergris-host-1", jolt = "5.6.0";
};
struct Text {
  std::string name, description, writing;
  bool operator==(const Text &) const = default;
};
struct ModelTexts {
  std::unordered_map<ModelId, Text> rows;
};
struct InstanceTexts {
  std::unordered_map<std::uint64_t, Text> rows;
};
using TextId = std::uint32_t;
enum class AuthoredTextKind { Detail, Refusal };
struct AuthoredText {
  TextId id = 0;
  Handle target;
  PlayerId author = 0;
  double created_s = 0;
  AuthoredTextKind kind = AuthoredTextKind::Detail;
  std::string text;
};
struct AuthoredTexts {
  std::vector<AuthoredText> rows{AuthoredText{}};
  std::unordered_map<std::uint64_t, std::vector<TextId>> details_by_target;
};
TextId append_authored_text(AuthoredTexts &, Handle, PlayerId, double created_s, AuthoredTextKind,
                            std::string);
void rebuild_text_index(AuthoredTexts &);
struct Asset {
  Hash hash, source_hash;
  std::string canonical_params, part_name;
  Hash parent_asset;
  Vec3 minimum, maximum;
  double volume = 0, capacity = 0;
  std::vector<TermId> replicated;
  nlohmann::json manifest;
};
struct PreparedAsset {
  Asset asset;
  std::string source;
};
struct Assets {
  std::unordered_map<Hash, Asset> rows;
};
struct Model {
  ModelId id = 0;
  Hash asset, source_hash;
  std::string part_name, canonical_params;
  TermId material = 0;
  PlayerId author = 0;
  double created_s = 0;
  ScriptId script = 0;
  std::map<std::string, ModelId> parts;
};
struct Models {
  std::vector<Model> rows{Model{}};
};
struct ParameterBlock {
  std::uint32_t id = 0;
  ModelId model = 0;
  Hash asset;
  std::string canonical_params;
  std::map<std::string, ModelId> parts;
};
struct ParameterBlocks {
  std::vector<ParameterBlock> rows{ParameterBlock{}};
};
struct PreparedScript {
  Hash hash, source_hash, bytecode_hash, registry_pin;
  RuntimePins pins;
  std::string source;
  react::CompiledScript compiled;
  react::ScriptDeclaration declaration;
  nlohmann::json metadata;
};
struct Script {
  ScriptId id = 0;
  Hash artifact, source_hash, bytecode_hash, registry_pin;
  RuntimePins pins;
  PlayerId author = 0;
  double created_s = 0;
  react::ScriptDeclaration declaration;
  nlohmann::json tests = nlohmann::json::array();
};
struct Scripts {
  std::vector<Script> rows{Script{}};
};
struct RegistryDocument {
  Hash hash;
  nlohmann::json data;
};
struct MechanicalBindings {
  nlohmann::json values = nlohmann::json::object();
};
struct MeasurementBindings {
  TermId volume = 0, capacity = 0;
};
struct ModelSubmission {
  Hash asset;
  TermId material = 0;
  PlayerId author = 0;
  double created_s = 0;
  ScriptId script = 0;
  Text text;
  react::Values settings;
};
struct ModelRegistration {
  ModelId root = 0;
  std::map<std::string, ModelId> parts;
};
struct EffectiveAsset {
  Hash hash;
  std::uint32_t params = 0;
};
struct TermProposal {
  react::Term term;
};
struct DuplicateChoice {
  TermId same_as = 0;
  std::string none_fits;
};
Hash sha256(std::string_view);
bool valid_hash(std::string_view);
PreparedAsset read_asset(const std::filesystem::path &root, Hash expected,
                         std::string canonical_params, const RuntimePins &);
void resolve_manifest_terms(PreparedAsset &, const react::Terms &);
PreparedScript prepare_script(std::string_view source, const react::Terms &,
                              const react::RightsDeclaration &, Hash registry_pin,
                              const RuntimePins &, react::VmSettings);
void write_script(const std::filesystem::path &directory, const PreparedScript &);
PreparedScript read_script(const std::filesystem::path &root, Hash expected, const RuntimePins &,
                           Hash registry_pin, const react::Terms &,
                           const react::RightsDeclaration &);
void import_asset(Assets &, PreparedAsset);
ScriptId import_script(Scripts &, PreparedScript, PlayerId author, double created_s);
ModelRegistration submit_model(Models &, ModelTexts &, react::ModelSettings &, const Assets &,
                               const Scripts &, const react::Terms &, const ModelSubmission &);
std::uint32_t add_variant(ParameterBlocks &, Models &, ModelTexts &, react::ModelSettings &,
                          const Assets &, const Scripts &, const react::Terms &, ModelId,
                          Hash asset, std::string canonical_params);
const std::map<std::string, ModelId> &part_models(const Models &, const ParameterBlocks &, ModelId,
                                                  std::uint32_t params);
EffectiveAsset resolve_asset(const Models &, const ParameterBlocks &, ModelId, std::uint32_t);
void project_measurements(react::AssetMeasurements &, ModelId, std::uint32_t, const Asset &,
                          const MeasurementBindings &);
void project_settings(react::ModelSettings &, const react::Terms &, ModelId, const react::Values &);
TermId append_term(react::Terms &, const TermProposal &, const DuplicateChoice &);
void set_material(react::Materials &, const react::Terms &, TermId, const react::Values &);
Hash save_registry(const std::filesystem::path &, const react::Terms &, const react::Materials &,
                   const react::RuleVersions &, const MechanicalBindings &);
RegistryDocument read_registry(const std::filesystem::path &, Hash);
void load_terms(const RegistryDocument &, react::Terms &);
void load_materials(const RegistryDocument &, const react::Terms &, react::Materials &);
void load_rules(const RegistryDocument &, const Scripts &, react::RuleVersions &);
nlohmann::json term_json(const react::Term &);
react::Term term_from_json(const nlohmann::json &);
nlohmann::json declaration_json(const react::ScriptDeclaration &);
react::ScriptDeclaration declaration_from_json(const nlohmann::json &);
} // namespace amber::catalog
