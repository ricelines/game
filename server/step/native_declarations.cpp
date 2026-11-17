#include "catalog/catalog.hpp"
#include "step.hpp"
namespace amber::step {
using Json = nlohmann::json;
void declare_construction_actions(react::Terms &terms, react::NativeActions &declarations) {
  const Json vector = {
      {"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}};
  const Json quaternion = {
      {"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 4}, {"maxItems", 4}};
  const Json str = {{"type", "string"}};
  const Json material = {
      {"anyOf",
       Json::array({str, {{"type", "integer"}, {"minimum", 1}, {"maximum", UINT32_MAX}}})}};
  const Json params = {{"type", "integer"}, {"minimum", 0}, {"maximum", UINT32_MAX}};
  const auto add = [&](std::string name, std::string description, std::string right, bool reach,
                       Json properties, Json required, react::NativeOperation operation) {
    catalog::declare_native(declarations, terms, std::move(name), std::move(description),
                            std::move(right), reach,
                            {{"type", "object"},
                             {"properties", std::move(properties)},
                             {"required", std::move(required)},
                             {"additionalProperties", false}},
                            operation);
  };
  add("engine.create", "Place a model with the caller's Build permission on its destination plot.",
      "", false,
      {{"model", str},
       {"params", params},
       {"material", material},
       {"position", vector},
       {"rotation", quaternion}},
      {"model", "position"}, react::ConstructionAction::Create);
  add("engine.destroy",
      "Remove the selected object and its declared parts using Destroy permission.", "destroy",
      false, Json::object(), Json::array(), react::ConstructionAction::Destroy);
  add("engine.reconfigure", "Change the selected model binding using Edit permission.", "edit",
      true, {{"model", str}, {"params", params}, {"material", material}}, {"model"},
      react::ConstructionAction::Reconfigure);
}
} // namespace amber::step
