#include "revision.hpp"
#include "bindings/authority/descriptors.hpp"
#include "bindings/commerce/commerce.hpp"
#include "bindings/metadata/metadata.hpp"
#include "bindings/scene.hpp"
#include "catalog/revision.hpp"
#include "private.hpp"

#include <algorithm>

namespace amber::bindings {
namespace {
using Json = nlohmann::json;
using structural_detail::PreparedReplacement;
std::string arguments(const Json &value) {
  if (!value.is_object() || value.size() != 2 || !value.contains("model") ||
      !value.contains("proposal") || !value.at("model").is_string() ||
      !value.at("proposal").is_string())
    return "expected {model:'m:N',proposal:'m:N'}";
  return {};
}
struct AssemblyChanges {
  Handle root;
  std::vector<PreparedReplacement *> replacements;
};
std::vector<AssemblyChanges> grouped(std::vector<PreparedReplacement> &replacements) {
  std::vector<AssemblyChanges> groups;
  for (auto &replacement : replacements) {
    const auto found =
        std::ranges::find(groups, replacement.mechanical_root, &AssemblyChanges::root);
    if (found == groups.end())
      groups.push_back({replacement.mechanical_root, {&replacement}});
    else
      found->replacements.push_back(&replacement);
  }
  return groups;
}
// Assemblies share native capacity even though their object edits are independent.
// Validate their complete new geometry together, before changing any definition.
void check_geometry(const ReplacementEdit &edit, const std::vector<AssemblyChanges> &groups) {
  std::vector<space::BodySpec> all;
  std::vector<Handle> retiring, removed;
  std::size_t all_sensors = 0;
  for (const auto &group : groups) {
    const auto &first = *group.replacements.front();
    auto definitions = first.retained;
    std::vector<space::BodySpec> bodies;
    std::size_t sensors = 0;
    for (auto *replacement : group.replacements) {
      removed.insert(removed.end(), replacement->removed.begin(), replacement->removed.end());
      for (auto h : replacement->removed)
        definitions.erase(h.bits);
      for (const auto &row : replacement->plan.retained)
        definitions[row.object.bits] = replacement->retained.at(row.object.bits);
      for (const auto &added : replacement->added) {
        if (added.body.geometry)
          bodies.push_back(added.body);
        sensors += added.volumes->size();
      }
    }
    for (const auto &[handle, definition] : definitions) {
      (void)handle;
      if (definition.body.geometry)
        bodies.push_back(definition.body);
      sensors += definition.volumes->size();
    }
    const auto checked = space::can_rebuild_assembly(edit.geometry.space, bodies, sensors,
                                                     first.before, first.promote);
    if (checked.status != space::Status::Ok &&
        (first.in_scene || checked.status != space::Status::Capacity))
      throw std::invalid_argument("model edit physics: " +
                                  std::string(space::status_text(checked.status)));
    if (!first.in_scene)
      continue;
    const bool physical_root = std::ranges::any_of(
        bodies, [&](const auto &body) { return body.object == first.mechanical_root; });
    const bool aggregate = first.promote && bodies.size() > (physical_root ? 1u : 0u);
    for (auto body : bodies) {
      if (aggregate)
        body.motion = space::Motion::Kinematic;
      else if (const auto old = space::body_motion(edit.geometry.space, edit.identity, body.object))
        body.motion = old->motion;
      all.push_back(body);
    }
    all_sensors += sensors + (aggregate ? 1 : 0);
    retiring.insert(retiring.end(), first.before.begin(), first.before.end());
  }
  std::string retirement_error;
  if (!can_retire_objects(edit.lifecycle, removed, retirement_error))
    throw std::invalid_argument(retirement_error);
  const auto checked = space::can_add(edit.geometry.space, all, all_sensors, retiring);
  if (checked.status != space::Status::Ok)
    throw std::invalid_argument("model edit physics: " +
                                std::string(space::status_text(checked.status)));
}
drive::NativeCompletion edit_definition(void *local, const drive::NativeCall &call) {
  using namespace structural_detail;
  auto &edit = *static_cast<ModelDefinitionEdit *>(local);
  drive::NativeCompletion out;
  const auto target = authority::identifier(call.arguments.at("model"), "m:");
  const auto proposal = authority::identifier(call.arguments.at("proposal"), "m:");
  const auto author = std::get_if<PlayerId>(&call.request.caller);
  if (!target || target >= edit.models.rows.size() || edit.models.rows[target].id != target)
    throw std::invalid_argument("unknown model");
  if (!author || edit.models.rows[target].author != *author) {
    out.result = record::Result::Denied;
    out.reply = {{"ok", false},
                 {"denied",
                  {{"need", "edit"},
                   {"on", call.arguments.at("model")},
                   {"why", "only the model author can edit its definition"},
                   {"fix", "ask the model author to edit this definition"}}}};
    return out;
  }
  const auto &replacement = edit.replacement;
  const auto identity = replacement.identity;
  const auto &params = replacement.geometry.values.params;
  const auto mappings =
      catalog::definition_parameters(edit.models, edit.parameters, edit.assets, target, proposal);
  bool changed = false;
  for (const auto &[before, after] : mappings)
    changed |= catalog::resolve_asset(edit.models, edit.parameters, target, before).hash !=
               catalog::resolve_asset(edit.models, edit.parameters, proposal, after).hash;
  out.reply = {{"ok", true}, {"effect", "none"}, {"reason", "no_effect"}};
  if (!changed)
    return out;
  const auto before_source = edit.models.rows[target].source_hash;
  const auto before_asset = edit.models.rows[target].asset;
  std::vector<Handle> roots;
  for (std::uint32_t slot = 0; slot < identity.model.size(); ++slot)
    if (const auto object = identity.at(slot); object && identity.model[slot] == target)
      roots.push_back(object);
  // Cancellation is an ordinary effect. If the following geometry edit is
  // rejected, the cancellation and its records remain visible to the caller.
  commerce::rebind_offers(edit.offers, roots, native_input(call), out.effects);
  out.changed = out.effects.changed;
  const ConstructionCatalog geometry_catalog{edit.models, edit.parameters, edit.assets};
  std::vector<PreparedReplacement> replacements;
  try {
    for (const auto root : roots) {
      const auto mapped =
          std::ranges::find(mappings, params[root.slot()], &catalog::RevisedParameter::before);
      auto assembly =
          lifecycle::expand(edit.models, edit.parameters, edit.assets, proposal, mapped->after);
      auto plan =
          lifecycle::plan_replace(edit.models, replacement.parts, identity, replacement.transforms,
                                  params, root, assembly, identity.material[root.slot()]);
      replacements.push_back(prepare_replacement(
          replacement, call, std::move(plan), geometry_catalog, replacement.geometry.values, true));
    }
    check_geometry(replacement, grouped(replacements));
  } catch (const std::exception &error) {
    out.result = record::Result::Denied;
    out.reply = {{"ok", false}, {"error", "bad_input"}, {"problem", error.what()}};
    return out;
  }
  const auto updated = catalog::update_definition(edit.models, edit.parameters, edit.texts,
                                                  edit.settings, edit.assets, target, proposal);
  catalog::project_definition_measurements(edit.measurements, edit.models, edit.parameters,
                                           edit.assets, updated, edit.measurement_roles);
  // Geometry is already checked. Only the installed model/child identities
  // differ from the proposal descriptors; retained defaults are the same.
  for (auto &prepared : replacements) {
    const auto root = prepared.plan.root;
    const auto assembly =
        lifecycle::expand(edit.models, edit.parameters, edit.assets, target, params[root.slot()]);
    prepared.plan =
        lifecycle::plan_replace(edit.models, replacement.parts, identity, replacement.transforms,
                                params, root, assembly, identity.material[root.slot()]);
  }
  auto groups = grouped(replacements);
  std::vector<space::AssemblyEdit> extracted;
  for (const auto &group : groups) {
    const auto &first = *group.replacements.front();
    extracted.push_back(space::extract_assembly(replacement.geometry.space, identity, group.root,
                                                std::span(first.before).subspan(1)));
  }
  for (std::size_t i = 0; i < groups.size(); ++i)
    commit_replacements(edit.replacement, call, groups[i].replacements, extracted[i], *author, out);
  out.changed = true;
  out.result = record::Result::Applied;
  out.reply.erase("reason");
  out.reply["effect"] = "applied";
  edit.publication.definitions_changed(updated.models);
  auto event = structural_record(call, record::Kind::System, "model_definition", roots);
  event.detail = {{"model", "m:" + std::to_string(target)},
                  {"proposal", "m:" + std::to_string(proposal)},
                  {"before_source", before_source},
                  {"after_source", edit.models.rows[target].source_hash},
                  {"before_asset", before_asset},
                  {"after_asset", edit.models.rows[target].asset}};
  const auto &source = edit.models.rows.at(proposal);
  event.credits.push_back({source.author, source.created_s, false});
  out.records.insert(out.records.begin(), std::move(event));
  return out;
}
} // namespace
TermId declare_model_edit(react::Terms &terms, command::NativeActions &declarations) {
  return command::declare_native(
      declarations, terms, "base.revise_model",
      "Replace a model definition with a baked proposal, updating its live instances. Only its "
      "author may do this.",
      "", false,
      {{"type", "object"},
       {"required", {"model", "proposal"}},
       {"properties", {{"model", {{"type", "string"}}}, {"proposal", {{"type", "string"}}}}},
       {"additionalProperties", false}});
}
void register_model_edit(drive::NativeOperations &operations, TermId action,
                         ModelDefinitionEdit &edit) {
  drive::register_native(operations, action, {arguments, edit_definition, &edit});
}
} // namespace amber::bindings
