#include "catalog.hpp"

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
std::vector<RevisedParameter> definition_parameters(const Models &models,
                                                    const ParameterBlocks &parameters,
                                                    const Assets &assets, ModelId target,
                                                    ModelId proposal) {
  const auto &before = model(models, target);
  const auto &next = model(models, proposal);
  if (!before.part_name.empty() || !next.part_name.empty())
    throw std::invalid_argument("definition editing requires complete model roots");
  std::map<nlohmann::json, std::uint32_t> proposed;
  proposed.emplace(nlohmann::json::parse(next.canonical_params), 0);
  for (const auto &block : parameters.rows)
    if (block.id && block.model == proposal &&
        assets.rows.at(block.asset).source_hash == next.source_hash)
      proposed.emplace(nlohmann::json::parse(block.canonical_params), block.id);
  const auto &schema = assets.rows.at(next.asset).manifest.at("parameter_schema");
  auto defaults = nlohmann::json::object();
  for (const auto &[name, range] : schema.items())
    defaults[name] = range.at(0);
  std::vector<RevisedParameter> result{{0, 0}};
  for (const auto &block : parameters.rows) {
    if (!block.id || block.model != target)
      continue;
    auto expected = defaults;
    const auto previous = nlohmann::json::parse(block.canonical_params);
    for (const auto &[name, value] : previous.items())
      if (schema.contains(name))
        expected[name] = value;
    const auto found = proposed.find(expected);
    if (found == proposed.end())
      throw std::invalid_argument("proposal is missing a compatible current parameter set");
    result.push_back({block.id, found->second});
  }
  return result;
}
ModelUpdate update_definition(Models &models, ParameterBlocks &parameters, ModelTexts &texts,
                              const Assets &assets, ModelId target, ModelId proposal) {
  const auto mappings = definition_parameters(models, parameters, assets, target, proposal);
  const auto &before = model(models, target);
  const auto &next = model(models, proposal);
  struct Alias {
    Model model;
    ModelId inherit_from;
  };
  struct Variant {
    std::uint32_t before_params, proposal_params, after_params;
    Hash asset;
    std::string canonical_params;
    std::map<std::string, ModelId> parts;
  };
  ModelUpdate result;
  result.model = target;
  std::vector<Alias> aliases;
  std::vector<Variant> variants;
  const auto model_count = models.rows.size();
  auto after = before;
  after.asset = next.asset;
  after.source_hash = next.source_hash;
  after.canonical_params = next.canonical_params;
  for (const auto &[old, chosen] : mappings) {
    const auto original = resolve_asset(models, parameters, target, old);
    const auto effective = resolve_asset(models, parameters, proposal, chosen);
    const auto &asset = assets.rows.at(effective.hash);
    variants.push_back({old, chosen, old, effective.hash, asset.canonical_params, {}});
    result.changed |= original.hash != effective.hash;
  }
  if (!result.changed) {
    return result;
  }
  std::map<std::pair<ModelId, ModelId>, ModelId> alias_ids;
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
    if (const auto found = alias_ids.find(key); found != alias_ids.end())
      return found->second;
    const auto &old = model(models, old_id);
    const auto &proposed = model(models, proposed_id);
    Model changed = old;
    changed.id = static_cast<ModelId>(model_count + aliases.size());
    changed.asset = proposed.asset;
    changed.source_hash = proposed.source_hash;
    changed.part_name = proposed.part_name;
    changed.canonical_params = proposed.canonical_params;
    const auto id = changed.id;
    alias_ids.emplace(key, id);
    const auto slot = aliases.size();
    aliases.push_back({std::move(changed), old_id});
    auto children = parts(old.parts, proposed.parts);
    aliases[slot].model.parts = std::move(children);
    return id;
  };
  for (auto &variant : variants) {
    variant.parts = parts(part_models(models, parameters, target, variant.before_params),
                          part_models(models, parameters, proposal, variant.proposal_params));
    if (!variant.before_params)
      after.parts = variant.parts;
  }
  for (const auto &alias : aliases) {
    models.rows.push_back(alias.model);
    if (const auto inherited = texts.rows.find(alias.inherit_from); inherited != texts.rows.end())
      texts.rows.emplace(alias.model.id, inherited->second);
    result.inherited_settings.emplace_back(alias.model.id, alias.inherit_from);
    result.models.push_back(alias.model.id);
  }
  models.rows[target] = std::move(after);
  result.models.push_back(target);
  for (const auto &variant : variants) {
    if (variant.before_params) {
      auto &block = parameters.rows.at(variant.before_params);
      block.asset = variant.asset;
      block.canonical_params = variant.canonical_params;
      block.parts = variant.parts;
    }
    result.parameters.push_back({variant.before_params, variant.after_params});
  }
  return result;
}
} // namespace amber::catalog
