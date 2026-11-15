#include "common.hpp"

namespace amber::bindings::editing {
namespace {
void fields(const Json &row, std::initializer_list<std::string_view> required,
            std::initializer_list<std::string_view> optional = {}) {
  if (!row.is_object())
    throw std::invalid_argument("expected an input record");
  for (auto key : required)
    if (!row.contains(key))
      throw std::invalid_argument("missing input: " + std::string(key));
  for (const auto &[key, value] : row.items()) {
    (void)value;
    if (std::find(required.begin(), required.end(), key) == required.end() &&
        std::find(optional.begin(), optional.end(), key) == optional.end())
      throw std::invalid_argument("unknown input: " + key);
  }
}
void text(const Json &row) {
  fields(row, {}, {"name", "description", "writing"});
  for (const auto &[name, value] : row.items()) {
    (void)name;
    if (!value.is_string())
      throw std::invalid_argument("text fields must be strings");
  }
}
void string(const Json &row, std::string_view name) {
  if (!row.at(name).is_string())
    throw std::invalid_argument(std::string(name) + " must be a string");
}
void values(const Json &row, bool nullable) {
  if (!row.is_object())
    throw std::invalid_argument("values must be a term-to-scalar record");
  for (const auto &[name, value] : row.items()) {
    (void)name;
    if (value.is_boolean() || (nullable && value.is_null()))
      continue;
    if (!value.is_number() || !std::isfinite(value.get<double>()))
      throw std::invalid_argument("values must be finite quantities or booleans");
  }
}
template <class Fn> std::string validate(Fn fn) {
  try {
    fn();
    return {};
  } catch (const std::exception &error) {
    return error.what();
  }
}
Json schema(Json properties, Json required, Json example) {
  return {{"type", "object"},
          {"properties", std::move(properties)},
          {"required", std::move(required)},
          {"additionalProperties", false},
          {"examples", Json::array({std::move(example)})}};
}
} // namespace
std::string validate_install(const Json &row) {
  return validate([&] {
    fields(row, {"script"}, {"choices"});
    string(row, "script");
    if (row.contains("choices")) {
      if (!row["choices"].is_array())
        throw std::invalid_argument("choices must be an array");
      for (const auto &choice : row["choices"]) {
        fields(choice, {"slot", "scope", "rights", "expires_s"}, {"cap", "period_s"});
        string(choice, "slot");
        if (!choice["scope"].is_object() || choice["scope"].size() != 1 ||
            !choice["rights"].is_array())
          throw std::invalid_argument("slot needs one scope and a rights array");
        for (const auto &right : choice["rights"])
          if (!right.is_string())
            throw std::invalid_argument("rights must be strings");
        for (auto key : {"expires_s", "period_s"})
          if (choice.contains(key) &&
              (!choice[key].is_number() || !std::isfinite(choice[key].get<double>()) ||
               choice[key].get<double>() < 0))
            throw std::invalid_argument("grant durations must be finite and nonnegative");
        if (choice.contains("cap") &&
            (!choice["cap"].is_number_integer() || choice["cap"].get<std::int64_t>() < 0))
          throw std::invalid_argument("cap must be nonnegative whole money units");
      }
    }
  });
}
std::string validate_uninstall(const Json &row) {
  return validate([&] {
    fields(row, {"script"});
    string(row, "script");
  });
}
std::string validate_model_script(const Json &row) {
  return validate([&] {
    fields(row, {"model", "script"});
    string(row, "model");
    if (!row["script"].is_null())
      string(row, "script");
  });
}
std::string validate_edit_text(const Json &row) {
  return validate([&] {
    fields(row, {"text"});
    text(row["text"]);
  });
}
std::string validate_model_text(const Json &row) {
  return validate([&] {
    fields(row, {"model", "text"});
    string(row, "model");
    text(row["text"]);
  });
}
std::string validate_edit_detail(const Json &row) {
  return validate([&] {
    fields(row, {"id", "text"});
    string(row, "text");
    if (!row["id"].is_number_integer() || row["id"].get<std::int64_t>() <= 0 ||
        row["id"].get<std::uint64_t>() > UINT32_MAX)
      throw std::invalid_argument("id must name a positive elaborated text ID");
  });
}
std::string validate_set_setting(const Json &row) {
  return validate([&] {
    fields(row, {"values"});
    values(row["values"], true);
  });
}
std::string validate_model_setting(const Json &row) {
  return validate([&] {
    fields(row, {"model", "values"});
    string(row, "model");
    values(row["values"], true);
  });
}
std::string validate_patch_state(const Json &row) {
  return validate([&] {
    fields(row, {"values"});
    values(row["values"], false);
  });
}
std::string validate_relate(const Json &row) {
  return validate([&] {
    fields(row, {"term", "other", "present"});
    if (!row["term"].is_string() && !row["term"].is_number_integer())
      throw std::invalid_argument("term must be a registered name or ID");
    string(row, "other");
    if (!row["present"].is_boolean())
      throw std::invalid_argument("present must be boolean");
  });
}
std::string validate_define_term(const Json &row) {
  return validate([&] {
    fields(row, {"proposal"}, {"choice"});
    const auto &proposal = row["proposal"];
    fields(proposal, {"name", "kind"},
           {"source", "dimensions", "conserved", "combine", "along", "paired", "priority",
            "description", "inputs", "measurement"});
    string(proposal, "name");
    string(proposal, "kind");
    if (row.contains("choice")) {
      fields(row["choice"], {}, {"same_as", "none_fits"});
      if (row["choice"].size() != 1)
        throw std::invalid_argument("choose same_as or none_fits");
      if (row["choice"].contains("none_fits")) {
        string(row["choice"], "none_fits");
        if (row["choice"]["none_fits"].get_ref<const std::string &>().empty())
          throw std::invalid_argument("none_fits needs a distinction");
      }
    }
  });
}
std::string validate_define_material(const Json &row) {
  return validate([&] {
    fields(row, {"material", "values"});
    if (!row["material"].is_string() && !row["material"].is_number_integer())
      throw std::invalid_argument("material must be a registered name or ID");
    values(row["values"], false);
  });
}
Declarations declare_editing(react::Terms &terms, command::NativeActions &native) {
  const Json str = {{"type", "string"}};
  const Json scalar_values = {{"type", "object"},
                              {"additionalProperties", {{"type", {"number", "boolean", "null"}}}}};
  const Json text = {{"type", "object"},
                     {"properties", {{"name", str}, {"description", str}, {"writing", str}}},
                     {"additionalProperties", false}};
  Json slot;
  slot["type"] = "object";
  slot["required"] = {"slot", "scope", "rights", "expires_s"};
  slot["additionalProperties"] = false;
  slot["properties"] = {{"slot", str},
                        {"scope", {{"type", "object"}}},
                        {"rights", {{"type", "array"}, {"items", str}}},
                        {"cap", {{"type", "integer"}, {"minimum", 0}}},
                        {"period_s", {{"type", "number"}, {"minimum", 0}}},
                        {"expires_s", {{"type", "number"}, {"exclusiveMinimum", 0}}}};
  auto add = [&](const char *name, const char *description, const char *right, Json inputs) {
    return command::declare_native(native, terms, name, description, right, false,
                                   std::move(inputs));
  };
  Declarations ids;
  ids.install = add(
      "engine.install",
      "Install an uploaded ordinary script on the target. Omit choices to preview slots; provide "
      "choices (possibly []) to commit. Grants use ordinary permission checks.",
      "edit",
      schema({{"script", str}, {"choices", {{"type", "array"}, {"items", slot}}}}, {"script"},
             {{"script", "s:SCRIPT_ID"}, {"choices", Json::array()}}));
  ids.uninstall =
      add("engine.uninstall",
          "Remove an explicit instance script and its watches/grants. Model defaults are edited "
          "through engine.model_script.",
          "edit", schema({{"script", str}}, {"script"}, {{"script", "s:SCRIPT_ID"}}));
  ids.model_script = add(
      "engine.model_script",
      "As the model author, replace its default ordinary script for current and future instances; "
      "null removes it.",
      "",
      schema({{"model", str}, {"script", {{"type", {"string", "null"}}}}}, {"model", "script"},
             {{"model", "m:MODEL_ID"}, {"script", "s:SCRIPT_ID"}}));
  ids.edit_text =
      add("engine.edit_text",
          "Patch target maker claims. Empty strings clear a field; claims are "
          "never measured facts.",
          "edit", schema({{"text", text}}, {"text"}, {{"text", {{"name", "An authored name"}}}}));
  ids.model_text = add(
      "engine.model_text", "As the model author, patch default maker claims.", "",
      schema({{"model", str}, {"text", text}}, {"model", "text"},
             {{"model", "m:MODEL_ID"}, {"text", {{"description", "An authored description"}}}}));
  ids.edit_detail =
      add("engine.edit_detail",
          "Rewrite one elaborated detail on the current target under ordinary Edit permission. "
          "Preserve its original elaborator attribution and the maker's separate claims.",
          "edit",
          schema({{"id", {{"type", "integer"}, {"minimum", 1}}}, {"text", str}}, {"id", "text"},
                 {{"id", 1}, {"text", "An edited public detail"}}));
  ids.set_setting =
      add("engine.set_setting",
          "Set target setting overrides by registered term; null removes an "
          "override. Intrinsic, measured and state terms are rejected.",
          "edit", schema({{"values", scalar_values}}, {"values"}, {{"values", Json::object()}}));
  ids.model_setting = add("engine.model_setting",
                          "As the model author, set default settings. Instance overrides "
                          "retain precedence; null removes a default.",
                          "",
                          schema({{"model", str}, {"values", scalar_values}}, {"model", "values"},
                                 {{"model", "m:MODEL_ID"}, {"values", Json::object()}}));
  const Json state_values = {{"type", "object"},
                             {"additionalProperties", {{"type", {"number", "boolean"}}}}};
  ids.patch_state =
      add("engine.patch_state",
          "Architect/elaborator repair of current target state, requiring "
          "current Edit permission. This is not ordinary player state authoring.",
          "edit", schema({{"values", state_values}}, {"values"}, {{"values", Json::object()}}));
  ids.relate = add(
      "engine.relate",
      "Edit the target's outgoing authored relation to a live object. "
      "Space-owned containment/support/attachment/zone relations cannot be edited.",
      "edit",
      schema({{"term", {{"type", {"string", "integer"}}}},
              {"other", str},
              {"present", {{"type", "boolean"}}}},
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
       {{"type", "array"},
        {"minItems", 8},
        {"maxItems", 8},
        {"items", {{"type", "integer"}, {"minimum", -128}, {"maximum", 127}}}}},
      {"conserved", {{"type", "boolean"}}},
      {"combine", {{"enum", {"none", "sum", "max", "any", "share_equal", "share_priority"}}}},
      {"along", {{"type", {"string", "integer"}}}},
      {"paired", {{"type", {"string", "integer"}}}},
      {"priority", {{"type", {"string", "integer"}}}},
      {"description", str},
      {"inputs", {{"type", "array"}, {"items", {{"type", "object"}}}}},
      {"measurement", {{"enum", {"baked", "volume", "mass", "runtime"}}}}};
  proposal["additionalProperties"] = false;
  ids.define_term = add(
      "engine.define_term",
      "Architect/elaborator vocabulary proposal. Supply a short name; server assigns your stable "
      "pN namespace. Omit choice to inspect duplicates, then choose same_as or explain none_fits.",
      "",
      schema(
          {{"proposal", proposal},
           {"choice",
            {{"type", "object"},
             {"properties", {{"same_as", {{"type", {"string", "integer"}}}}, {"none_fits", str}}},
             {"additionalProperties", false}}}},
          {"proposal"},
          {{"proposal",
            {{"name", "new_action"},
             {"kind", "action"},
             {"description", "A distinct authored action"}}}}));
  ids.define_material =
      add("engine.define_material",
          "Architect/elaborator registry edit of a Material term's complete intrinsic scalar map. "
          "Geometry measurements cannot be declared here.",
          "",
          schema({{"material", {{"type", {"string", "integer"}}}}, {"values", state_values}},
                 {"material", "values"},
                 {{"material", "pN.NEW_MATERIAL"}, {"values", Json::object()}}));
  return ids;
}
} // namespace amber::bindings::editing
