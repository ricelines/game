#include "settings.hpp"
#include "view_private.hpp"

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
Json action_record(const catalog::Models &models, const catalog::Scripts &scripts,
                   const react::InstanceScripts &instances, const react::NativeActions &native,
                   const react::ValueRead &values, const net::Invocation &call, Handle target,
                   TermId id) {
  const auto &term = values.terms.rows.at(id);
  const ActionRequest request{call.principal.player, id, target, {}};
  const auto resolved = react::resolve(native, values.terms, models, scripts, instances,
                                       values.identity, request, call.principal.body);
  Json inputs = Json::array(), args = Json::object();
  for (const auto &input : term.inputs) {
    inputs.push_back(input_record(values.terms, input));
    if (!input.optional)
      args[input.name] = example_value(input.type);
  }
  Json example = {{"action", term.name}, {"args", args}};
  if (target)
    example["target"] = to_string(target);
  Json result = {{"name", term.name},
                 {"id", term.id},
                 {"description", term.description},
                 {"inputs", inputs},
                 {"right", resolved.right},
                 {"reach", resolved.reach},
                 {"signature", react::action_signature(values.terms, native, id)},
                 {"example", example}};
  if (const auto declaration = native.rows.find(id);
      declaration != native.rows.end() && !declaration->second.inputs_schema.empty()) {
    result.erase("inputs");
    result["inputs_schema"] = declaration->second.inputs_schema;
    if (declaration->second.inputs_schema.contains("examples") &&
        !declaration->second.inputs_schema["examples"].empty())
      result["example"]["args"] = declaration->second.inputs_schema["examples"][0];
  }
  return result;
}
bool available(const amber::catalog::State &catalog_state, const amber::react::State &react_state,
               const react::ValueRead &values, AuthorityRead authority,
               const amber::perm::State &perm_state, Step now, const net::Invocation &call,
               Handle target, TermId id) {
  auto &models = catalog_state.models;
  auto &scripts = catalog_state.scripts;
  auto &instances = react_state.installations;
  auto &native = react_state.native_declarations;
  auto &limits = perm_state.owner_limits;
  auto &players = perm_state.players;
  auto &grants = perm_state.grants;

  const ActionRequest request{call.principal.player, id, target, {}};
  const auto resolved = react::resolve(native, values.terms, models, scripts, instances,
                                       values.identity, request, call.principal.body);
  if (!resolved)
    return false;
  if (resolved.right.empty())
    return true;
  return perm::check_right(resolved.right, request.caller,
                           perm::target(values.identity, authority, limits, perm_state.plots,
                                        resolved.permission_target),
                           players, grants, now)
      .allowed;
}
} // namespace
net::Reply read_actions(const amber::catalog::State &catalog_state,
                        const amber::react::State &react_state,
                        const amber::ObjectState &objects_state, SessionViews &sessions,
                        observe::Hearing &hearing, const amber::Settings &settings,
                        const amber::perm::State &perm_state, Step now,
                        const net::Invocation &call) {
  const auto values = react::value_read(objects_state, catalog_state, react_state);
  const auto authority = amber::authority(objects_state.objects);

  const auto page_size = settings.actions_page;
  const auto events_per_response = settings.perception.events_per_response;

  auto &models = catalog_state.models;
  auto &scripts = catalog_state.scripts;
  auto &instances = react_state.installations;
  auto &native = react_state.native_declarations;

  try {
    Handle target = call.principal.body;
    if (call.request.fields.contains("target"))
      target = view_detail::target(values.identity, call);
    const auto requested = call.request.fields.value("name", std::string{});
    Json result;
    if (!requested.empty()) {
      const auto found = values.terms.by_name.find(requested);
      if (found == values.terms.by_name.end() ||
          values.terms.rows[found->second].kind != react::Kind::Action)
        return view_detail::error("Unknown action term");
      result = {{"action", action_record(models, scripts, instances, native, values, call, target,
                                         found->second)}};
    } else {
      auto names = target ? object_action_ids(models, scripts, instances, values, target)
                          : std::vector<TermId>{};
      if (!call.request.fields.contains("target")) {
        const auto player_ids = player_action_ids(native, values);
        names.insert(names.end(), player_ids.begin(), player_ids.end());
      }
      std::erase_if(names, [&](TermId id) {
        return !available(catalog_state, react_state, values, authority, perm_state, now, call,
                          target, id);
      });
      std::sort(names.begin(), names.end(), [&](TermId left, TermId right) {
        return values.terms.rows[left].name < values.terms.rows[right].name;
      });
      names.erase(std::unique(names.begin(), names.end()), names.end());
      const auto page = call.request.fields.value("page", std::size_t{});
      const auto begin = page > names.size() / page_size ? names.size() : page * page_size,
                 end = std::min(names.size(), begin + page_size);
      result = {{"actions", Json::array()},
                {"page", page},
                {"more", end < names.size()},
                {"total", names.size()}};
      for (auto index = begin; index < end; ++index)
        result["actions"].push_back(
            action_record(models, scripts, instances, native, values, call, target, names[index]));
    }
    if (target)
      sessions.rows[call.session].acquired.insert(target.bits);
    result["ok"] = true;
    append_heard(result, hearing, call.session,
                 call.request.fields.value("events_limit", events_per_response));
    return net::Reply::result(std::move(result));
  } catch (const std::exception &error) {
    return view_detail::error(error.what());
  }
}
} // namespace amber::net
