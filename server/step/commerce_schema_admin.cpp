#include "catalog/catalog.hpp"
#include "step.hpp"

namespace amber::step {
namespace {
Json schema(Json properties, Json required, Json example) {
  return {{"type", "object"},
          {"properties", std::move(properties)},
          {"required", std::move(required)},
          {"additionalProperties", false},
          {"examples", Json::array({std::move(example)})}};
}
} // namespace
AdminDeclarations declare_admin(react::Terms &terms, react::NativeActions &native) {
  const Json str = {{"type", "string"}};
  const Json boolean = {{"type", "boolean"}};
  const Json rights = {{"type", "array"}, {"items", str}};
  const Json group = {{"type", {"string", "null"}},
                      {"description", "group:N; null clears the assigned group"}};
  const Json plot = {{"type", "integer"}, {"minimum", 1}, {"maximum", UINT32_MAX}};
  Json scope = schema({{"object", str},
                       {"owned", str},
                       {"owned_on_plot", schema({{"owner", str}, {"plot", plot}}, {"owner", "plot"},
                                                {{"owner", "p:PLAYER_ID"}, {"plot", 1}})},
                       {"all_land", {{"enum", {true}}}},
                       {"account", str},
                       {"avatar", str},
                       {"plot", plot}},
                      Json::array(), {{"object", "o:SLOT.GENERATION"}});
  scope["oneOf"] = Json::array();
  for (const auto *name :
       {"object", "owned", "owned_on_plot", "all_land", "account", "avatar", "plot"})
    scope["oneOf"].push_back(Json{{"required", {name}}});
  const Json holder = {
      {"oneOf",
       Json::array({str, schema({{"script", str}, {"on", str}}, {"script", "on"},
                                {{"script", "s:SCRIPT_ID"}, {"on", "o:SLOT.GENERATION"}})})},
      {"description", "p:N or an installed script with on:'world' or on:'o:SLOT.GENERATION'"}};
  auto add = [&](react::NativeOperation operation, const char *name, const char *description,
                 Json inputs) {
    return catalog::declare_native(native, terms, name, description, "", false, std::move(inputs),
                                   operation);
  };
  AdminDeclarations ids;
  auto grant_rights = rights;
  grant_rights["minItems"] = 1;
  ids.grant =
      add(react::PermissionAction::Grant, "engine.grant",
          "Grant current rights to a player or installed script. Spending and script-held or "
          "script-issued grants require future expiry; spending also requires a period. "
          "Delegation shares its parent's allowance.",
          schema({{"holder", holder},
                  {"scope", scope},
                  {"rights", grant_rights},
                  {"cap", {{"type", "integer"}, {"minimum", 0}, {"maximum", INT64_MAX}}},
                  {"period_s", {{"type", "number"}, {"minimum", 0}}},
                  {"expires_s", {{"type", "number"}, {"minimum", 0}}}},
                 {"holder", "scope", "rights"},
                 {{"holder", "p:PLAYER_ID"},
                  {"scope", {{"object", "o:SLOT.GENERATION"}}},
                  {"rights", {"use"}},
                  {"expires_s", 3600}}));
  auto revoke = schema({{"grant", str}, {"holder", holder}, {"scope", scope}}, Json::array(),
                       {{"grant", "g:GRANT_ID"}});
  revoke["oneOf"] =
      Json::array({Json{{"required", {"grant"}},
                        {"properties", {{"grant", Json::object()}}},
                        {"additionalProperties", false}},
                   Json{{"required", {"holder"}},
                        {"properties", {{"holder", Json::object()}, {"scope", Json::object()}}},
                        {"additionalProperties", false}}});
  ids.revoke =
      add(react::PermissionAction::Revoke, "engine.revoke",
          "Revoke one issued grant or issued grants to a holder, optionally on one exact scope. "
          "Scripts may revoke their issued children or relinquish their own held grants. "
          "Architects may revoke any grant. Delegated children are revoked too.",
          std::move(revoke));
  ids.list_grants = add(react::PermissionAction::ListGrants, "engine.list_grants",
                        "Read your held and given grants, remaining caps and expiry; scripts see "
                        "their exact held grants and issued children.",
                        schema(Json::object(), Json::array(), Json::object()));
  ids.preset = add(
      react::PermissionAction::Preset, "engine.preset",
      "As current goods owner, patch everyone/group/next-owner rights, group and no-copy. "
      "Inherited Edit/Copy/Transfer restrictions cannot be widened. Escrow goods cannot be edited.",
      schema({{"everyone", rights},
              {"group_rights", rights},
              {"next_owner", rights},
              {"group", group},
              {"no_copy", boolean}},
             Json::array(), {{"everyone", {"use"}}}));
  ids.group_create = add(react::PermissionAction::GroupCreate, "engine.group_create",
                         "Create a named group controlled by you and join it.",
                         schema({{"name", str}}, {"name"}, {{"name", "Workshop members"}}));
  ids.group_member = add(
      react::PermissionAction::GroupMember, "engine.group_member",
      "As group controller, add/remove a player; any member may leave themself.",
      schema({{"group", str}, {"player", str}, {"member", boolean}}, {"group", "player", "member"},
             {{"group", "group:GROUP_ID"}, {"player", "p:PLAYER_ID"}, {"member", true}}));
  ids.plot_permissions =
      add(react::PermissionAction::PlotPermissions, "engine.plot_permissions",
          "As current plot owner, patch build/enter/run_scripts rights, assigned group and player "
          "bans.",
          schema({{"plot", plot},
                  {"everyone", rights},
                  {"group_rights", rights},
                  {"group", group},
                  {"banned", {{"type", "array"}, {"items", str}}}},
                 {"plot"}, {{"plot", 1}, {"everyone", {"enter"}}}));
  native.rows.at(ids.plot_permissions).space = "land";
  return ids;
}
} // namespace amber::step
