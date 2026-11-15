#include "authoring.hpp"
#include "drive/drive.hpp"

#include <algorithm>
#include <set>

namespace amber::bindings::authoring {
Json failure(std::string error) {
  return {{"ok", false}, {"errors", Json::array({std::move(error)})}};
}
Json install_model(PreparedModel prepared, PlayerId author, double created_s,
                   CatalogInstall &binding) {
  try {
    if (prepared.assets.empty())
      throw std::invalid_argument("missing verified asset");
    if (!prepared.material || prepared.material >= binding.terms.rows.size() ||
        binding.terms.rows[prepared.material].kind != react::Kind::Material)
      throw std::invalid_argument("unknown material term");
    const auto root_hash = prepared.assets.front().asset.hash;
    std::vector<catalog::Hash> variant_hashes;
    for (const auto &variant : prepared.variants) {
      if (variant.empty())
        throw std::invalid_argument("missing verified variant asset");
      variant_hashes.push_back(variant.front().asset.hash);
    }
    auto each_asset = [&](auto &&visit) {
      for (auto &asset : prepared.assets)
        visit(asset);
      for (auto &variant : prepared.variants)
        for (auto &asset : variant)
          visit(asset);
    };
    // Prepare every geometry in the submission before exposing any family ID.
    std::set<catalog::Hash> geometry;
    each_asset([&](const catalog::PreparedAsset &asset) {
      if (binding.geometry.prepare && !binding.assets.rows.contains(asset.asset.hash) &&
          geometry.insert(asset.asset.hash).second)
        binding.geometry.prepare(binding.geometry.local, asset.asset);
    });
    each_asset([&](catalog::PreparedAsset &asset) {
      binding.sources.assets.try_emplace(asset.asset.hash, asset.source);
      catalog::import_asset(binding.assets, std::move(asset));
    });
    catalog::ModelSubmission submission;
    submission.asset = root_hash;
    submission.material = prepared.material;
    submission.author = author;
    submission.created_s = created_s;
    submission.text = std::move(prepared.text);
    auto register_family = [&](catalog::Models &models, catalog::ModelTexts &texts,
                               react::ModelSettings &settings, catalog::ParameterBlocks &params) {
      const auto root = catalog::submit_model(models, texts, settings, binding.assets,
                                              binding.scripts, binding.terms, submission);
      std::vector<std::uint32_t> variants;
      for (const auto &hash : variant_hashes)
        variants.push_back(catalog::add_variant(params, models, texts, settings, binding.assets,
                                                binding.scripts, binding.terms, root.root, hash,
                                                binding.assets.rows.at(hash).canonical_params));
      return std::pair{root, variants};
    };
    // Validation depends only on this proposal and immutable artifacts, so the
    // scratch tables stay bounded by its own family/variants, not world history.
    catalog::Models checked_models;
    catalog::ModelTexts checked_texts;
    react::ModelSettings checked_settings;
    catalog::ParameterBlocks checked_parameters;
    (void)register_family(checked_models, checked_texts, checked_settings, checked_parameters);
    const auto [registered, variants] =
        register_family(binding.models, binding.texts, binding.settings, binding.parameters);
    auto project = [&](ModelId id, std::uint32_t params, const catalog::Hash &hash) {
      catalog::project_measurements(binding.measurements, id, params, binding.assets.rows.at(hash),
                                    binding.measured_terms);
    };
    project(registered.root, 0, root_hash);
    for (const auto &[name, id] : registered.parts) {
      (void)name;
      project(id, 0, binding.models.rows[id].asset);
    }
    auto variant_rows = Json::array();
    for (std::size_t index = 0; index < variants.size(); ++index) {
      const auto params = variants[index];
      const auto &hash = variant_hashes[index];
      project(registered.root, params, hash);
      for (const auto &[name, child] : binding.parameters.rows[params].parts) {
        (void)name;
        project(child, 0, binding.models.rows[child].asset);
      }
      variant_rows.push_back({{"params", params},
                              {"hash", hash},
                              {"measured", binding.assets.rows.at(hash).manifest}});
    }
    return {{"ok", true},
            {"model", "m:" + std::to_string(registered.root)},
            {"hash", root_hash},
            {"measured", binding.assets.rows.at(root_hash).manifest},
            {"variants", std::move(variant_rows)}};
  } catch (const std::exception &error) {
    return failure(error.what());
  }
}
Json install_script(catalog::PreparedScript prepared, PlayerId author, double created_s,
                    ScriptInstall &binding) {
  try {
    if (command::collides(binding.native, prepared.declaration))
      throw std::invalid_argument("script action collides with a reserved player action");
    prepared.declaration.approved = false;
    prepared.declaration.active = prepared.declaration.kind != react::ScriptDeclaration::Kind::Rule;
    const auto id = static_cast<ScriptId>(binding.scripts.rows.size());
    prepared.declaration.id = id;
    prepared.declaration.author = author;
    catalog::write_script(binding.script_root / prepared.hash, prepared);
    const auto loaded = react::load_script(binding.vm, prepared.compiled, prepared.declaration);
    if (loaded.status != react::RunOutcome::Status::Ok)
      throw std::runtime_error("script failed VM loading: " + loaded.error);
    const auto hash = prepared.hash;
    const auto source = prepared.source;
    catalog::import_script(binding.scripts, std::move(prepared), author, created_s);
    binding.sources.scripts.emplace(id, source);
    return {{"ok", true}, {"script", "s:" + std::to_string(id)}, {"hash", hash}};
  } catch (const std::exception &error) {
    return failure(error.what());
  }
}
Json install_variant(PreparedVariant prepared, CatalogInstall &binding) {
  try {
    if (!prepared.model || prepared.model >= binding.models.rows.size() || prepared.assets.empty())
      throw std::invalid_argument("variant target or verified assets are missing");
    const auto &model = binding.models.rows[prepared.model];
    const auto &root = prepared.assets.front().asset;
    if (model.source_hash != root.source_hash)
      throw std::invalid_argument(
          "variant model source changed; rebake against the current source");
    const auto root_hash = root.hash;
    const auto canonical = root.canonical_params;
    for (const auto &asset : prepared.assets)
      if (binding.geometry.prepare && !binding.assets.rows.contains(asset.asset.hash))
        binding.geometry.prepare(binding.geometry.local, asset.asset);
    for (auto &asset : prepared.assets) {
      binding.sources.assets.try_emplace(asset.asset.hash, asset.source);
      catalog::import_asset(binding.assets, std::move(asset));
    }
    const auto params = catalog::add_variant(binding.parameters, binding.models, binding.texts,
                                             binding.settings, binding.assets, binding.scripts,
                                             binding.terms, prepared.model, root_hash, canonical);
    catalog::project_measurements(binding.measurements, prepared.model, params,
                                  binding.assets.rows.at(root_hash), binding.measured_terms);
    for (const auto &[name, child] : binding.parameters.rows[params].parts) {
      (void)name;
      catalog::project_measurements(binding.measurements, child, 0,
                                    binding.assets.rows.at(binding.models.rows[child].asset),
                                    binding.measured_terms);
    }
    return {{"ok", true},
            {"model", "m:" + std::to_string(prepared.model)},
            {"params", params},
            {"hash", root_hash},
            {"measured", binding.assets.rows.at(root_hash).manifest}};
  } catch (const std::exception &error) {
    return failure(error.what());
  }
}
AppliedCompletion complete_authoring(const drive::Input &input, Completion completion,
                                     bool session_live, CatalogInstall &binding,
                                     ScriptInstall &scripts, record::Journal &journal, Step step,
                                     double hz, std::span<const PlayerId> seed_authors) {
  AppliedCompletion applied;
  const auto author = std::get<PlayerId>(input.caller);
  const auto previous_models = binding.models.rows.size();
  const auto previous_parameters = binding.parameters.rows.size();
  if (!session_live)
    applied.reply = failure("session_closed");
  else if (auto *failed = std::get_if<Failure>(&completion.result))
    applied.reply = failure(failed->error);
  else if (auto *model = std::get_if<PreparedModel>(&completion.result))
    applied.reply = install_model(std::move(*model), author, step / hz, binding);
  else if (auto *variant = std::get_if<PreparedVariant>(&completion.result))
    applied.reply = install_variant(std::move(*variant), binding);
  else
    applied.reply = install_script(std::move(std::get<catalog::PreparedScript>(completion.result)),
                                   author, step / hz, scripts);
  const bool success = applied.reply.value("ok", false);
  const bool variant = input.operation == "register_variant";
  const bool changed = success && (!variant || binding.models.rows.size() != previous_models ||
                                   binding.parameters.rows.size() != previous_parameters);
  record::Event event;
  event.step = step;
  event.actor = record::Actor::player(author);
  event.kind = variant ? record::Kind::Action : record::Kind::Create;
  event.name = input.operation;
  event.changed = changed;
  event.result = !success  ? record::Result::Denied
                 : changed ? record::Result::Applied
                           : record::Result::NoEffect;
  event.detail = applied.reply;
  event.detail["of"] = variant                               ? "model_variant"
                       : input.operation == "register_model" ? "model"
                                                             : "script";
  event.detail["maker"] = "p:" + std::to_string(author);
  if (success && variant) {
    const auto &source = binding.models.rows.at(variant_request(input.fields).model);
    event.credits.push_back(
        {source.author, source.created_s,
         std::binary_search(seed_authors.begin(), seed_authors.end(), source.author)});
  }
  applied.reply["record_id"] = journal.append(std::move(event), input.origin).cause;
  for (auto id = previous_models; id < binding.models.rows.size(); ++id)
    applied.added_model_ids.push_back(static_cast<ModelId>(id));
  return applied;
}
} // namespace amber::bindings::authoring
