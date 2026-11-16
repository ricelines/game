#include "replacement_private.hpp"

#include <algorithm>

namespace amber::step {
namespace {
using structural_detail::ReplacementGeometry;
struct AssemblyChanges {
  Handle root;
  std::vector<ReplacementGeometry *> replacements;
};
std::vector<AssemblyChanges> grouped(std::vector<ReplacementGeometry> &replacements) {
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
// All changed instances share the same real native body pool.
void check_geometry(const space::Space &space, IdentityRead identity,
                    const std::vector<AssemblyChanges> &groups) {
  std::vector<space::BodySpec> all;
  std::vector<Handle> retiring;
  std::size_t all_sensors = 0;
  for (const auto &group : groups) {
    const auto &first = *group.replacements.front();
    auto definitions = first.retained;
    std::vector<space::BodySpec> bodies;
    std::size_t sensors = 0;
    for (auto *replacement : group.replacements) {
      for (auto h : replacement->removed)
        definitions.erase(h.bits);
      for (const auto &row : replacement->rows.retained)
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
    const auto checked =
        space::can_rebuild_assembly(space, bodies, sensors, first.before, first.promote);
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
      else if (const auto old = space::body_motion(space, identity, body.object))
        body.motion = old->motion;
      all.push_back(body);
    }
    all_sensors += sensors + (aggregate ? 1 : 0);
    retiring.insert(retiring.end(), first.before.begin(), first.before.end());
  }
  const auto checked = space::can_add(space, all, all_sensors, retiring);
  if (checked.status != space::Status::Ok)
    throw std::invalid_argument("model edit physics: " +
                                std::string(space::status_text(checked.status)));
}
void cancellation_effects(NativeCompletion &out, NativeCompletion cancellation) {
  out.changed |= cancellation.changed;
  out.changed_objects.insert(out.changed_objects.end(), cancellation.changed_objects.begin(),
                             cancellation.changed_objects.end());
  out.records.insert(out.records.end(), std::make_move_iterator(cancellation.records.begin()),
                     std::make_move_iterator(cancellation.records.end()));
}
} // namespace
std::string validate_model_edit(const nlohmann::json &value) {
  if (!value.is_object() || value.size() != 2 || !value.contains("model") ||
      !value.contains("proposal") || !value.at("model").is_string() ||
      !value.at("proposal").is_string())
    return "expected {model:'m:N',proposal:'m:N'}";
  return {};
}
NativeCompletion
revise_model(amber::ObjectState &objects_state, amber::catalog::State &catalog_state,
             amber::react::State &react_state, catalog::MeasurementBindings measurement_roles,
             const GeometryAssets &geometry, amber::space::State &space_state,
             amber::perm::State &perm_state, PhysicsDefaults defaults, const NativeCall &call) {
  const auto values = amber::react::value_read(objects_state, catalog_state, react_state);

  auto &settings = react_state.model_settings;

  auto &space = space_state.space;

  auto &parts = objects_state.parts;

  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;
  auto &texts = catalog_state.model_text;
  auto &assets = catalog_state.assets;
  auto &measurements = catalog_state.baked_measurements;
  auto transforms = amber::transforms(objects_state.objects);

  auto permissions = amber::permission_flags(objects_state.objects);

  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto &ledger = perm_state.ledger;
  auto &offers = perm_state.offers;
  auto &trades = perm_state.trades;
  auto &plots = perm_state.plots;

  using namespace structural_detail;
  NativeCompletion out;
  const auto target = identifier(call.arguments.at("model"), "m:");
  const auto proposal = identifier(call.arguments.at("proposal"), "m:");
  const auto author = std::get_if<PlayerId>(&call.request.caller);
  if (!target || target >= models.rows.size() || models.rows[target].id != target)
    throw std::invalid_argument("unknown model");
  const auto group = models.rows[target].group;
  const auto &players = perm_state.players;
  if (!author || !*author ||
      (models.rows[target].author != *author &&
       (!group || !std::binary_search(players.rows[*author].groups.begin(),
                                      players.rows[*author].groups.end(), group))))
    return structural_denied(
        "edit", call.arguments.at("model").get<std::string>(),
        "only the model author or a model group member can edit its definition",
        "ask the model author to edit this definition");
  const auto identity = values.identity;
  const auto &params = values.params;
  const auto mappings =
      catalog::definition_parameters(models, parameters, assets, target, proposal);
  bool changed = false;
  for (const auto &[before, after] : mappings)
    changed |= catalog::resolve_asset(models, parameters, target, before).hash !=
               catalog::resolve_asset(models, parameters, proposal, after).hash;
  out.reply = {{"ok", true}, {"effect", "none"}, {"reason", "no_effect"}};
  if (!changed)
    return out;
  const auto before_source = models.rows[target].source_hash;
  const auto before_asset = models.rows[target].asset;
  std::vector<Handle> roots;
  for (std::uint32_t slot = 0; slot < identity.model.size(); ++slot)
    if (const auto object = identity.at(slot); object && identity.model[slot] == target)
      roots.push_back(object);
  // This ordinary cancellation persists even if the subsequent geometry edit is denied.
  cancellation_effects(
      out, rebind_offers(offers, trades, permissions, identity, parts, roots, call.step));
  std::vector<ReplacementGeometry> replacements;
  try {
    for (const auto root : roots) {
      const auto mapped =
          std::ranges::find(mappings, params[root.slot()], &catalog::RevisedParameter::before);
      auto assembly = lifecycle::expand(models, parameters, assets, proposal, mapped->after);
      auto rows = lifecycle::plan_replace(models, parts, identity, transforms, params, root,
                                          assembly, identity.material[root.slot()]);
      replacements.push_back(replacement_geometry(space_state, objects_state, authority.plot,
                                                  geometry, catalog_state, values, defaults, call,
                                                  std::move(rows), true));
    }
    std::vector<Handle> removed;
    for (const auto &replacement : replacements)
      removed.insert(removed.end(), replacement.removed.begin(), replacement.removed.end());
    std::sort(removed.begin(), removed.end());
    removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
    const auto money = perm::can_refund_object_accounts(ledger, authority, removed);
    if (money != perm::Reason::Allowed)
      throw std::invalid_argument(std::string(perm::reason_text(money)));
    check_geometry(space, identity, grouped(replacements));
  } catch (const std::exception &error) {
    out.result = record::Result::Denied;
    out.reply = {{"ok", false}, {"error", "bad_input"}, {"problem", error.what()}};
    return out;
  }
  const auto updated =
      catalog::update_definition(models, parameters, texts, assets, target, proposal);
  catalog::inherit_definition_settings(settings, updated);
  catalog::project_definition_measurements(measurements, models, parameters, assets, updated,
                                           measurement_roles);
  for (auto &replacement : replacements) {
    const auto root = replacement.rows.root;
    const auto assembly =
        lifecycle::expand(models, parameters, assets, target, params[root.slot()]);
    replacement.rows = lifecycle::plan_replace(models, parts, identity, transforms, params, root,
                                               assembly, identity.material[root.slot()]);
  }
  auto groups = grouped(replacements);
  std::vector<space::AssemblyEdit> extracted;
  for (const auto &group : groups) {
    const auto &first = *group.replacements.front();
    extracted.push_back(
        space::extract_assembly(space, identity, group.root, std::span(first.before).subspan(1)));
  }
  for (std::size_t i = 0; i < groups.size(); ++i)
    rebuild_instances(objects_state, space_state, plots, catalog_state, groups[i].replacements,
                      extracted[i], *author, call, out);
  out.changed = true;
  out.result = record::Result::Applied;
  out.reply.erase("reason");
  out.reply["effect"] = "applied";
  out.changed_models = updated.models;
  auto event = structural_record(call, record::Kind::System, "model_definition", roots);
  event.detail = {
      {"model", "m:" + std::to_string(target)}, {"proposal", "m:" + std::to_string(proposal)},
      {"before_source", before_source},         {"after_source", models.rows[target].source_hash},
      {"before_asset", before_asset},           {"after_asset", models.rows[target].asset}};
  const auto &source = models.rows.at(proposal);
  event.credits.push_back({source.author, source.created_s, false});
  out.records.insert(out.records.begin(), std::move(event));
  return out;
}
TermId declare_model_edit(react::Terms &terms, react::NativeActions &declarations) {
  return react::declare_native(
      declarations, terms, "base.revise_model",
      "Replace a model definition with a baked proposal, updating its live instances. Only its "
      "author may do this.",
      "", false,
      {{"type", "object"},
       {"required", {"model", "proposal"}},
       {"properties", {{"model", {{"type", "string"}}}, {"proposal", {{"type", "string"}}}}},
       {"additionalProperties", false}},
      react::ConstructionAction::ReviseModel);
}
} // namespace amber::step
