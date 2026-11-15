#include "revision.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace amber::catalog {
namespace {
const Model &model(const Models &models, ModelId id) {
  if (!id || id >= models.rows.size() || models.rows[id].id != id)
    throw std::invalid_argument("unknown model in definition edit");
  return models.rows[id];
}
} // namespace
ModelRevisionPlan plan_revision(const Models &models, const ParameterBlocks &parameters,
                                const Assets &assets, ModelId target, ModelId proposal,
                                std::span<const std::uint32_t> referenced) {
  const auto &before = model(models, target);
  const auto &next = model(models, proposal);
  if (!before.part_name.empty() || !next.part_name.empty())
    throw std::invalid_argument("definition editing requires complete model roots");
  ModelRevisionPlan plan;
  plan.model = target;
  plan.proposal = proposal;
  plan.before_source = before.source_hash;
  plan.before_asset = before.asset;
  plan.model_count = models.rows.size();
  plan.parameter_count = parameters.rows.size();
  plan.after = before;
  plan.after.asset = next.asset;
  plan.after.source_hash = next.source_hash;
  plan.after.canonical_params = next.canonical_params;
  std::vector<std::uint32_t> needed(referenced.begin(), referenced.end());
  needed.push_back(0);
  std::sort(needed.begin(), needed.end());
  needed.erase(std::unique(needed.begin(), needed.end()), needed.end());
  std::map<nlohmann::json, std::uint32_t> proposed_parameters;
  proposed_parameters.emplace(nlohmann::json::parse(next.canonical_params), 0);
  for (const auto &block : parameters.rows)
    if (block.id && block.model == proposal &&
        assets.rows.at(block.asset).source_hash == next.source_hash)
      proposed_parameters.emplace(nlohmann::json::parse(block.canonical_params), block.id);
  const auto &schema = assets.rows.at(next.asset).manifest.at("parameter_schema");
  auto defaults = nlohmann::json::object();
  for (const auto &[name, range] : schema.items())
    defaults[name] = range.at(0);
  for (auto old : needed) {
    const auto original = resolve_asset(models, parameters, target, old);
    std::uint32_t chosen = 0;
    if (old) {
      auto expected = defaults;
      const auto previous = nlohmann::json::parse(assets.rows.at(original.hash).canonical_params);
      for (const auto &[name, value] : previous.items()) {
        if (!schema.contains(name))
          throw std::invalid_argument("proposal removed a currently used parameter");
        expected[name] = value;
      }
      const auto found = proposed_parameters.find(expected);
      if (found == proposed_parameters.end())
        throw std::invalid_argument("proposal is missing a compatible current parameter set");
      chosen = found->second;
    }
    const auto effective = resolve_asset(models, parameters, proposal, chosen);
    const auto &asset = assets.rows.at(effective.hash);
    plan.variants.push_back({old, chosen, old, effective.hash, asset.canonical_params, {}});
    plan.changed |= original.hash != effective.hash;
  }
  if (!plan.changed) {
    plan.after = before;
    for (auto &variant : plan.variants)
      variant.parts = part_models(models, parameters, target, variant.before_params);
    return plan;
  }
  std::map<std::pair<ModelId, ModelId>, ModelId> aliases;
  std::function<ModelId(ModelId, ModelId)> alias;
  std::function<std::map<std::string, ModelId>(const std::map<std::string, ModelId> &,
                                               const std::map<std::string, ModelId> &)>
      parts;
  parts = [&](const auto &old, const auto &proposed) {
    std::map<std::string, ModelId> result;
    for (const auto &[name, child] : proposed) {
      const auto retained = old.find(name);
      result[name] = retained == old.end() ? child : alias(retained->second, child);
    }
    return result;
  };
  alias = [&](ModelId old_id, ModelId proposed_id) -> ModelId {
    const auto key = std::pair{old_id, proposed_id};
    if (const auto found = aliases.find(key); found != aliases.end())
      return found->second;
    const auto &old = model(models, old_id);
    const auto &proposed = model(models, proposed_id);
    Model changed = old;
    changed.id = static_cast<ModelId>(plan.model_count + plan.aliases.size());
    changed.asset = proposed.asset;
    changed.source_hash = proposed.source_hash;
    changed.part_name = proposed.part_name;
    changed.canonical_params = proposed.canonical_params;
    const auto id = changed.id;
    aliases.emplace(key, id);
    const auto slot = plan.aliases.size();
    plan.aliases.push_back({std::move(changed), old_id});
    auto children = parts(old.parts, proposed.parts);
    plan.aliases[slot].model.parts = std::move(children);
    return id;
  };
  std::uint32_t next_parameter = static_cast<std::uint32_t>(plan.parameter_count);
  for (auto &variant : plan.variants) {
    variant.parts = parts(part_models(models, parameters, target, variant.before_params),
                          part_models(models, parameters, proposal, variant.proposal_params));
    if (variant.before_params)
      variant.after_params = next_parameter++;
    else
      plan.after.parts = variant.parts;
  }
  return plan;
}
const RevisionVariant &revision_variant(const ModelRevisionPlan &plan, std::uint32_t before) {
  const auto found = std::find_if(plan.variants.begin(), plan.variants.end(),
                                  [&](const auto &row) { return row.before_params == before; });
  if (found == plan.variants.end())
    throw std::invalid_argument("parameter set was not included in the definition edit");
  return *found;
}
void commit_revision(Models &models, ParameterBlocks &parameters, ModelTexts &texts,
                     react::ModelSettings &settings, const ModelRevisionPlan &plan) {
  const auto &before = model(models, plan.model);
  if (models.rows.size() != plan.model_count || parameters.rows.size() != plan.parameter_count ||
      before.source_hash != plan.before_source || before.asset != plan.before_asset)
    throw std::invalid_argument("definition edit plan is stale");
  if (!plan.changed)
    return;
  for (const auto &alias : plan.aliases) {
    models.rows.push_back(alias.model);
    if (const auto inherited = texts.rows.find(alias.inherit_from); inherited != texts.rows.end())
      texts.rows.emplace(alias.model.id, inherited->second);
    if (const auto inherited = settings.rows.find(alias.inherit_from);
        inherited != settings.rows.end())
      settings.rows.emplace(alias.model.id, inherited->second);
  }
  models.rows[plan.model] = plan.after;
  for (const auto &variant : plan.variants)
    if (variant.before_params) {
      parameters.rows.push_back({variant.after_params, plan.model, variant.asset,
                                 variant.canonical_params, variant.parts});
      parameters.by_model_asset[{plan.model, variant.asset}] = variant.after_params;
    }
}
void project_revision_measurements(react::AssetMeasurements &measurements,
                                   const ModelRevisionPlan &plan, const Assets &assets,
                                   MeasurementBindings roles) {
  if (!plan.changed)
    return;
  project_measurements(measurements, plan.model, 0, assets.rows.at(plan.after.asset), roles);
  for (const auto &alias : plan.aliases)
    project_measurements(measurements, alias.model.id, 0, assets.rows.at(alias.model.asset), roles);
  for (const auto &variant : plan.variants)
    if (variant.before_params)
      project_measurements(measurements, plan.model, variant.after_params,
                           assets.rows.at(variant.asset), roles);
}
} // namespace amber::catalog
