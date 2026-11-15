#pragma once
#include "observe/observe.hpp"

namespace amber::bindings {
observe::ObjectDescription describe_object(Handle, IdentityRead, const catalog::Models &,
                                           const catalog::ParameterBlocks &,
                                           const catalog::Assets &, const catalog::ModelTexts &,
                                           const catalog::InstanceTexts &, const react::ValueRead &,
                                           std::span<const TermId> inspection_terms,
                                           std::span<const std::string> declared_actions, Step,
                                           double hz);
nlohmann::json object_metadata(IdentityRead, AuthorityRead, ParentRead, const catalog::ModelTexts &,
                               const catalog::InstanceTexts &, std::span<const Handle>);
nlohmann::json model_metadata(const catalog::Models &, const catalog::ModelTexts &,
                              std::span<const ModelId>);
nlohmann::json term_metadata(const react::Terms &, std::span<const TermId>);
struct MaterialAppearanceRoles {
  TermId red = 0, green = 0, blue = 0, alpha = 0, metallic = 0, roughness = 0;
};
nlohmann::json material_metadata(const react::Terms &, const react::Materials &,
                                 MaterialAppearanceRoles, std::span<const TermId>);
struct ObserverBinding {
  Vec3 eye, forward;
  observe::Scalar dim_lux;
};
struct BoxSensor {
  Vec3 size, offset;
};
struct ZoneBinding {
  observe::Scalar ambient;
  int priority;
  std::optional<BoxSensor> box;
};
std::optional<observe::LightBinding> light_binding(const catalog::Asset &, const react::Terms &,
                                                   Handle source, float default_source_radius_m);
std::optional<ZoneBinding> zone_binding(const catalog::Asset &, const react::Terms &);
std::optional<ObserverBinding> observer_binding(const catalog::Asset &, const react::Terms &);
observe::Observer observer(const ObserverBinding &, Handle, TransformRead, const react::ValueRead &,
                           Step, double hz);
} // namespace amber::bindings
