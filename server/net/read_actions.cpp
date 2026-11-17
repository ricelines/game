#include "catalog/catalog.hpp"
#include "settings.hpp"
#include "view_private.hpp"

#include <map>

namespace amber::net {
namespace {
Json input_record(const react::Terms &terms, const react::Input &input) {
  constexpr const char *names[]{"number", "integer", "boolean", "handle",
                                "vector", "string",  "term"};
  Json result = {{"name", input.name},
                 {"type", names[static_cast<unsigned>(input.type)]},
                 {"optional", input.optional}};
  if (input.term)
    result["quantity"] = {{"term", terms.rows.at(input.term).name},
                          {"dimensions", terms.rows.at(input.term).dimensions},
                          {"unit", react::format_unit(terms.rows.at(input.term).dimensions)}};
  return result;
}
Json example_value(react::InputType type) {
  switch (type) {
  case react::InputType::Number:
    return 1.0;
  case react::InputType::Integer:
    return 1;
  case react::InputType::Boolean:
    return true;
  case react::InputType::Handle:
    return "o:SLOT.GENERATION";
  case react::InputType::Vector:
    return Json::array({0, 0, 0});
  case react::InputType::String:
    return "text";
  case react::InputType::Term:
    return "REGISTERED_TERM_NAME";
  }
  return nullptr;
}
Json action_record(const catalog::State &catalog_state, const react::State &react_state,
                   IdentityRead identity, const net::Invocation &call, Handle target, TermId id,
                   const Json *plot) {
  const auto &native = react_state.native_declarations;
  const auto &terms = react_state.terms;
  const auto &term = terms.rows.at(id);
  const auto declaration = native.rows.find(id);
  const ActionRequest request{call.principal.player, id, target, {}};
  const auto resolved =
      react::resolve(react_state, catalog_state, identity, request, call.principal.body);
  Json inputs = Json::array(), args = Json::object();
  for (const auto &input : term.inputs) {
    inputs.push_back(input_record(terms, input));
    if (!input.optional)
      args[input.name] = example_value(input.type);
  }
  Json example = {{"action", term.name}, {"args", args}};
  if (target && resolved.route != react::Route::World)
    example["target"] = to_string(target);
  Json result = {{"name", term.name},
                 {"id", term.id},
                 {"description", term.description},
                 {"inputs", inputs},
                 {"right", resolved.right},
                 {"reach", resolved.reach},
                 {"signature", react::action_signature(terms, native, id)},
                 {"example", example}};
  if (declaration != native.rows.end() && !declaration->second.inputs_schema.empty()) {
    result.erase("inputs");
    result["inputs_schema"] = declaration->second.inputs_schema;
    if (declaration->second.inputs_schema.contains("examples") &&
        !declaration->second.inputs_schema["examples"].empty())
      result["example"]["args"] = declaration->second.inputs_schema["examples"][0];
  }
  if (plot) {
    const bool has_plot =
        declaration != native.rows.end() && declaration->second.inputs_schema.contains("properties")
            ? declaration->second.inputs_schema.at("properties").contains("plot")
            : std::ranges::any_of(term.inputs,
                                  [](const auto &input) { return input.name == "plot"; });
    if (has_plot)
      result["example"]["args"]["plot"] = plot->at("plot");
    if (declaration != native.rows.end())
      if (const auto *permission =
              std::get_if<react::PermissionAction>(&declaration->second.operation);
          permission && (*permission == react::PermissionAction::Grant ||
                         *permission == react::PermissionAction::Revoke)) {
        auto &args = result["example"]["args"];
        args["scope"] = {{"plot", plot->at("plot")}};
        if (*permission == react::PermissionAction::Grant)
          args["rights"] = {"enter"};
        else {
          args.erase("grant");
          args["holder"] = "p:PLAYER_ID";
        }
      }
    if (plot->contains("for_sale") && result["example"]["args"].contains("offer")) {
      result["example"]["args"]["offer"] = plot->at("for_sale").at("offer");
      if (result["example"]["args"].contains("max_price"))
        result["example"]["args"]["max_price"] = plot->at("for_sale").at("price");
    }
  }
  if (!target && declaration != native.rows.end() &&
      declaration->second.operation == react::NativeOperation(react::ExchangeAction::ReturnGoods))
    result["example"]["target"] = "o:SLOT.GENERATION";
  return result;
}
bool available(const amber::catalog::State &catalog_state, const amber::react::State &react_state,
               const ObjectState &objects, const amber::perm::State &perm_state, Step now,
               const net::Invocation &call, Handle target, TermId id) {
  auto &limits = perm_state.owner_limits;
  auto &players = perm_state.players;
  auto &grants = perm_state.grants;

  const auto ids = identity(objects.objects);
  const auto auth = authority(objects.objects);
  const ActionRequest request{call.principal.player, id, target, {}};
  const auto resolved =
      react::resolve(react_state, catalog_state, ids, request, call.principal.body);
  if (!resolved)
    return false;
  if (resolved.right.empty())
    return true;
  return perm::check_right(
             resolved.right, request.caller,
             perm::target(ids, auth, limits, perm_state.plots, resolved.permission_target), players,
             grants, now)
      .allowed;
}
} // namespace
namespace view_detail {
bool has_action_space(const react::State &state, std::string_view space) {
  if (space.empty())
    return false;
  return std::ranges::any_of(state.native_declarations.rows,
                             [&](const auto &row) { return row.second.space == space; }) ||
         std::ranges::any_of(state.world_scripts,
                             [&](const auto &row) { return row.space == space; });
}
Json action_spaces(const react::State &state) {
  std::map<std::string, std::string> spaces;
  for (const auto &[id, declaration] : state.native_declarations.rows)
    if (!declaration.space.empty())
      spaces.try_emplace(declaration.space);
  for (const auto &world : state.world_scripts)
    spaces[world.space] = world.description;
  auto result = Json::array();
  for (const auto &[space, description] : spaces) {
    Json entry = {
        {"target", space}, {"description", description}, {"actions", {{"target", space}}}};
    if (space == "land")
      entry["inspect"] = {{"target", space}};
    result.push_back(std::move(entry));
  }
  return result;
}
std::vector<TermId> space_action_ids(const catalog::State &catalog, const react::State &state,
                                     std::string_view space) {
  std::vector<TermId> result;
  for (const auto &[id, declaration] : state.native_declarations.rows) {
    bool shared = false;
    if (space == "land") {
      if (const auto *operation = std::get_if<react::ExchangeAction>(&declaration.operation))
        shared = *operation == react::ExchangeAction::ReturnGoods ||
                 *operation == react::ExchangeAction::Accept ||
                 *operation == react::ExchangeAction::CancelOffer ||
                 *operation == react::ExchangeAction::SetOfferPrice;
      else if (const auto *operation = std::get_if<react::PermissionAction>(&declaration.operation))
        shared = *operation == react::PermissionAction::Grant ||
                 *operation == react::PermissionAction::Revoke;
    }
    if (declaration.space == space || shared)
      result.push_back(id);
  }
  for (const auto &world : state.world_scripts)
    if (world.space == space && catalog.scripts.rows.at(world.script).declaration.active)
      for (const auto &action : catalog.scripts.rows.at(world.script).declaration.actions)
        result.push_back(action.action);
  std::sort(result.begin(), result.end(), [&](TermId a, TermId b) {
    return state.terms.rows[a].name < state.terms.rows[b].name;
  });
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}
} // namespace view_detail
net::Reply read_actions(const amber::catalog::State &catalog_state,
                        const amber::react::State &react_state,
                        const amber::ObjectState &objects_state, observe::Hearing &hearing,
                        const amber::Settings &settings, const amber::perm::State &perm_state,
                        Step now, const net::Invocation &call) {
  const auto values = react::value_read(objects_state, catalog_state, react_state);

  const auto page_size = settings.actions_page;
  const auto events_per_response = settings.perception.events_per_response;

  auto &models = catalog_state.models;
  auto &scripts = catalog_state.scripts;
  auto &instances = react_state.installations;
  auto &native = react_state.native_declarations;

  try {
    const auto target_name = call.request.fields.value("target", std::string{});
    std::string space;
    Json plot;
    Handle target = call.principal.body;
    if (target_name.starts_with("plot:")) {
      const auto id = view_detail::plot_id(target_name, perm_state.plots);
      plot = {{"plot", id}};
      if (const auto *offer = perm::plot_offer(perm_state.offers, id, now, call.principal.player))
        plot["for_sale"] = {{"offer", "offer:" + std::to_string(offer->id)},
                            {"price", offer->price}};
      space = "land";
      target = {};
    } else if (view_detail::has_action_space(react_state, target_name)) {
      space = target_name;
      target = {};
    } else if (!target_name.empty())
      target = view_detail::target(values.identity, call);
    const auto requested = call.request.fields.value("name", std::string{});
    Json result = Json::object();
    const auto record = [&](TermId id) {
      return action_record(catalog_state, react_state, values.identity, call, target, id,
                           plot.is_null() ? nullptr : &plot);
    };
    if (!requested.empty()) {
      const auto found = values.terms.by_name.find(requested);
      if (found == values.terms.by_name.end() ||
          values.terms.rows[found->second].kind != react::Kind::Action)
        return view_detail::error("Unknown action term");
      if (target_name.empty()) {
        const auto resolved =
            react::resolve(react_state, catalog_state, values.identity,
                           {call.principal.player, found->second, {}, {}}, call.principal.body);
        if (resolved && !resolved.permission_target)
          target = {};
      }
      result = {{"action", record(found->second)}};
    } else {
      auto names = !space.empty() ? view_detail::space_action_ids(catalog_state, react_state, space)
                   : target       ? object_action_ids(models, scripts, instances, values, target)
                                  : std::vector<TermId>{};
      if (target_name.empty()) {
        const auto player_ids = player_action_ids(native, values);
        names.insert(names.end(), player_ids.begin(), player_ids.end());
        result["action_spaces"] = view_detail::action_spaces(react_state);
      }
      std::erase_if(names, [&](TermId id) {
        return !available(catalog_state, react_state, objects_state, perm_state, now, call, target,
                          id);
      });
      if (space.empty()) {
        std::sort(names.begin(), names.end(), [&](TermId left, TermId right) {
          return values.terms.rows[left].name < values.terms.rows[right].name;
        });
        names.erase(std::unique(names.begin(), names.end()), names.end());
      }
      const auto page = call.request.fields.value("page", std::size_t{});
      const auto begin = page > names.size() / page_size ? names.size() : page * page_size,
                 end = std::min(names.size(), begin + page_size);
      result.update({{"actions", Json::array()},
                     {"page", page},
                     {"more", end < names.size()},
                     {"total", names.size()}});
      for (auto index = begin; index < end; ++index)
        result["actions"].push_back(record(names[index]));
    }
    result["ok"] = true;
    append_heard(result, hearing, call.session,
                 call.request.fields.value("events_limit", events_per_response));
    return net::Reply::result(std::move(result));
  } catch (const std::exception &error) {
    return view_detail::error(error.what());
  }
}
} // namespace amber::net
