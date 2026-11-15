#pragma once

#include "catalog.hpp"

namespace amber::catalog {
struct RevisedParameter {
  std::uint32_t before = 0, after = 0;
};
struct ModelUpdate {
  ModelId model = 0;
  bool changed = false;
  std::vector<ModelId> models;
  std::vector<RevisedParameter> parameters;
};
// Resolve every registered parameter set against the proposal's baked sets.
// The default adopts the new defaults; explicit values survive on retained keys.
std::vector<RevisedParameter> definition_parameters(const Models &, const ParameterBlocks &,
                                                    const Assets &, ModelId model,
                                                    ModelId proposal);
// A rare definition edit on the single writer. Parameter block identities stay
// usable and receive the new geometry; no old definition can be reconfigured in.
ModelUpdate update_definition(Models &, ParameterBlocks &, ModelTexts &, react::ModelSettings &,
                              const Assets &, ModelId model, ModelId proposal);
void project_definition_measurements(react::AssetMeasurements &, const Models &,
                                     const ParameterBlocks &, const Assets &, const ModelUpdate &,
                                     MeasurementBindings);
} // namespace amber::catalog
