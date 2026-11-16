#include "catalog/catalog.hpp"
#include "step.hpp"

#include <algorithm>
#include <set>

namespace amber::step::authoring {
Json failure(std::string error) {
  return {{"ok", false}, {"errors", Json::array({std::move(error)})}};
}
step::NativeCompletion install_model(PreparedModel prepared, PlayerId author, double created_s,
                                     amber::catalog::State &catalog_state,
                                     amber::react::State &react_state,
                                     catalog::MeasurementBindings measured_terms) {
  auto &assets = catalog_state.assets;
  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;

  auto &settings = react_state.model_settings;
  auto &measurements = catalog_state.baked_measurements;
  auto &scripts = catalog_state.scripts;
  auto &terms = react_state.terms;

  step::NativeCompletion out;
  const auto before = models.rows.size();
  try {
    if (prepared.assets.empty())
      throw std::invalid_argument("missing verified asset");
    if (!prepared.material || prepared.material >= terms.rows.size() ||
        terms.rows[prepared.material].kind != react::Kind::Material)
      throw std::invalid_argument("unknown material term");
    const auto root_hash = prepared.assets.front().hash;
    std::vector<catalog::Hash> variant_hashes;
    for (const auto &variant : prepared.variants) {
      if (variant.empty())
        throw std::invalid_argument("missing verified variant asset");
      variant_hashes.push_back(variant.front().hash);
    }
    auto each_asset = [&](auto &&visit) {
      for (auto &asset : prepared.assets)
        visit(asset);
      for (auto &variant : prepared.variants)
        for (auto &asset : variant)
          visit(asset);
    };
    each_asset([&](catalog::Asset &asset) { catalog::import_asset(assets, std::move(asset)); });
    catalog::ModelSubmission submission;
    submission.asset = root_hash;
    submission.material = prepared.material;
    submission.author = author;
    submission.created_s = created_s;
    submission.text = std::move(prepared.text);
    auto register_family = [&](catalog::State &family, react::ModelSettings &settings) {
      const auto root = catalog::submit_model(family.models, family.model_text, settings, assets,
                                              scripts, terms, submission);
      std::vector<std::uint32_t> variants;
      for (const auto &hash : variant_hashes)
        variants.push_back(catalog::add_variant(family, catalog_state, settings, terms, root.root,
                                                hash, assets.rows.at(hash).canonical_params));
      return std::pair{root, variants};
    };
    // Validation depends only on this proposal and immutable artifacts, so the
    // scratch tables stay bounded by its own family/variants, not world history.
    catalog::State checked_catalog;
    react::ModelSettings checked_settings;
    (void)register_family(checked_catalog, checked_settings);
    const auto [registered, variants] = register_family(catalog_state, settings);
    auto project = [&](ModelId id, std::uint32_t params, const catalog::Hash &hash) {
      catalog::project_measurements(measurements, id, params, assets.rows.at(hash), measured_terms);
    };
    project(registered.root, 0, root_hash);
    for (const auto &[name, id] : registered.parts) {
      (void)name;
      project(id, 0, models.rows[id].asset);
    }
    auto variant_rows = Json::array();
    for (std::size_t index = 0; index < variants.size(); ++index) {
      const auto params = variants[index];
      const auto &hash = variant_hashes[index];
      project(registered.root, params, hash);
      for (const auto &[name, child] : parameters.rows[params].parts) {
        (void)name;
        project(child, 0, models.rows[child].asset);
      }
      variant_rows.push_back(
          {{"params", params}, {"hash", hash}, {"measured", assets.rows.at(hash).manifest}});
    }
    out.reply = {{"ok", true},
                 {"model", "m:" + std::to_string(registered.root)},
                 {"hash", root_hash},
                 {"measured", assets.rows.at(root_hash).manifest},
                 {"variants", std::move(variant_rows)}};
    out.changed = true;
    out.result = record::Result::Applied;
    for (auto id = before; id < models.rows.size(); ++id)
      out.changed_models.push_back(static_cast<ModelId>(id));
    return out;
  } catch (const std::exception &error) {
    out.reply = failure(error.what());
    out.result = record::Result::Denied;
    return out;
  }
}
step::NativeCompletion install_script(catalog::PreparedScript prepared, PlayerId author,
                                      double created_s, catalog::Scripts &scripts, react::Vm &vm,
                                      const react::NativeActions &native,
                                      const std::filesystem::path &script_root) {
  step::NativeCompletion out;
  try {
    if (react::collides(native, prepared.declaration))
      throw std::invalid_argument("script action collides with a reserved player action");
    prepared.declaration.approved = false;
    prepared.declaration.active = prepared.declaration.kind != react::ScriptDeclaration::Kind::Rule;
    const auto id = static_cast<ScriptId>(scripts.rows.size());
    prepared.declaration.id = id;
    prepared.declaration.author = author;
    catalog::write_script(script_root / prepared.hash, prepared);
    const auto loaded = react::load_script(vm, prepared.compiled, prepared.declaration);
    if (loaded.status != react::RunOutcome::Status::Ok)
      throw std::runtime_error("script failed VM loading: " + loaded.error);
    const auto hash = prepared.hash;
    catalog::import_script(scripts, std::move(prepared), author, created_s);
    out.reply = {{"ok", true}, {"script", "s:" + std::to_string(id)}, {"hash", hash}};
    out.changed = true;
    out.result = record::Result::Applied;
    out.changed_scripts.push_back(id);
    return out;
  } catch (const std::exception &error) {
    out.reply = failure(error.what());
    out.result = record::Result::Denied;
    return out;
  }
}
step::NativeCompletion install_variant(PreparedVariant prepared,
                                       amber::catalog::State &catalog_state,
                                       amber::react::State &react_state,
                                       catalog::MeasurementBindings measured_terms) {
  auto &assets = catalog_state.assets;
  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;

  auto &measurements = catalog_state.baked_measurements;

  step::NativeCompletion out;
  const auto before_models = models.rows.size();
  const auto before_params = parameters.rows.size();
  try {
    if (!prepared.model || prepared.model >= models.rows.size() || prepared.assets.empty())
      throw std::invalid_argument("variant target or verified assets are missing");
    const auto &model = models.rows[prepared.model];
    const auto &root = prepared.assets.front();
    if (model.source_hash != root.source_hash)
      throw std::invalid_argument(
          "variant model source changed; rebake against the current source");
    const auto root_hash = root.hash;
    const auto canonical = root.canonical_params;
    for (auto &asset : prepared.assets) {
      catalog::import_asset(assets, std::move(asset));
    }
    const auto params =
        catalog::add_variant(catalog_state, catalog_state, react_state.model_settings,
                             react_state.terms, prepared.model, root_hash, canonical);
    catalog::project_measurements(measurements, prepared.model, params, assets.rows.at(root_hash),
                                  measured_terms);
    for (const auto &[name, child] : parameters.rows[params].parts) {
      (void)name;
      catalog::project_measurements(measurements, child, 0,
                                    assets.rows.at(models.rows[child].asset), measured_terms);
    }
    out.reply = {{"ok", true},
                 {"model", "m:" + std::to_string(prepared.model)},
                 {"params", params},
                 {"hash", root_hash},
                 {"measured", assets.rows.at(root_hash).manifest}};
    out.changed = before_models != models.rows.size() || before_params != parameters.rows.size();
    out.result = out.changed ? record::Result::Applied : record::Result::NoEffect;
    if (out.changed)
      out.changed_models.push_back(prepared.model);
    for (auto id = before_models; id < models.rows.size(); ++id)
      out.changed_models.push_back(static_cast<ModelId>(id));
    return out;
  } catch (const std::exception &error) {
    out.reply = failure(error.what());
    out.result = record::Result::Denied;
    return out;
  }
}
} // namespace amber::step::authoring
