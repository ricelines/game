#include "catalog/catalog.hpp"
#include "editing_private.hpp"

namespace amber::step::editing {
namespace {
Json schema(Json properties, Json required, Json example) {
  return {{"type", "object"},
          {"properties", std::move(properties)},
          {"required", std::move(required)},
          {"additionalProperties", false},
          {"examples", Json::array({std::move(example)})}};
}
} // namespace
Declarations declare_editing(react::Terms &terms, react::NativeActions &native) {
  const Json str = {{"type", "string"}};
  const Json term = {{"type", {"string", "integer"}}, {"minimum", 0}, {"maximum", UINT32_MAX}};
  const Json plot = {{"minimum", 1}, {"maximum", UINT32_MAX}};
  const Json scalar_values = {{"type", "object"},
                              {"additionalProperties", {{"type", {"number", "boolean", "null"}}}}};
  const Json text = {{"type", "object"},
                     {"properties", {{"name", str}, {"description", str}, {"writing", str}}},
                     {"additionalProperties", false}};
  Json slot;
  slot["type"] = "object";
  slot["required"] = {"slot", "scope", "rights", "expires_s"};
  slot["additionalProperties"] = false;
  slot["properties"] = {
      {"slot", str},
      {"scope",
       {{"type", "object"},
        {"properties", {{"plot", plot}, {"owned_on_plot", {{"properties", {{"plot", plot}}}}}}}}},
      {"rights", {{"type", "array"}, {"items", str}}},
      {"cap", {{"type", "integer"}, {"minimum", 0}, {"maximum", INT64_MAX}}},
      {"period_s", {{"type", "number"}, {"minimum", 0}}},
      {"expires_s", {{"type", "number"}, {"minimum", 0}}}};
  auto add = [&](react::NativeOperation operation, const char *name, const char *description,
                 const char *right, Json inputs) {
    return catalog::declare_native(native, terms, name, description, right, false,
                                   std::move(inputs), operation);
  };
  Declarations ids;
  ids.install = add(
      react::ScriptAction::Install, "engine.install",
      "Install an uploaded ordinary script on the target. Omit choices to preview slots; provide "
      "choices (possibly []) to commit. Grants use ordinary permission checks.",
      "edit",
      schema({{"script", str}, {"choices", {{"type", "array"}, {"items", slot}}}}, {"script"},
             {{"script", "s:SCRIPT_ID"}, {"choices", Json::array()}}));
  ids.uninstall =
      add(react::ScriptAction::Uninstall, "engine.uninstall",
          "Remove an explicit instance script and its watches/grants. Model defaults are edited "
          "through engine.model_script.",
          "edit", schema({{"script", str}}, {"script"}, {{"script", "s:SCRIPT_ID"}}));
  ids.model_script = add(
      react::ScriptAction::ModelScript, "engine.model_script",
      "As the model author, replace its default ordinary script for current and future instances; "
      "null removes it.",
      "",
      schema({{"model", str}, {"script", {{"type", {"string", "null"}}}}}, {"model", "script"},
             {{"model", "m:MODEL_ID"}, {"script", "s:SCRIPT_ID"}}));
  ids.edit_text =
      add(react::WritingAction::EditText, "engine.edit_text",
          "Patch target maker claims. Empty strings clear a field; claims are "
          "never measured facts.",
          "edit", schema({{"text", text}}, {"text"}, {{"text", {{"name", "An authored name"}}}}));
  ids.model_text = add(
      react::WritingAction::ModelText, "engine.model_text",
      "As the model author, patch default maker claims.", "",
      schema({{"model", str}, {"text", text}}, {"model", "text"},
             {{"model", "m:MODEL_ID"}, {"text", {{"description", "An authored description"}}}}));
  ids.edit_detail =
      add(react::WritingAction::EditDetail, "engine.edit_detail",
          "Rewrite one elaborated detail on the current target under ordinary Edit permission. "
          "Preserve its original elaborator attribution and the maker's separate claims.",
          "edit",
          schema({{"id", {{"type", "integer"}, {"minimum", 1}, {"maximum", UINT32_MAX}}},
                  {"text", str}},
                 {"id", "text"}, {{"id", 1}, {"text", "An edited public detail"}}));
  ids.set_setting =
      add(react::ValueAction::SetSetting, "engine.set_setting",
          "Set target setting overrides by registered term; null removes an "
          "override. Intrinsic, measured and state terms are rejected.",
          "edit", schema({{"values", scalar_values}}, {"values"}, {{"values", Json::object()}}));
  ids.model_setting = add(react::ValueAction::ModelSetting, "engine.model_setting",
                          "As the model author, set default settings. Instance overrides "
                          "retain precedence; null removes a default.",
                          "",
                          schema({{"model", str}, {"values", scalar_values}}, {"model", "values"},
                                 {{"model", "m:MODEL_ID"}, {"values", Json::object()}}));
  ids.world_setting =
      add(react::ValueAction::WorldSetting, "engine.world_setting",
          "As an architect, change world settings by registered term; null removes a default. "
          "Object, model and plot overrides retain precedence.",
          "", schema({{"values", scalar_values}}, {"values"}, {{"values", Json::object()}}));
  const Json state_values = {{"type", "object"},
                             {"additionalProperties", {{"type", {"number", "boolean"}}}}};
  ids.patch_state =
      add(react::ValueAction::PatchState, "engine.patch_state",
          "Architect/elaborator repair of current target state, requiring "
          "current Edit permission. This is not ordinary player state authoring.",
          "edit", schema({{"values", state_values}}, {"values"}, {{"values", Json::object()}}));
  ids.relate = add(
      react::ValueAction::Relate, "engine.relate",
      "Edit the target's outgoing authored relation to a live object. "
      "Space-owned containment/support/attachment/zone relations cannot be edited.",
      "edit",
      schema({{"term", term}, {"other", str}, {"present", {{"type", "boolean"}}}},
             {"term", "other", "present"},
             {{"term", "REGISTERED_RELATION"}, {"other", "o:SLOT.GENERATION"}, {"present", true}}));
  Json proposal;
  proposal["type"] = "object";
  proposal["required"] = {"name", "kind"};
  proposal["properties"] = {
      {"name", str},
      {"kind", {{"enum", {"quantity", "flag", "event", "relation", "action", "material"}}}},
      {"source", {{"enum", {"intrinsic", "measured", "state", "setting"}}}},
      {"dimensions",
       {{"type", "array"}, {"minItems", 8}, {"maxItems", 8}, {"items", {{"type", "number"}}}}},
      {"conserved", {{"type", "boolean"}}},
      {"combine", {{"enum", {"none", "sum", "max", "any", "share_equal", "share_priority"}}}},
      {"along", term},
      {"paired", term},
      {"priority", term},
      {"description", str},
      {"inputs",
       {{"type", "array"}, {"items", {{"type", "object"}, {"properties", {{"term", term}}}}}}},
      {"measurement", {{"enum", {"baked", "volume", "mass", "runtime"}}}}};
  proposal["additionalProperties"] = false;
  ids.define_term = add(
      react::RegistryAction::DefineTerm, "engine.define_term",
      "Architect/elaborator vocabulary proposal. Supply a short name; server assigns your stable "
      "pN namespace. Omit choice to inspect duplicates, then choose same_as or explain none_fits.",
      "",
      schema({{"proposal", proposal},
              {"choice",
               {{"type", "object"},
                {"properties", {{"same_as", term}, {"none_fits", str}}},
                {"additionalProperties", false},
                {"oneOf", Json::array({Json{{"required", {"same_as"}}},
                                       Json{{"required", {"none_fits"}}}})}}}},
             {"proposal"},
             {{"proposal",
               {{"name", "new_action"},
                {"kind", "action"},
                {"description", "A distinct authored action"}}}}));
  ids.define_material =
      add(react::RegistryAction::DefineMaterial, "engine.define_material",
          "Architect/elaborator registry edit of a Material term's complete intrinsic scalar map. "
          "Geometry measurements cannot be declared here.",
          "",
          schema({{"material", term}, {"values", state_values}}, {"material", "values"},
                 {{"material", "pN.NEW_MATERIAL"}, {"values", Json::object()}}));
  return ids;
}
} // namespace amber::step::editing
