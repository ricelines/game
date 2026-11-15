#include "common.hpp"

namespace amber::bindings::editing {
namespace {
catalog::Text patch(catalog::Text value, const Json &text) {
  if (text.contains("name"))
    value.name = text["name"].get<std::string>();
  if (text.contains("description"))
    value.description = text["description"].get<std::string>();
  if (text.contains("writing"))
    value.writing = text["writing"].get<std::string>();
  return value;
}
} // namespace
drive::NativeCompletion edit_text(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<TextEditing *>(local);
  try {
    detail::edit(binding.access, call);
    const auto object = call.request.target;
    const auto existing = binding.instance_texts.rows.find(object.bits);
    catalog::Text before;
    if (existing != binding.instance_texts.rows.end())
      before = existing->second;
    else if (const auto defaults =
                 binding.model_texts.rows.find(binding.access.identity.model[object.slot()]);
             defaults != binding.model_texts.rows.end())
      before = defaults->second;
    const auto after = patch(before, call.arguments.at("text"));
    if (before == after)
      return detail::result(false);
    binding.instance_texts.rows[object.bits] = after;
    metadata_changed(binding.changes, std::span{&object, 1});
    return detail::result(true);
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
drive::NativeCompletion model_text(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<TextEditing *>(local);
  try {
    const auto model = detail::model(binding.models, call);
    const auto existing = binding.model_texts.rows.find(model);
    const auto before =
        existing == binding.model_texts.rows.end() ? catalog::Text{} : existing->second;
    const auto after = patch(before, call.arguments.at("text"));
    if (before == after)
      return detail::result(false);
    binding.model_texts.rows[model] = after;
    const auto instances = detail::instances(binding.access.identity, model);
    metadata_changed(binding.changes, instances, std::span{&model, 1});
    return detail::result(true);
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
drive::NativeCompletion edit_detail(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<TextEditing *>(local);
  try {
    detail::edit(binding.access, call);
    const auto object = call.request.target;
    const auto id = call.arguments.at("id").get<catalog::TextId>();
    if (!id || id >= binding.details.rows.size() || binding.details.rows[id].id != id ||
        binding.details.rows[id].target != object ||
        binding.details.rows[id].kind != catalog::AuthoredTextKind::Detail)
      throw std::invalid_argument("no elaborated detail with that ID on this target");
    auto &entry = binding.details.rows[id];
    const auto replacement = call.arguments.at("text").get<std::string>();
    const auto changed = entry.text != replacement;
    if (changed) {
      entry.text = replacement;
      metadata_changed(binding.changes, std::span{&object, 1});
    }
    return detail::result(changed, {{"id", id},
                                    {"text", entry.text},
                                    {"elaborated", true},
                                    {"author", "p:" + std::to_string(entry.author)},
                                    {"created_s", entry.created_s}});
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
} // namespace amber::bindings::editing
