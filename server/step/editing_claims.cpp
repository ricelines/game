#include "editing_private.hpp"

namespace amber::step::editing {
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
step::NativeCompletion edit_text(const step::NativeCall &call,
                                 catalog::InstanceTexts &instance_texts,
                                 const catalog::ModelTexts &model_texts, IdentityRead identity) {
  try {
    const auto object = call.request.target;
    const auto existing = instance_texts.rows.find(object.bits);
    catalog::Text before;
    if (existing != instance_texts.rows.end())
      before = existing->second;
    else if (const auto defaults = model_texts.rows.find(identity.model[object.slot()]);
             defaults != model_texts.rows.end())
      before = defaults->second;
    const auto after = patch(before, call.arguments.at("text"));
    if (before == after)
      return detail::result(false);
    instance_texts.rows[object.bits] = after;
    auto out = detail::result(true);
    out.changed_objects.push_back(object);
    return out;
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
step::NativeCompletion model_text(const step::NativeCall &call, catalog::ModelTexts &model_texts,
                                  const catalog::Models &models, IdentityRead identity) {
  try {
    const auto model = detail::model(models, call);
    const auto existing = model_texts.rows.find(model);
    const auto before = existing == model_texts.rows.end() ? catalog::Text{} : existing->second;
    const auto after = patch(before, call.arguments.at("text"));
    if (before == after)
      return detail::result(false);
    model_texts.rows[model] = after;
    const auto instances = detail::instances(identity, model);
    auto out = detail::result(true);
    out.changed_objects = instances;
    out.changed_models.push_back(model);
    return out;
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
step::NativeCompletion edit_detail(const step::NativeCall &call, catalog::AuthoredTexts &details) {
  try {
    const auto object = call.request.target;
    const auto id = call.arguments.at("id").get<catalog::TextId>();
    if (!id || id >= details.rows.size() || details.rows[id].id != id ||
        details.rows[id].target != object ||
        details.rows[id].kind != catalog::AuthoredTextKind::Detail)
      throw std::invalid_argument("no elaborated detail with that ID on this target");
    auto &entry = details.rows[id];
    const auto replacement = call.arguments.at("text").get<std::string>();
    const auto changed = entry.text != replacement;
    if (changed) {
      entry.text = replacement;
    }
    auto out = detail::result(changed, {{"id", id},
                                        {"text", entry.text},
                                        {"elaborated", true},
                                        {"author", "p:" + std::to_string(entry.author)},
                                        {"created_s", entry.created_s}});
    if (changed)
      out.changed_objects.push_back(object);
    return out;
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
} // namespace amber::step::editing
