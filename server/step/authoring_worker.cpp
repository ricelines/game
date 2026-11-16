#include "catalog/catalog.hpp"
#include "step.hpp"

#include <charconv>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

namespace amber::step::authoring {
namespace {
AssetRequest asset_request(const Json &row) {
  AssetRequest request;
  request.asset = row.at("asset").get<std::string>();
  if (!catalog::valid_hash(request.asset))
    throw std::invalid_argument("invalid asset hash");
  request.canonical_params = row.at("canonical_params").get<std::string>();
  if (!Json::parse(request.canonical_params).is_object())
    throw std::invalid_argument("canonical_params must encode an object");
  return request;
}
} // namespace
ModelRequest model_request(const Json &row, const react::Terms &terms) {
  const auto asset = asset_request(row);
  ModelRequest request;
  request.asset = asset.asset;
  request.canonical_params = asset.canonical_params;
  const auto material = row.at("material").get<std::string>();
  const auto found = terms.by_name.find(material);
  if (found == terms.by_name.end() || terms.rows.at(found->second).kind != react::Kind::Material)
    throw std::invalid_argument("unknown material term");
  request.material = found->second;
  const auto &text = row.at("text");
  if (!text.is_object())
    throw std::invalid_argument("text must be an object");
  for (const auto &[name, value] : text.items())
    if ((name != "name" && name != "description" && name != "writing") || !value.is_string())
      throw std::invalid_argument("text accepts only name, description and writing strings");
  request.text = {text.at("name").get<std::string>(), text.at("description").get<std::string>(),
                  text.value("writing", std::string{})};
  if (row.contains("variants")) {
    if (!row["variants"].is_array())
      throw std::invalid_argument("variants must be an array");
    for (const auto &variant : row["variants"])
      request.variants.push_back(asset_request(variant));
  }
  // The transported manifest is intentionally ignored. Verified disk artifacts
  // determine geometry, measurements, bindings, and declared children.
  return request;
}
ScriptRequest script_request(const Json &row) {
  return {row.at("source").get<std::string>()};
}
VariantRequest variant_request(const Json &row) {
  VariantRequest request;
  const auto text = row.at("model").get<std::string>();
  if (!text.starts_with("m:"))
    throw std::invalid_argument("variant model must name m:N");
  const auto parsed = std::from_chars(text.data() + 2, text.data() + text.size(), request.model);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !request.model)
    throw std::invalid_argument("variant model must name m:N");
  const auto asset = asset_request(row);
  request.asset = asset.asset;
  request.canonical_params = asset.canonical_params;
  return request;
}
namespace {
std::vector<catalog::Asset> prepare_assets(const catalog::Hash &root, const std::string &canonical,
                                           const RegistrySnapshot &registry,
                                           const WorkerSettings &settings) {
  std::vector<catalog::Asset> result;
  std::set<catalog::Hash> loaded;
  auto load = [&](const catalog::Hash &hash, const std::string &canonical) {
    if (!loaded.insert(hash).second)
      return;
    auto asset = catalog::read_asset(settings.asset_root, hash, canonical, settings.pins);
    catalog::resolve_manifest_terms(asset, registry.terms);
    result.push_back(std::move(asset));
  };
  load(root, canonical);
  if (!result.front().parent_asset.empty())
    throw std::invalid_argument("submit an assembly root, not a detached declared part");
  for (std::size_t index = 0; index < result.size(); ++index) {
    const auto parts = result[index].manifest.at("parts");
    const auto parameters = result[index].canonical_params;
    for (const auto &[name, child] : parts.items()) {
      (void)name;
      load(child.at("asset").get<std::string>(), parameters);
    }
  }
  return result;
}
PreparedModel prepare_model(ModelRequest request, const RegistrySnapshot &registry,
                            const WorkerSettings &settings) {
  if (!request.material || request.material >= registry.terms.rows.size() ||
      registry.terms.rows[request.material].kind != react::Kind::Material)
    throw std::invalid_argument("unknown material term");
  PreparedModel prepared{
      prepare_assets(request.asset, request.canonical_params, registry, settings),
      request.material,
      std::move(request.text),
      {}};
  std::set<std::string> parameters{prepared.assets.front().canonical_params};
  for (const auto &variant : request.variants) {
    auto assets = prepare_assets(variant.asset, variant.canonical_params, registry, settings);
    if (assets.front().source_hash != prepared.assets.front().source_hash)
      throw std::invalid_argument("staged variant does not share the submitted model source");
    if (!parameters.insert(assets.front().canonical_params).second)
      throw std::invalid_argument("staged model repeats a canonical parameter set");
    prepared.variants.push_back(std::move(assets));
  }
  return prepared;
}
PreparedVariant prepare_variant(VariantRequest request, const RegistrySnapshot &registry,
                                const WorkerSettings &settings) {
  auto assets = prepare_assets(request.asset, request.canonical_params, registry, settings);
  return {request.model, std::move(assets)};
}
} // namespace
struct Worker::Impl {
  struct Job {
    std::uint64_t input_id;
    std::variant<ModelRequest, VariantRequest, ScriptRequest> request;
    RegistrySnapshot registry;
  };
  WorkerSettings settings;
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<Job> pending;
  std::vector<Completion> completed;
  bool stopping = false;
  std::thread thread;

  explicit Impl(WorkerSettings value) : settings(std::move(value)), thread([this] { run(); }) {}
  ~Impl() {
    {
      std::lock_guard lock(mutex);
      stopping = true;
      pending.clear();
    }
    ready.notify_one();
    thread.join();
  }
  void run() {
    const react::RightsDeclaration rights{
        {"use", "take", "move", "edit", "copy", "transfer", "destroy"},
        {"spend", "build", "enter", "run_scripts", "animate", "camera"}};
    while (true) {
      Job job;
      {
        std::unique_lock lock(mutex);
        ready.wait(lock, [&] { return stopping || !pending.empty(); });
        if (stopping)
          return;
        job = std::move(pending.front());
        pending.pop_front();
      }
      Completion completion;
      completion.input_id = job.input_id;
      try {
        if (auto *model = std::get_if<ModelRequest>(&job.request)) {
          completion.result = prepare_model(std::move(*model), job.registry, settings);
        } else if (auto *variant = std::get_if<VariantRequest>(&job.request)) {
          completion.result = prepare_variant(std::move(*variant), job.registry, settings);
        } else {
          auto artifact = catalog::prepare_script(std::get<ScriptRequest>(job.request).source,
                                                  job.registry.terms, rights, job.registry.pin,
                                                  settings.pins, settings.limits);
          artifact.declaration.approved = false;
          artifact.declaration.active =
              artifact.declaration.kind != react::ScriptDeclaration::Kind::Rule;
          completion.result = std::move(artifact);
        }
      } catch (const std::exception &error) {
        completion.result = Failure{error.what()};
      }
      {
        std::lock_guard lock(mutex);
        completed.push_back(std::move(completion));
      }
    }
  }
  void enqueue(Job job) {
    {
      std::lock_guard lock(mutex);
      pending.push_back(std::move(job));
    }
    ready.notify_one();
  }
};
Worker::Worker(WorkerSettings settings) : impl_(std::make_unique<Impl>(std::move(settings))) {}
Worker::~Worker() = default;
void Worker::submit(std::uint64_t input_id, ModelRequest request, RegistrySnapshot registry) {
  impl_->enqueue({input_id, std::move(request), std::move(registry)});
}
void Worker::submit(std::uint64_t input_id, VariantRequest request, RegistrySnapshot registry) {
  impl_->enqueue({input_id, std::move(request), std::move(registry)});
}
void Worker::submit(std::uint64_t input_id, ScriptRequest request, RegistrySnapshot registry) {
  impl_->enqueue({input_id, std::move(request), std::move(registry)});
}
std::vector<Completion> Worker::poll() {
  std::lock_guard lock(impl_->mutex);
  std::vector<Completion> result;
  result.swap(impl_->completed);
  return result;
}
} // namespace amber::step::authoring
