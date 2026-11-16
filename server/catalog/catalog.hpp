#pragma once

#include "react/react.hpp"
#include "snap/snap.hpp"
#include "store/store.hpp"

#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <unordered_map>

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
};
TextId append_authored_text(AuthoredTexts &, Handle, PlayerId, double created_s, AuthoredTextKind,
                            std::string);
struct Asset {
  Hash hash, source_hash;
  std::string canonical_params, part_name;
  Hash parent_asset;
  Vec3 minimum, maximum;
  double volume = 0, capacity = 0;
  std::vector<TermId> replicated;
  nlohmann::json manifest;
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
  GroupId group = 0;
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
struct ModelRegistration {
  ModelId root = 0;
  std::map<std::string, ModelId> parts;
};
struct EffectiveAsset {
  Hash hash;
  std::uint32_t params = 0;
};
struct DuplicateChoice {
  TermId same_as = 0;
  std::string none_fits;
};
Hash sha256(std::string_view);
bool valid_hash(std::string_view);
Asset read_asset(const std::filesystem::path &root, Hash expected, std::string canonical_params,
                 const RuntimePins &);
void import_asset(Assets &, Asset);
const std::map<std::string, ModelId> &part_models(const Models &, const ParameterBlocks &, ModelId,
                                                  std::uint32_t params);
EffectiveAsset resolve_asset(const Models &, const ParameterBlocks &, ModelId, std::uint32_t);
struct RevisedParameter {
  std::uint32_t before = 0, after = 0;
};
struct ModelUpdate {
  ModelId model = 0;
  bool changed = false;
  std::vector<ModelId> models;
  std::vector<RevisedParameter> parameters;
  std::vector<std::pair<ModelId, ModelId>> inherited_settings;
};
// Resolve every registered parameter set against the proposal's baked sets.
// The default adopts the new defaults; explicit values survive on retained keys.
std::vector<RevisedParameter> definition_parameters(const Models &, const ParameterBlocks &,
                                                    const Assets &, ModelId model,
                                                    ModelId proposal);
// A rare definition edit on the single writer. Parameter block identities stay
// usable and receive the new geometry; no old definition can be reconfigured in.
ModelUpdate update_definition(Models &, ParameterBlocks &, ModelTexts &, const Assets &,
                              ModelId model, ModelId proposal);

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
  std::string source;
};
struct Scripts {
  std::vector<Script> rows{Script{}};
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
struct TermProposal {
  react::Term term;
};

void resolve_manifest_terms(Asset &, const react::Terms &);
PreparedScript prepare_script(std::string_view source, const react::Terms &,
                              const react::RightsDeclaration &, Hash registry_pin,
                              const RuntimePins &, react::VmSettings);
void write_script(const std::filesystem::path &directory, const PreparedScript &);
PreparedScript read_script(const std::filesystem::path &root, Hash expected, const RuntimePins &,
                           Hash registry_pin, const react::Terms &,
                           const react::RightsDeclaration &);
ScriptId import_script(Scripts &, PreparedScript, PlayerId author, double created_s);
void restore_scripts(Scripts &, react::Vm &, react::RuleVersions &, const RegistryDocument &,
                     const react::Terms &, const RuntimePins &,
                     const std::filesystem::path &source_root,
                     const std::filesystem::path &destination_root);
void restore_assets(Assets &, const Models &, const ParameterBlocks &, const react::Terms &,
                    const RuntimePins &, const std::filesystem::path &asset_root);
ModelRegistration submit_model(Models &, ModelTexts &, react::ModelSettings &, const Assets &,
                               const Scripts &, const react::Terms &, const ModelSubmission &);
std::uint32_t add_variant(State &family, const State &catalog_state, react::ModelSettings &,
                          const react::Terms &, ModelId, Hash asset, std::string canonical_params);
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
void inherit_definition_settings(react::ModelSettings &, const ModelUpdate &);
void project_definition_measurements(react::AssetMeasurements &, const Models &,
                                     const ParameterBlocks &, const Assets &, const ModelUpdate &,
                                     MeasurementBindings);

using ScriptSelection = std::optional<std::span<const ScriptId>>;
void write_scripts(snap::Output &, const Scripts &, ScriptSelection = {});
void read_scripts(snap::Input &, Scripts &);

struct State {
  Assets assets;
  Models models;
  Scripts scripts;
  ParameterBlocks parameters;
  ModelTexts model_text;
  InstanceTexts instance_text;
  AuthoredTexts authored_text;
  react::AssetMeasurements baked_measurements;
  MechanicalBindings mechanical;
  RegistryDocument registry;
};

// nullopt means full snapshot; an explicitly empty span means empty capture.
using ModelSelection = std::optional<std::span<const ModelId>>;
using ParameterSelection = std::optional<std::span<const std::uint32_t>>;
using ObjectSelection = std::optional<std::span<const Handle>>;
void write_models(snap::Output &, const Models &, ModelSelection = {});
void read_models(snap::Input &, Models &);
void write_parameters(snap::Output &, const ParameterBlocks &, ParameterSelection = {});
void read_parameters(snap::Input &, ParameterBlocks &);
void write_text(snap::Output &, const ModelTexts &, const InstanceTexts &, ModelSelection = {},
                ObjectSelection = {});
void read_text(snap::Input &, ModelTexts &, InstanceTexts &);
void write_authored_texts(snap::Output &, const AuthoredTexts &, ObjectSelection = {});
void read_authored_texts(snap::Input &, AuthoredTexts &);
} // namespace amber::catalog

namespace amber::lifecycle {
AssemblyTemplate expand(const catalog::Models &, const catalog::ParameterBlocks &,
                        const catalog::Assets &, ModelId, std::uint32_t params = 0);
Replacement plan_bindings(const catalog::Models &, const Parts &, IdentityRead,
                          const std::vector<std::uint32_t> &params, Handle,
                          const AssemblyTemplate &, std::optional<TermId> material = {});
Replacement plan_replace(const catalog::Models &, const Parts &, IdentityRead, TransformRead,
                         const std::vector<std::uint32_t> &params, Handle, const AssemblyTemplate &,
                         std::optional<TermId> material = {});
} // namespace amber::lifecycle

namespace amber::catalog {
TermId declare_native(react::NativeActions &, react::Terms &, std::string name,
                      std::string description, std::string right, bool reach,
                      nlohmann::json inputs_schema, react::NativeOperation);
}
