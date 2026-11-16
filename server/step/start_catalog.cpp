#include "catalog/catalog.hpp"
#include "start_private.hpp"

#include <fstream>
#include <set>

namespace amber::step::start {
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
      {"spend", "build", "enter", "run_scripts", "animate", "camera"}};
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
std::vector<catalog::Asset> prepare_assets(const Json &plan, const std::filesystem::path &root,
                                           const catalog::RuntimePins &pins,
                                           const react::Terms &terms) {
  std::vector<catalog::Asset> result;
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
    const auto children = result[index].manifest.at("parts");
    const auto canonical = result[index].canonical_params;
    for (const auto &[name, child] : children.items()) {
      (void)name;
      load(child.at("asset").get<std::string>(), canonical);
    }
  }
  return result;
}
void import_assets(std::vector<catalog::Asset> prepared, catalog::Assets &assets) {
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
    const auto group = row.value("group", GroupId{});
    models.rows[result.root].group = group;
    for (const auto &[name, part] : result.parts)
      models.rows[part].group = group;
    if (!symbols.models.emplace(row.at("key").get<std::string>(), result.root).second)
      throw std::invalid_argument("duplicate initial model symbol");
  }
}
void install_catalog(const Json &plan, amber::react::State &react_state,
                     const catalog::Hash &registry, const catalog::RuntimePins &pins,
                     react::VmSettings limits, const std::filesystem::path &asset_root,
                     const std::filesystem::path &script_root, amber::catalog::State &catalog_state,
                     Symbols &symbols) {
  auto &vm = react_state.vm;

  auto &terms = react_state.terms;
  auto &assets = catalog_state.assets;
  auto &models = catalog_state.models;
  auto &scripts = catalog_state.scripts;
  auto &texts = catalog_state.model_text;
  auto &settings = react_state.model_settings;
  auto &rules = react_state.rules;

  auto prepared_scripts = prepare_scripts(plan, terms, registry, pins, limits);
  for (const auto &script : prepared_scripts)
    catalog::write_script(script_root / script.artifact.hash, script.artifact);
  std::vector<SeedRuleApproval> approvals;
  for (const auto &script : prepared_scripts)
    if (script.requested_rule)
      approvals.push_back({script.key, "trusted local initial-world content selected at launch"});
  install_scripts(std::move(prepared_scripts), scripts, vm, symbols, approvals);
  auto prepared_assets = prepare_assets(plan, asset_root, pins, terms);
  import_assets(std::move(prepared_assets), assets);
  install_models(plan, assets, scripts, terms, models, texts, settings, symbols);
  for (const auto &script : scripts.rows)
    if (script.id && script.declaration.kind == react::ScriptDeclaration::Kind::Rule)
      react::add_rule(rules, scripts, terms, script.id);
}

void bind_relation_reads(react::RelationsRead &reads, react::AuthoredRelations &authored,
                         const react::Terms &terms, const space::PhysicalRelations &physical,
                         std::array<TermId, 4> physical_terms) {
  reads.columns.clear();
  for (auto &[term, relation] : authored)
    reads.columns[term] = &relation;
  for (const auto &term : terms.rows)
    if (term.id && term.kind == react::Kind::Relation && !reads.columns.contains(term.id))
      reads.columns[term.id] = &authored[term.id];
  reads.columns[physical_terms[0]] = &physical.contains;
  reads.columns[physical_terms[1]] = &physical.supports;
  reads.columns[physical_terms[2]] = &physical.attached_to;
  reads.columns[physical_terms[3]] = &physical.in_zone;
}
} // namespace amber::step::start
