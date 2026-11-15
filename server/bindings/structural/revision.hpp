#pragma once

#include "structural.hpp"

namespace amber::bindings {
class CatalogPublication;
struct ModelRevisionRead {
  IdentityRead identity;
  const std::vector<std::uint32_t> &params;
  const lifecycle::Parts &parts;
  const catalog::Models &models;
  const catalog::ParameterBlocks &parameters;
  const catalog::Assets &assets;
  const react::Materials &materials;
  const react::Terms &terms;
  TermId density;
};
struct RevisionMasses {
  std::size_t count = 0;
  std::map<TermId, double> before, after;
};
// Rare authoring query. Uses the same prospective bindings/material override
// choices as commit; each ordinary object's own disjoint volume is counted once.
RevisionMasses measure_model_change(const ModelRevisionRead &, ModelId model, ModelId proposal);
struct ModelRevisionPrerequisite {
  void *local = nullptr;
  bool (*preflight)(const void *, std::span<const Handle> affected, std::span<const Handle> removed,
                    std::vector<space::VisibleCustody> &forecast, std::string &error) = nullptr;
  void (*commit)(void *, std::span<const Handle> affected, const InvocationInput &,
                 react::DeferredEffects &) = nullptr;
};
struct ModelRevisionOperation {
  const ModelRevisionRead &read;
  catalog::Models &models;
  catalog::ParameterBlocks &parameters;
  catalog::ModelTexts &texts;
  react::ModelSettings &settings;
  react::AssetMeasurements &measurements;
  catalog::MeasurementBindings measurement_roles;
  ReplacementEdit replacement;
  CatalogPublication &publication;
  ModelRevisionPrerequisite prerequisite{};
};
drive::StructuralCompletion revise_model(void *, const drive::StructuralCall &);
} // namespace amber::bindings
