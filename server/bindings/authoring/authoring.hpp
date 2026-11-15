#pragma once

#include "bindings/viewing/sources.hpp"
#include "command/command.hpp"

#include <memory>
#include <span>
#include <variant>

namespace amber::drive {
struct Input;
struct InputOperations;
} // namespace amber::drive
namespace amber::record {
class Journal;
}

namespace amber::bindings::authoring {
using Json = nlohmann::json;
struct AssetRequest {
  catalog::Hash asset;
  std::string canonical_params;
};
struct ModelRequest {
  catalog::Hash asset;
  std::string canonical_params;
  TermId material = 0;
  catalog::Text text;
  std::vector<AssetRequest> variants;
};
struct ScriptRequest {
  std::string source;
};
struct VariantRequest {
  ModelId model = 0;
  catalog::Hash asset;
  std::string canonical_params;
};
ModelRequest model_request(const Json &, const react::Terms &);
VariantRequest variant_request(const Json &);
ScriptRequest script_request(const Json &);
struct RegistrySnapshot {
  react::Terms terms;
  catalog::Hash pin;
};
struct WorkerSettings {
  std::filesystem::path asset_root;
  catalog::RuntimePins pins;
  react::VmSettings limits;
};
struct PreparedModel {
  std::vector<catalog::PreparedAsset> assets; // root first, then its real declared parts
  TermId material = 0;
  catalog::Text text;
  std::vector<std::vector<catalog::PreparedAsset>> variants;
};
struct PreparedVariant {
  ModelId model = 0;
  std::vector<catalog::PreparedAsset> assets;
};
struct Failure {
  std::string error;
};
struct Completion {
  std::uint64_t input_id = 0;
  std::variant<PreparedModel, PreparedVariant, catalog::PreparedScript, Failure> result;
};
class Worker {
public:
  explicit Worker(WorkerSettings);
  ~Worker();
  Worker(const Worker &) = delete;
  Worker &operator=(const Worker &) = delete;
  void submit(std::uint64_t input_id, ModelRequest, RegistrySnapshot);
  void submit(std::uint64_t input_id, VariantRequest, RegistrySnapshot);
  void submit(std::uint64_t input_id, ScriptRequest, RegistrySnapshot);
  std::vector<Completion> poll();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
struct SubmissionQueue {
  Worker &worker;
  const react::Terms &terms;
  const catalog::RegistryDocument &registry;
};
void register_inputs(drive::InputOperations &, SubmissionQueue &);
struct GeometryHook {
  void *local = nullptr;
  void (*prepare)(void *, const catalog::Asset &) = nullptr;
};
struct CatalogInstall {
  catalog::Assets &assets;
  catalog::Models &models;
  catalog::ParameterBlocks &parameters;
  catalog::ModelTexts &texts;
  react::ModelSettings &settings;
  react::AssetMeasurements &measurements;
  viewing::PublicSources &sources;
  const catalog::Scripts &scripts;
  const react::Terms &terms;
  catalog::MeasurementBindings measured_terms;
  GeometryHook geometry;
};
struct ScriptInstall {
  catalog::Scripts &scripts;
  react::Vm &vm;
  viewing::PublicSources &sources;
  const command::NativeActions &native;
  std::filesystem::path script_root;
};
// Called only by the simulation writer, with identity from the accepted Input.
Json install_model(PreparedModel, PlayerId author, double created_s, CatalogInstall &);
Json install_variant(PreparedVariant, CatalogInstall &);
Json install_script(catalog::PreparedScript, PlayerId author, double created_s, ScriptInstall &);
struct AppliedCompletion {
  Json reply;
  std::vector<ModelId> added_model_ids;
};
AppliedCompletion complete_authoring(const drive::Input &, Completion, bool session_live,
                                     CatalogInstall &, ScriptInstall &, record::Journal &, Step,
                                     double hz, std::span<const PlayerId> seed_authors);
Json failure(std::string);
} // namespace amber::bindings::authoring
