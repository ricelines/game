#include "common.hpp"
#include "drive/drive.hpp"

namespace amber::bindings::viewing {
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
                          {"unit", command::format_unit(terms.rows.at(input.term).dimensions)}};
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
Json action_record(const CatalogRead &catalog, const control::Invocation &call, Handle target,
                   TermId id) {
  const auto &term = catalog.values.terms.rows.at(id);
  const ActionRequest request{call.principal.player, id, target, {}};
  const auto resolved =
      command::resolve(catalog.native, catalog.values.terms, catalog.models, catalog.scripts,
                       catalog.instances, catalog.values.identity, request, call.principal.body);
  Json inputs = Json::array(), args = Json::object();
  for (const auto &input : term.inputs) {
    inputs.push_back(input_record(catalog.values.terms, input));
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
                 {"signature", command::action_signature(catalog.values.terms, catalog.native, id)},
                 {"example", example}};
  if (const auto native = catalog.native.rows.find(id);
      native != catalog.native.rows.end() && !native->second.inputs_schema.empty()) {
    result.erase("inputs");
    result["inputs_schema"] = native->second.inputs_schema;
    if (native->second.inputs_schema.contains("examples") &&
        !native->second.inputs_schema["examples"].empty())
      result["example"]["args"] = native->second.inputs_schema["examples"][0];
  }
  return result;
}
bool available(const ActionsBinding &binding, const control::Invocation &call, Handle target,
               TermId id) {
  const auto &catalog = binding.catalog;
  const ActionRequest request{call.principal.player, id, target, {}};
  const auto resolved =
      command::resolve(catalog.native, catalog.values.terms, catalog.models, catalog.scripts,
                       catalog.instances, catalog.values.identity, request, call.principal.body);
  if (!resolved)
    return false;
  if (resolved.right.empty())
    return true;
  return drive::check_right(resolved.right, request.caller,
                            perm::target(catalog.values.identity, binding.authority, binding.limits,
                                         resolved.permission_target),
                            binding.players, binding.grants, binding.now)
      .allowed;
}
} // namespace
control::Reply actions_handler(void *local, const control::Invocation &call) {
  auto &b = *static_cast<ActionsBinding *>(local);
  try {
    Handle target = call.principal.body;
    if (call.request.fields.contains("target"))
      target = detail::target(b.catalog, call);
    const auto requested = call.request.fields.value("name", std::string{});
    Json result;
    if (!requested.empty()) {
      const auto found = b.catalog.values.terms.by_name.find(requested);
      if (found == b.catalog.values.terms.by_name.end() ||
          b.catalog.values.terms.rows[found->second].kind != react::Kind::Action)
        return detail::error("Unknown action term");
      result = {{"action", action_record(b.catalog, call, target, found->second)}};
    } else {
      auto names = target ? object_action_ids(b.catalog, target) : std::vector<TermId>{};
      if (!call.request.fields.contains("target")) {
        const auto native = player_action_ids(b.catalog);
        names.insert(names.end(), native.begin(), native.end());
      }
      std::erase_if(names, [&](TermId id) { return !available(b, call, target, id); });
      std::sort(names.begin(), names.end(), [&](TermId left, TermId right) {
        return b.catalog.values.terms.rows[left].name < b.catalog.values.terms.rows[right].name;
      });
      names.erase(std::unique(names.begin(), names.end()), names.end());
      const auto page = call.request.fields.value("page", std::size_t{});
      const auto begin = page > names.size() / b.page_size ? names.size() : page * b.page_size,
                 end = std::min(names.size(), begin + b.page_size);
      result = {{"actions", Json::array()},
                {"page", page},
                {"more", end < names.size()},
                {"total", names.size()}};
      for (auto index = begin; index < end; ++index)
        result["actions"].push_back(action_record(b.catalog, call, target, names[index]));
    }
    if (target)
      b.sessions.rows[call.session].acquired.insert(target.bits);
    result["ok"] = true;
    append_heard(result, b.hearing, call.session,
                 call.request.fields.value("events_limit", b.events_per_response));
    return control::Reply::result(std::move(result));
  } catch (const std::exception &error) {
    return detail::error(error.what());
  }
}
} // namespace amber::bindings::viewing
