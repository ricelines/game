#pragma once

#include "catalog.hpp"

namespace amber::catalog {
struct RevisionAlias {
  Model model;
  ModelId inherit_from = 0;
};
struct RevisionVariant {
  std::uint32_t before_params = 0, proposal_params = 0, after_params = 0;
  Hash asset;
  std::string canonical_params;
  std::map<std::string, ModelId> parts;
};
struct ModelRevisionPlan {
  ModelId model = 0, proposal = 0;
  Hash before_source, before_asset;
  std::size_t model_count = 0, parameter_count = 0;
  Model after;
  std::vector<RevisionAlias> aliases;
  std::vector<RevisionVariant> variants;
  bool changed = false;
};
// referenced_params includes current instance blocks; the default (zero) is
// always included. New IDs are predictions valid until the single writer yields.
ModelRevisionPlan plan_revision(const Models &, const ParameterBlocks &, const Assets &,
                                ModelId model, ModelId proposal,
                                std::span<const std::uint32_t> referenced_params);
const RevisionVariant &revision_variant(const ModelRevisionPlan &, std::uint32_t before_params);
void commit_revision(Models &, ParameterBlocks &, ModelTexts &, react::ModelSettings &,
                     const ModelRevisionPlan &);
void project_revision_measurements(react::AssetMeasurements &, const ModelRevisionPlan &,
                                   const Assets &, MeasurementBindings);
} // namespace amber::catalog
