#include "common.hpp"

#include <fstream>
#include <set>

namespace amber::bindings::start {
Json read_plan(const std::filesystem::path &path) {
  std::ifstream input(path);
  auto plan = Json::parse(input);
  if (plan.at("version") != 1)
    throw std::invalid_argument("unsupported start plan version");
  return plan;
}
catalog::RegistryDocument load_registry(const Json &plan, const std::filesystem::path &root,
                                        react::Terms &terms, react::Materials &materials,
                                        catalog::MechanicalBindings &mechanical) {
  const auto &data = plan.at("registry");
  const catalog::RegistryDocument source{catalog::sha256(data.dump()), data};
  catalog::load_terms(source, terms);
  catalog::load_materials(source, terms, materials);
  mechanical.values = data.at("mechanical");
  const auto hash = catalog::save_registry(root, terms, materials, {}, mechanical);
  return catalog::read_registry(root, hash);
}
std::vector<PreparedSeedScript> prepare_scripts(const Json &plan, const react::Terms &terms,
                                                const catalog::Hash &registry,
                                                const catalog::RuntimePins &pins,
                                                react::VmSettings limits) {
  const react::RightsDeclaration rights{
      {"use", "take", "move", "edit", "copy", "transfer", "destroy"},
      {"spend", "build", "enter", "run_scripts", "animate", "camera", "issue"}};
  std::vector<PreparedSeedScript> result;
  std::set<std::string> keys;
  for (const auto &row : plan.at("scripts")) {
    const auto key = row.at("key").get<std::string>();
    if (!keys.insert(key).second)
      throw std::invalid_argument("duplicate start script key");
    auto prepared = catalog::prepare_script(row.at("source").get<std::string>(), terms, rights,
                                            registry, pins, limits);
    prepared.declaration.approved = false;
    prepared.declaration.active = prepared.declaration.kind != react::ScriptDeclaration::Kind::Rule;
    result.push_back({key, row.at("author").get<PlayerId>(), row.at("world_rule").get<bool>(),
                      std::move(prepared)});
  }
  return result;
}
void install_scripts(std::vector<PreparedSeedScript> prepared, catalog::Scripts &scripts,
                     react::Vm &vm, Symbols &symbols, std::span<const SeedRuleApproval> approvals) {
  for (auto &row : prepared) {
    const auto found = std::find_if(approvals.begin(), approvals.end(), [&](const auto &approval) {
      return approval.key == row.key && !approval.evidence.empty();
    });
    auto &declaration = row.artifact.declaration;
    if (row.requested_rule != (declaration.kind == react::ScriptDeclaration::Kind::Rule))
      throw std::invalid_argument("initial world_rule request disagrees with derived declaration");
    if (row.requested_rule && found != approvals.end()) {
      declaration.approved = true;
      declaration.active = true;
    }
    auto compiled = std::move(row.artifact.compiled);
    const auto id = catalog::import_script(scripts, std::move(row.artifact), row.author, 0);
    const auto loaded = react::load_script(vm, compiled, scripts.rows[id].declaration);
    if (loaded.status != react::RunOutcome::Status::Ok)
      throw std::runtime_error("initial script failed actual VM loading: " + loaded.error);
    if (!symbols.scripts.emplace(row.key, id).second)
      throw std::invalid_argument("duplicate installed script symbol");
  }
}
std::vector<catalog::PreparedAsset> prepare_assets(const Json &plan,
                                                   const std::filesystem::path &root,
                                                   const catalog::RuntimePins &pins,
                                                   const react::Terms &terms) {
  std::vector<catalog::PreparedAsset> result;
  std::set<std::string> hashes;
  auto load = [&](const std::string &hash, const std::string &canonical) {
    if (!hashes.insert(hash).second)
      return;
    auto prepared = catalog::read_asset(root, hash, canonical, pins);
    catalog::resolve_manifest_terms(prepared, terms);
    result.push_back(std::move(prepared));
  };
  for (const auto &row : plan.at("models"))
    load(row.at("asset").get<std::string>(), row.at("canonical_params").get<std::string>());
  for (std::size_t index = 0; index < result.size(); ++index) {
    // Copy before push_back: a child insertion can reallocate the prepared vector.
    const auto children = result[index].asset.manifest.at("parts");
    const auto canonical = result[index].asset.canonical_params;
    for (const auto &[name, child] : children.items()) {
      (void)name;
      load(child.at("asset").get<std::string>(), canonical);
    }
  }
  return result;
}
void import_assets(std::vector<catalog::PreparedAsset> prepared, catalog::Assets &assets) {
  for (auto &asset : prepared)
    catalog::import_asset(assets, std::move(asset));
}
void install_models(const Json &plan, const catalog::Assets &assets,
                    const catalog::Scripts &scripts, const react::Terms &terms,
                    catalog::Models &models, catalog::ModelTexts &texts,
                    react::ModelSettings &settings, Symbols &symbols) {
  for (const auto &row : plan.at("models")) {
    catalog::ModelSubmission submission;
    submission.asset = row.at("asset").get<std::string>();
    submission.material = detail::term(terms, row.at("material"));
    submission.author = row.at("author").get<PlayerId>();
    if (row.contains("script"))
      submission.script = symbols.scripts.at(row.at("script").get<std::string>());
    const auto &text = row.at("text");
    submission.text = {text.value("name", std::string{}), text.value("description", std::string{}),
                       text.value("writing", std::string{})};
    const auto authored_settings = row.value("settings", Json::object());
    for (const auto &[name, value] : authored_settings.items())
      submission.settings.emplace(detail::term(terms, name), detail::scalar(value, symbols));
    const auto result =
        catalog::submit_model(models, texts, settings, assets, scripts, terms, submission);
    if (!symbols.models.emplace(row.at("key").get<std::string>(), result.root).second)
      throw std::invalid_argument("duplicate initial model symbol");
  }
}
} // namespace amber::bindings::start
