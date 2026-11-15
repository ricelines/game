#pragma once

#include "bindings/presentation.hpp"
#include "catalog/catalog.hpp"
#include "perm/perm.hpp"
#include "react/allocation.hpp"

namespace amber::control {
class Sessions;
}
namespace amber::drive {
struct Clock;
}
namespace amber::bindings {
struct IlluminanceReads;
namespace viewing {
struct SessionViews;
}
// Shared invalidation and publication backlog for authored and structural edits.
struct MetadataChanges {
  viewing::SessionViews &views;
  IlluminanceReads &illuminance;
  std::vector<Handle> objects;
  std::vector<ModelId> models;
};
void invalidate_metadata(MetadataChanges &, std::span<const Handle>);
void metadata_changed(MetadataChanges &, std::span<const Handle>, std::span<const ModelId> = {});

// Collects definition changes and publishes one coherent catalog/metadata view.
// Retained inputs are readonly; the only table writes rebuild derived dispatch.
class CatalogPublication {
public:
  CatalogPublication(const catalog::Models &, const catalog::Scripts &, const react::Terms &,
                     const react::Materials &, const catalog::ModelTexts &,
                     const catalog::InstanceTexts &, const react::ValueRead &, AuthorityRead,
                     ParentRead, react::Triggers &, react::PriorityLinks &, MetadataChanges &,
                     MaterialAppearanceRoles);
  void definitions_changed(std::span<const ModelId>);
  void registry_changed(std::span<const TermId>, std::span<const TermId>);
  void flush(control::Sessions &);
  const nlohmann::json &initial_catalog() const { return catalog_; }
  const std::vector<TermId> &inspection_terms() const { return inspection_terms_; }

private:
  const catalog::Models &models_;
  const catalog::Scripts &scripts_;
  const react::Terms &terms_;
  const react::Materials &materials_;
  const catalog::ModelTexts &model_text_;
  const catalog::InstanceTexts &instance_text_;
  const react::ValueRead &values_;
  AuthorityRead authority_;
  ParentRead parents_;
  react::Triggers &triggers_;
  react::PriorityLinks &priorities_;
  MetadataChanges &changes_;
  MaterialAppearanceRoles appearance_;
  std::vector<ModelId> model_ids_;
  std::vector<TermId> term_ids_, material_ids_, inspection_terms_, added_terms_, changed_materials_;
  nlohmann::json catalog_;
};
struct MaterialRefresh;
struct RegistryPersistence {
  const react::Terms &terms;
  const react::Materials &materials;
  const react::RuleVersions &rules;
  const catalog::Scripts &scripts;
  const catalog::MechanicalBindings &mechanical;
  catalog::RegistryDocument &document;
  std::filesystem::path directory;
  MaterialRefresh &physics;
  const drive::Clock &clock;
  double hz;
  CatalogPublication &publication;
};
catalog::Hash commit_registry(void *, std::span<const TermId>, std::span<const TermId>);
void publish_rule_registry(void *, ScriptId, bool approved, bool active, Step, double,
                           const record::Origin &, react::DeferredEffects &);

struct ContextBinding {
  AuthorityRead authority;
};
struct ObjectBinding {
  IdentityRead identity;
  TransformRead transforms;
  AuthorityRead authority;
  ParentRead parents;
  const std::vector<std::uint32_t> &params;
  const catalog::Models &models;
  const SparseColumn<std::vector<ScriptId>> &instances;
};
struct AssetReadBinding {
  IdentityRead identity;
  const std::vector<std::uint32_t> &params;
  const catalog::Models &models;
  const catalog::ParameterBlocks &parameters;
  const catalog::Assets &assets;
};
struct MaterialReadBinding {
  const react::Terms &terms;
  const react::Materials &materials;
};
struct ModelMeasureBinding {
  const react::Terms &terms;
  const react::Materials &materials;
  const catalog::Models &models;
  const catalog::ParameterBlocks &parameters;
  const catalog::Assets &assets;
  TermId density;
};
struct InstanceMeasureBinding {
  IdentityRead identity;
  TransformRead transforms;
  const std::vector<std::uint32_t> &params;
  const Relation &declared_parts;
  const react::Terms &terms;
  const react::Materials &materials;
  const catalog::Models &models;
  const catalog::ParameterBlocks &parameters;
  const catalog::Assets &assets;
  TermId density;
};
struct MeterReadBinding {
  const react::MeterRows &meters;
  const react::Terms &terms;
  IdentityRead identity;
};
struct PlotReadBinding {
  const perm::Plots &plots;
  AuthorityRead authority;
};
int context_binding(lua_State *);
int object_binding(lua_State *);
int asset_binding(lua_State *);
int material_binding(lua_State *);
int measure_model_binding(lua_State *);
int measure_instance_binding(lua_State *);
int meter_binding(lua_State *);
int plot_binding(lua_State *);
} // namespace amber::bindings
