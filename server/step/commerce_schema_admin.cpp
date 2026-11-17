#include "catalog/catalog.hpp"
#include "step.hpp"

#include <algorithm>
#include <cmath>

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
void string(const Json &value) {
  if (!value.is_string())
    throw std::invalid_argument("expected an identifier string");
}
void holder(const Json &value) {
  if (value.is_string()) {
    step::identifier(value, "p:");
    return;
  }
  fields(value, {"script", "on"});
  step::identifier(value.at("script"), "s:");
  string(value.at("on"));
  if (value.at("on") != "world" && !parse_handle(value.at("on").get<std::string>()))
    throw std::invalid_argument("script holder needs world or a full object handle");
}
void scope(const Json &scope) {
  fields(scope, {}, {"object", "owned_on_plot", "owned", "all_land", "account", "avatar", "plot"});
  if (scope.size() != 1)
    throw std::invalid_argument("choose one scope");
  if (scope.contains("owned")) {
    step::identifier(scope.at("owned"), "p:");
  } else if (scope.contains("all_land")) {
    if (scope.at("all_land") != true)
      throw std::invalid_argument("all_land must be true");
  } else if (scope.contains("owned_on_plot")) {
    fields(scope.at("owned_on_plot"), {"owner", "plot"});
    step::identifier(scope.at("owned_on_plot").at("owner"), "p:");
    if (!scope.at("owned_on_plot").at("plot").is_number_unsigned() &&
        !scope.at("owned_on_plot").at("plot").is_number_integer())
      throw std::invalid_argument("plot must be a positive ID");
    if (scope.at("owned_on_plot").at("plot").get<std::int64_t>() <= 0)
      throw std::invalid_argument("plot must be a positive ID");
  } else if (scope.contains("plot")) {
    if (!scope.at("plot").is_number_integer() || scope.at("plot").get<std::int64_t>() <= 0)
      throw std::invalid_argument("plot must be a positive ID");
  } else
    string(scope.begin().value());
}
void strings(const Json &value) {
  if (!value.is_array())
    throw std::invalid_argument("rights must be an array");
  for (const auto &item : value)
    string(item);
}
void preset_fields(const Json &row, bool plot) {
  for (const auto *key : {"everyone", "group_rights", "next_owner"})
    if (row.contains(key))
      strings(row.at(key));
  if (row.contains("group") && !row.at("group").is_null())
    step::identifier(row.at("group"), "group:");
  if (!plot && row.contains("no_copy") && !row.at("no_copy").is_boolean())
    throw std::invalid_argument("no_copy must be boolean");
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
std::string validate_grant(const Json &row) {
  return validate([&] {
    fields(row, {"holder", "scope", "rights"}, {"cap", "period_s", "expires_s"});
    holder(row.at("holder"));
    scope(row.at("scope"));
    strings(row.at("rights"));
    if (row.at("rights").empty())
      throw std::invalid_argument("grant needs at least one right");
    if (row.contains("cap") &&
        (!row.at("cap").is_number_integer() || row.at("cap").get<std::int64_t>() < 0))
      throw std::invalid_argument("cap must be nonnegative whole money units");
    for (const auto *key : {"period_s", "expires_s"})
      if (row.contains(key) &&
          (!row.at(key).is_number() || !std::isfinite(row.at(key).get<double>()) ||
           row.at(key).get<double>() < 0))
        throw std::invalid_argument("grant durations must be finite and nonnegative");
  });
}
std::string validate_revoke(const Json &row) {
  return validate([&] {
    fields(row, {}, {"grant", "holder", "scope"});
    if (row.contains("grant")) {
      if (row.size() != 1)
        throw std::invalid_argument("choose one grant or holder with optional scope");
      step::identifier(row.at("grant"), "g:");
    } else {
      if (!row.contains("holder"))
        throw std::invalid_argument("revoke needs a grant or holder");
      holder(row.at("holder"));
      if (row.contains("scope"))
        scope(row.at("scope"));
    }
  });
}
std::string validate_list_grants(const Json &row) {
  return validate([&] { fields(row, {}); });
}
std::string validate_preset(const Json &row) {
  return validate([&] {
    fields(row, {}, {"everyone", "group_rights", "next_owner", "group", "no_copy"});
    preset_fields(row, false);
  });
}
std::string validate_group_create(const Json &row) {
  return validate([&] {
    fields(row, {"name"});
    string(row.at("name"));
    const auto &name = row.at("name").get_ref<const std::string &>();
    if (name.find_first_not_of(" \t\r\n") == std::string::npos)
      throw std::invalid_argument("group name must not be empty");
  });
}
std::string validate_group_member(const Json &row) {
  return validate([&] {
    fields(row, {"group", "player", "member"});
    step::identifier(row.at("group"), "group:");
    step::identifier(row.at("player"), "p:");
    if (!row.at("member").is_boolean())
      throw std::invalid_argument("member must be boolean");
  });
}
std::string validate_plot_permissions(const Json &row) {
  return validate([&] {
    fields(row, {"plot"}, {"everyone", "group_rights", "group", "banned"});
    if (!row.at("plot").is_number_integer() || row.at("plot").get<std::int64_t>() <= 0)
      throw std::invalid_argument("plot must be a positive ID");
    preset_fields(row, true);
    if (row.contains("banned")) {
      if (!row.at("banned").is_array())
        throw std::invalid_argument("banned must be an array of player IDs");
      for (const auto &item : row.at("banned"))
        step::identifier(item, "p:");
    }
  });
}
AdminDeclarations declare_admin(react::Terms &terms, react::NativeActions &native) {
  const Json str = {{"type", "string"}};
  const Json boolean = {{"type", "boolean"}};
  const Json rights = {{"type", "array"}, {"items", str}, {"uniqueItems", true}};
  const Json group = {{"type", {"string", "null"}},
                      {"description", "group:N; null clears the assigned group"}};
  const Json scope = {{"type", "object"},
                      {"description", "Exactly one of object, owned_on_plot:{owner,plot}, "
                                      "account:p:N, avatar or plot"}};
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
  ids.grant =
      add(react::PermissionAction::Grant, "engine.grant",
          "Grant current rights to a player or installed script. Spending and script-held or "
          "script-issued grants require future expiry; spending also requires a period. "
          "Delegation shares its parent's allowance.",
          schema({{"holder", holder},
                  {"scope", scope},
                  {"rights", rights},
                  {"cap", {{"type", "integer"}, {"minimum", 0}}},
                  {"period_s", {{"type", "number"}, {"minimum", 0}}},
                  {"expires_s", {{"type", "number"}, {"minimum", 0}}}},
                 {"holder", "scope", "rights"},
                 {{"holder", "p:PLAYER_ID"},
                  {"scope", {{"object", "o:SLOT.GENERATION"}}},
                  {"rights", {"use"}},
                  {"expires_s", 3600}}));
  ids.revoke =
      add(react::PermissionAction::Revoke, "engine.revoke",
          "Revoke one issued grant or issued grants to a holder, optionally on one exact scope. "
          "Scripts may revoke their issued children or relinquish their own held grants. "
          "Architects may revoke any grant. Delegated children are revoked too.",
          schema({{"grant", str}, {"holder", holder}, {"scope", scope}}, Json::array(),
                 {{"grant", "g:GRANT_ID"}}));
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
          schema({{"plot", {{"type", "integer"}, {"minimum", 1}}},
                  {"everyone", rights},
                  {"group_rights", rights},
                  {"group", group},
                  {"banned", {{"type", "array"}, {"items", str}, {"uniqueItems", true}}}},
                 {"plot"}, {{"plot", 1}, {"everyone", {"enter"}}}));
  native.rows.at(ids.plot_permissions).space = "land";
  return ids;
}
} // namespace amber::step
