#include "catalog/catalog.hpp"
#include "elaboration_private.hpp"

namespace amber::step {
namespace {
Json schema(Json props, Json required, Json example) {
  return {{"type", "object"},
          {"properties", std::move(props)},
          {"required", std::move(required)},
          {"additionalProperties", false},
          {"examples", Json::array({std::move(example)})}};
}
} // namespace
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
  answer["oneOf"] = Json::array({Json{{"properties",
                                       {{"request", Json::object()},
                                        {"kind", {{"enum", {"rule"}}}},
                                        {"script", Json::object()},
                                        {"subjects", Json::object()}}},
                                      {"required", {"script"}},
                                      {"additionalProperties", false}},
                                 Json{{"properties",
                                       {{"request", Json::object()},
                                        {"kind", {{"enum", {"patch"}}}},
                                        {"note", Json::object()}}},
                                      {"required", {"note"}},
                                      {"additionalProperties", false}},
                                 Json{{"properties",
                                       {{"request", Json::object()},
                                        {"kind", {{"enum", {"detail"}}}},
                                        {"text", Json::object()}}},
                                      {"required", {"text"}},
                                      {"additionalProperties", false}}});
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
