#include "catalog/catalog.hpp"
#include "elaboration_private.hpp"

#include <algorithm>

namespace amber::step {
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
void string(const Json &row, std::string_view key) {
  if (!row.at(key).is_string())
    throw std::invalid_argument(std::string(key) + " must be a string");
}
template <class Fn> std::string validate(Fn fn) {
  try {
    fn();
    return {};
  } catch (const std::exception &error) {
    return error.what();
  }
}
Json schema(Json props, Json required, Json example) {
  return {{"type", "object"},
          {"properties", std::move(props)},
          {"required", std::move(required)},
          {"additionalProperties", false},
          {"examples", Json::array({std::move(example)})}};
}
} // namespace
std::string validate_request_model(const Json &row) {
  return validate([&] {
    fields(row, {"note"});
    string(row, "note");
  });
}
std::string validate_requests(const Json &row) {
  return validate([&] {
    fields(row, {}, {"page", "page_size"});
    for (const auto &[key, value] : row.items())
      if (!value.is_number_integer() || value.get<double>() < (key == "page" ? 0 : 1) ||
          value.get<double>() > UINT32_MAX)
        throw std::invalid_argument(
            "page must be nonnegative and page_size positive integral values");
  });
}
std::string validate_claim(const Json &row) {
  return validate([&] {
    fields(row, {"request"});
    string(row, "request");
  });
}
std::string validate_answer_rule(const Json &row) {
  return validate([&] {
    fields(row, {"request", "kind", "script"}, {"subjects"});
    string(row, "request");
    string(row, "script");
    if (row.at("kind") != "rule")
      throw std::invalid_argument("rule answer kind required");
    if (row.contains("subjects")) {
      if (!row.at("subjects").is_object())
        throw std::invalid_argument("subjects must map names to full handles");
      for (const auto &[name, value] : row.at("subjects").items())
        if (name.empty() || name == "target" || name == "body" || !value.is_string())
          throw std::invalid_argument(
              "subject aliases must be strings; target and body are supplied by the server");
    }
  });
}
std::string validate_answer_patch(const Json &row) {
  return validate([&] {
    fields(row, {"request", "kind", "note"});
    string(row, "request");
    string(row, "note");
    if (row.at("kind") != "patch")
      throw std::invalid_argument("patch answer kind required");
  });
}
std::string validate_answer_detail(const Json &row) {
  return validate([&] {
    fields(row, {"request", "kind", "text"});
    string(row, "request");
    string(row, "text");
    if (row.at("kind") != "detail")
      throw std::invalid_argument("detail answer kind required");
  });
}
std::string validate_answer(const Json &row) {
  if (!row.is_object() || !row.contains("kind") || !row.at("kind").is_string())
    return "answer kind required";
  const auto kind = row.at("kind").get<std::string>();
  if (kind == "rule")
    return validate_answer_rule(row);
  if (kind == "patch")
    return validate_answer_patch(row);
  if (kind == "detail")
    return validate_answer_detail(row);
  return "answer kind must be rule, patch or detail";
}
std::string validate_refuse(const Json &row) {
  return validate([&] {
    fields(row, {"request", "text"});
    string(row, "request");
    string(row, "text");
  });
}
std::string validate_rule_activation(const Json &row) {
  return validate([&] {
    fields(row, {"script", "active"});
    string(row, "script");
    if (!row.at("active").is_boolean())
      throw std::invalid_argument("active must be boolean");
  });
}
ElaborationDeclarations declare_elaboration(react::Terms &terms, react::NativeActions &native) {
  ElaborationDeclarations ids;
  const Json str = {{"type", "string"}},
             positive = {{"type", "integer"}, {"minimum", 1}, {"maximum", UINT32_MAX}};
  auto add = [&](react::NativeOperation operation, std::string name, std::string description,
                 Json schema) {
    return catalog::declare_native(native, terms, std::move(name), std::move(description), "",
                                   false, std::move(schema), operation);
  };
  ids.request_model =
      add(react::ElaborationAction::RequestModel, "engine.request_model",
          "Request modeling of your actual last not-modeled result, retaining its original target "
          "and capture.",
          schema({{"note", str}}, {"note"}, {{"note", "Describe the missing behavior"}}));
  ids.requests =
      add(react::ElaborationAction::Requests, "engine.requests",
          "Elaborator list of unresolved requests: explicit first, then shared gaps by occurrence "
          "count. Page size can be raised.",
          schema({{"page", {{"type", "integer"}, {"minimum", 0}, {"maximum", UINT32_MAX}}},
                  {"page_size", positive}},
                 Json::array(), Json::object()));
  ids.claim = add(react::ElaborationAction::Claim, "engine.claim",
                  "Claim an unresolved request as the current elaborator. Another elaborator's "
                  "claim cannot be taken.",
                  schema({{"request", str}}, {"request"}, {{"request", "request:REQUEST_ID"}}));
  Json answer =
      schema({{"request", str},
              {"kind", {{"enum", {"rule", "patch", "detail"}}}},
              {"script", str},
              {"subjects", {{"type", "object"}, {"additionalProperties", str}}},
              {"note", str},
              {"text", str}},
             {"request", "kind"},
             {{"request", "request:REQUEST_ID"}, {"kind", "rule"}, {"script", "s:SCRIPT_ID"}});
  answer["oneOf"] = Json::array(
      {Json{{"properties", {{"kind", {{"const", "rule"}}}}}, {"required", {"script"}}},
       Json{{"properties", {{"kind", {{"const", "patch"}}}}}, {"required", {"note"}}},
       Json{{"properties", {{"kind", {{"const", "detail"}}}}}, {"required", {"text"}}}});
  ids.answer =
      add(react::ElaborationAction::Answer, "engine.answer",
          "Answer an elaboration request. Rule answers run only the immutable script's verified "
          "tests in real with/without fixtures; the issuing session awaits completion. Patch "
          "documents already applied authorized changes; detail appends attributed text.",
          std::move(answer));
  ids.refuse = add(
      react::ElaborationAction::Refuse, "engine.refuse",
      "Close an unresolved request with attributed refusal text and notify its requester.",
      schema({{"request", str}, {"text", str}}, {"request", "text"},
             {{"request", "request:REQUEST_ID"}, {"text", "Explain why this request is refused"}}));
  ids.rule_activation =
      add(react::ElaborationAction::RuleActivation, "engine.rule_activation",
          "Elaborator/architect enable or disable an already fixture-approved immutable world "
          "rule. Enabling a historical version restores it through normal registry history; this "
          "cannot approve new code.",
          schema({{"script", str}, {"active", {{"type", "boolean"}}}}, {"script", "active"},
                 {{"script", "s:SCRIPT_ID"}, {"active", false}}));
  return ids;
}
} // namespace amber::step
