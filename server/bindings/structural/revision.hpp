#pragma once

#include "structural.hpp"

namespace amber::bindings {
class CatalogPublication;
namespace commerce {
struct RebindOffersBinding;
}
struct ModelDefinitionEdit {
  catalog::Models &models;
  catalog::ParameterBlocks &parameters;
  catalog::ModelTexts &texts;
  react::ModelSettings &settings;
  const catalog::Assets &assets;
  react::AssetMeasurements &measurements;
  catalog::MeasurementBindings measurement_roles;
  ReplacementEdit replacement;
  CatalogPublication &publication;
  commerce::RebindOffersBinding &offers;
};
TermId declare_model_edit(react::Terms &, command::NativeActions &);
void register_model_edit(drive::NativeOperations &, TermId, ModelDefinitionEdit &);
} // namespace amber::bindings
