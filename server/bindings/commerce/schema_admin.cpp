#include "admin.hpp"

#include <algorithm>
#include <cmath>

namespace amber::bindings::commerce {
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
    authority::identifier(value, "p:");
    return;
  }
  fields(value, {"script", "on"});
  authority::identifier(value.at("script"), "s:");
  string(value.at("on"));
  if (!parse_handle(value.at("on").get<std::string>()))
    throw std::invalid_argument("script holder needs a full object handle");
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
    authority::identifier(row.at("group"), "g:");
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
    const auto &scope = row.at("scope");
    fields(scope, {}, {"object", "owned_on_plot", "account", "avatar", "plot", "issuance"});
    if (scope.size() != 1)
      throw std::invalid_argument("choose one scope");
    if (scope.contains("owned_on_plot")) {
      fields(scope.at("owned_on_plot"), {"owner", "plot"});
      authority::identifier(scope.at("owned_on_plot").at("owner"), "p:");
      if (!scope.at("owned_on_plot").at("plot").is_number_unsigned() &&
          !scope.at("owned_on_plot").at("plot").is_number_integer())
        throw std::invalid_argument("plot must be a positive ID");
      if (scope.at("owned_on_plot").at("plot").get<std::int64_t>() <= 0)
        throw std::invalid_argument("plot must be a positive ID");
    } else if (scope.contains("plot")) {
      if (!scope.at("plot").is_number_integer() || scope.at("plot").get<std::int64_t>() <= 0)
        throw std::invalid_argument("plot must be a positive ID");
    } else if (scope.contains("issuance")) {
      if (scope.at("issuance") != true)
        throw std::invalid_argument("issuance must be true");
    } else
      string(scope.begin().value());
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
    fields(row, {}, {"grant", "holder"});
    if (row.size() != 1)
      throw std::invalid_argument("choose one grant or holder");
    if (row.contains("grant"))
      authority::identifier(row.at("grant"), "grant:");
    else
      holder(row.at("holder"));
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
    authority::identifier(row.at("group"), "g:");
    authority::identifier(row.at("player"), "p:");
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
        authority::identifier(item, "p:");
    }
  });
}
AdminDeclarations declare_admin(react::Terms &terms, command::NativeActions &native) {
  const Json str = {{"type", "string"}};
  const Json boolean = {{"type", "boolean"}};
  const Json rights = {{"type", "array"}, {"items", str}, {"uniqueItems", true}};
  const Json group = {{"type", {"string", "null"}},
                      {"description", "g:N; null clears the assigned group"}};
  const Json holder = {
      {"oneOf",
       Json::array({str, schema({{"script", str}, {"on", str}}, {"script", "on"},
                                {{"script", "s:SCRIPT_ID"}, {"on", "o:SLOT.GENERATION"}})})},
      {"description", "p:N or an actually installed {script:'s:N',on:'o:SLOT.GENERATION'}"}};
  auto add = [&](const char *name, const char *description, Json inputs) {
    return command::declare_native(native, terms, name, description, "", false, std::move(inputs));
  };
  AdminDeclarations ids;
  ids.grant = add(
      "engine.grant",
      "Grant current rights to a player or installed script. Spending and script holders require "
      "future expiry; spending also requires a period. Delegation shares its parent's allowance.",
      schema({{"holder", holder},
              {"scope",
               {{"type", "object"},
                {"description", "Exactly one of object, owned_on_plot:{owner,plot}, "
                                "account:p:N, avatar, plot or issuance:true"}}},
              {"rights", rights},
              {"cap", {{"type", "integer"}, {"minimum", 0}}},
              {"period_s", {{"type", "number"}, {"minimum", 0}}},
              {"expires_s", {{"type", "number"}, {"minimum", 0}}}},
             {"holder", "scope", "rights"},
             {{"holder", "p:PLAYER_ID"},
              {"scope", {{"object", "o:SLOT.GENERATION"}}},
              {"rights", {"use"}},
              {"expires_s", 3600}}));
  ids.revoke = add(
      "engine.revoke",
      "Revoke one issued grant (architects may revoke any) or all grants you issued to a holder. "
      "Delegated children are revoked too.",
      schema({{"grant", str}, {"holder", holder}}, Json::array(), {{"grant", "grant:GRANT_ID"}}));
  ids.list_grants = add("engine.list_grants",
                        "Read your indexed held and given grants, remaining caps and expiry.",
                        schema(Json::object(), Json::array(), Json::object()));
  ids.preset = add(
      "engine.preset",
      "As current goods owner, patch everyone/group/next-owner rights, group and no-copy. "
      "Inherited Edit/Copy/Transfer restrictions cannot be widened. Escrow goods cannot be edited.",
      schema({{"everyone", rights},
              {"group_rights", rights},
              {"next_owner", rights},
              {"group", group},
              {"no_copy", boolean}},
             Json::array(), {{"everyone", {"use"}}}));
  ids.group_create =
      add("engine.group_create", "Create a named group controlled by you and join it.",
          schema({{"name", str}}, {"name"}, {{"name", "Workshop members"}}));
  ids.group_member = add(
      "engine.group_member",
      "As group controller, add/remove a player; any member may leave themself.",
      schema({{"group", str}, {"player", str}, {"member", boolean}}, {"group", "player", "member"},
             {{"group", "g:GROUP_ID"}, {"player", "p:PLAYER_ID"}, {"member", true}}));
  ids.plot_permissions =
      add("engine.plot_permissions",
          "As current plot owner, patch build/enter/run_scripts rights, assigned group and player "
          "bans.",
          schema({{"plot", {{"type", "integer"}, {"minimum", 1}}},
                  {"everyone", rights},
                  {"group_rights", rights},
                  {"group", group},
                  {"banned", {{"type", "array"}, {"items", str}, {"uniqueItems", true}}}},
                 {"plot"}, {{"plot", 1}, {"everyone", {"enter"}}}));
  return ids;
}
} // namespace amber::bindings::commerce
