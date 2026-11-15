#include "bindings/scene.hpp"
#include "private.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <unordered_set>

namespace amber::bindings {
namespace {
using namespace structural_detail;
std::string edit_rights(const ReplaceOperation &operation, const drive::NativeCall &call,
                        std::span<const Handle> selected) {
  const auto &rights = operation.rights;
  const auto actor = permission_holder(call.request.caller);
  for (auto h : selected) {
    if (rights.identity.flags[h.slot()] & Escrow)
      return "edit: object belongs to an active offer or trade";
    const auto target = perm::target(rights.identity, rights.authority, rights.limits, h);
    const bool own =
        actor.kind == perm::HolderKind::Script &&
        declared_descendant(rights.declared_parts, rights.identity, actor.installed_on, h);
    const auto decision = own ? perm::Decision{perm::Reason::Allowed}
                              : perm::check(actor, perm::Verb::Edit, target, rights.players,
                                            rights.grants, call.step);
    if (!decision)
      return "edit: " + std::string(perm::reason_text(decision.reason));
  }
  return {};
}
} // namespace

namespace structural_detail {
PreparedReplacement prepare_replacement(const ReplacementEdit &operation,
                                        const drive::NativeCall &call, lifecycle::Replacement plan,
                                        ConstructionCatalog catalog, const react::ValueRead &values,
                                        bool preserve_defaults) {
  PreparedReplacement prepared;
  prepared.plan = std::move(plan);
  const auto &plan_ref = prepared.plan;
  const auto mechanical_root =
      space::assembly_root(operation.geometry.space, operation.identity, plan_ref.root);
  const auto before = lifecycle::collect(operation.parts, operation.identity, mechanical_root);
  const auto removed = lifecycle::collect(operation.parts, operation.identity, plan_ref.removed);
  const std::unordered_set<std::uint64_t> removed_set = [&] {
    std::unordered_set<std::uint64_t> result;
    for (auto h : removed)
      result.insert(h.bits);
    return result;
  }();
  std::map<std::uint64_t, const lifecycle::Rebinding *> rebound;
  for (const auto &row : plan_ref.retained)
    rebound[row.object.bits] = &row;
  auto prepare = [&](Handle h, lifecycle::Binding binding, PlotId plot, std::uint32_t flags,
                     bool new_part = false) {
    const auto effective =
        catalog::resolve_asset(catalog.models, catalog.parameters, binding.model, binding.params);
    if (preserve_defaults && h)
      binding.material = operation.identity.material[h.slot()];
    auto definition = describe_physics(
        operation.geometry.geometry, catalog.assets.rows.at(effective.hash), values,
        {binding.model, binding.params, binding.material, plot, h,
         preserve_defaults && h ? operation.identity.model[h.slot()] : 0},
        flags, operation.geometry.roles, operation.geometry.defaults, call.step, call.hz);
    if (new_part) {
      definition.body.motion = space::Motion::Kinematic;
      definition.mode_flags = ScriptedMotion;
    }
    if (definition.body.geometry)
      prepared.bodies.push_back(definition.body);
    prepared.sensors += definition.volumes->size();
    return definition;
  };
  std::map<std::uint64_t, space::Attachment> old_internal;
  for (auto h : before) {
    if (h != mechanical_root) {
      const auto parent = operation.parents.parent[h.slot()];
      old_internal[h.bits] = {h, parent, relative_pose(operation.transforms, h, parent), false};
    }
    if (removed_set.contains(h.bits))
      continue;
    auto changed = rebound.find(h.bits);
    const auto binding =
        changed == rebound.end()
            ? lifecycle::Binding{operation.identity.model[h.slot()],
                                 operation.identity.material[h.slot()], values.params[h.slot()]}
            : changed->second->after;
    prepared.retained.emplace(
        h.bits, prepare(h, binding, operation.plots[h.slot()], operation.identity.flags[h.slot()]));
  }
  const auto plot = operation.plots[plan_ref.root.slot()];
  for (const auto &part : plan_ref.added)
    prepared.added.push_back(prepare({}, {part.model, part.material, 0}, plot, Static, true));
  const auto root_motion =
      space::body_motion(operation.geometry.space, operation.identity, mechanical_root);
  const bool in_scene = root_motion || !(operation.identity.flags[mechanical_root.slot()] & Held);
  const bool was_promoted = std::ranges::any_of(before, [&](Handle h) {
    return h != mechanical_root &&
           space::assembly_root(operation.geometry.space, operation.identity, h) == mechanical_root;
  });
  const bool physical_parts = std::ranges::any_of(
      prepared.bodies, [&](const space::BodySpec &body) { return body.object != mechanical_root; });
  const bool promote =
      was_promoted || (physical_parts && root_motion &&
                       (root_motion->motion == space::Motion::Dynamic ||
                        (operation.identity.flags[mechanical_root.slot()] & Held)));
  prepared.mechanical_root = mechanical_root;
  prepared.before = before;
  prepared.removed = removed;
  prepared.old_internal = std::move(old_internal);
  prepared.in_scene = in_scene;
  prepared.promote = promote;
  for (const auto &[object, row] : rebound) {
    (void)row;
    prepared.rebound.insert(object);
  }
  prepared.capacity = space::can_rebuild_assembly(operation.geometry.space, prepared.bodies,
                                                  prepared.sensors, before, promote);
  return prepared;
}
void commit_replacements(ReplacementEdit &operation, const drive::NativeCall &call,
                         std::span<PreparedReplacement *> replacements,
                         const space::AssemblyEdit &edit, PlayerId maker,
                         drive::NativeCompletion &out) {
  const auto mechanical_root = edit.root;
  auto definitions = replacements.front()->retained;
  auto old_internal = replacements.front()->old_internal;
  std::unordered_set<std::uint64_t> rebound;
  std::vector<lifecycle::Created> created;
  std::vector<std::vector<record::Credit>> removed_credits;
  react::DeferredEffects effects;
  for (auto *prepared : replacements) {
    const auto &plan = prepared->plan;
    for (const auto &row : plan.retained)
      definitions[row.object.bits] = prepared->retained.at(row.object.bits);
    rebound.insert(prepared->rebound.begin(), prepared->rebound.end());
    removed_credits.push_back(
        operation.credits.resolve && !prepared->removed.empty()
            ? operation.credits.resolve(operation.credits.context, prepared->removed, 0, call.hz)
            : std::vector<record::Credit>{});
    if (!prepared->removed.empty())
      retire_objects(operation.lifecycle, prepared->removed, native_input(call), effects);
    created.push_back(operation.instances.replace(plan, maker, call.step));
    for (std::size_t i = 0; i < created.back().objects.size(); ++i) {
      auto definition = prepared->added[i];
      definition.body.object = created.back().objects[i];
      definitions.emplace(created.back().objects[i].bits, definition);
    }
  }
  const auto after = lifecycle::collect(operation.parts, operation.identity, mechanical_root);
  std::vector<space::BodySpec> bodies;
  std::vector<Handle> logical;
  std::vector<space::VolumeSpec> volumes;
  std::vector<space::Attachment> attachments;
  for (auto h : after) {
    const auto &definition = definitions.at(h.bits);
    if (definition.body.geometry)
      bodies.push_back(definition.body);
    else
      logical.push_back(h);
    for (const auto &volume : *definition.volumes)
      volumes.push_back({h, volume.geometry, volume.local, volume.kind});
    if (h != mechanical_root) {
      const auto parent = operation.parents.parent[h.slot()];
      const auto old = old_internal.find(h.bits);
      attachments.push_back(
          old != old_internal.end() && !rebound.contains(h.bits) && old->second.parent == parent
              ? old->second
              : space::Attachment{h, parent, relative_pose(operation.transforms, h, parent),
                                  false});
    }
  }
  const auto rebuilt =
      space::rebuild_assembly(operation.geometry.space, operation.identity, operation.transforms,
                              operation.parents, operation.geometry.flags, operation.physical, edit,
                              bodies, logical, volumes, attachments, operation.changes);
  if (rebuilt.status != space::Status::Ok)
    throw std::logic_error("preflighted assembly rebuild failed: " +
                           std::string(space::status_text(rebuilt.status)));
  perm::update_plots(operation.identity, operation.transforms, operation.membership, operation.land,
                     after, edit.in_scene);
  for (std::size_t i = 0; i < replacements.size(); ++i) {
    const auto &plan = replacements[i]->plan;
    const auto &removed = replacements[i]->removed;
    const auto &made = created[i];
    objects_rebound(operation.lifecycle, plan.retained, edit.in_scene, native_input(call), effects);
    objects_created(operation.lifecycle, made.objects, edit.in_scene, native_input(call), effects);
    out.changed = true;
    created_objects(out, made.objects);
    out.result = record::Result::Applied;
    out.reply["ok"] = true;
    out.reply["effect"] = "applied";
    std::vector<Handle> changed;
    for (const auto &row : plan.retained)
      changed.push_back(row.object);
    auto event = structural_record(call, record::Kind::System, "instance_binding", changed);
    const auto &original = plan.retained.front().before;
    event.detail = {{"target", to_string(plan.root)},
                    {"before",
                     {{"model", "m:" + std::to_string(original.model)},
                      {"params", original.params},
                      {"material", original.material}}},
                    {"after",
                     {{"model", "m:" + std::to_string(plan.retained.front().after.model)},
                      {"params", plan.retained.front().after.params},
                      {"material", plan.retained.front().after.material}}}};
    if (operation.credits.resolve)
      event.credits = operation.credits.resolve(operation.credits.context, changed, 0, call.hz);
    out.records.push_back(std::move(event));
    if (!removed.empty()) {
      auto destroyed = structural_record(call, record::Kind::Destroy, "parts", removed);
      destroyed.credits = std::move(removed_credits[i]);
      out.records.push_back(std::move(destroyed));
    }
    for (auto h : made.objects)
      out.records.push_back(birth_record(call, h, operation.identity, operation.owners,
                                         operation.plots, operation.geometry.values.params, maker,
                                         operation.credits));
  }
  completed_effects(out, std::move(effects));
}
void commit_replacement(ReplacementEdit &operation, const drive::NativeCall &call,
                        PreparedReplacement &prepared, const space::AssemblyEdit &edit,
                        PlayerId maker, drive::NativeCompletion &out) {
  PreparedReplacement *row = &prepared;
  commit_replacements(operation, call, std::span(&row, 1), edit, maker, out);
}
} // namespace structural_detail

ReplacementEdit replacement_edit(ReplaceOperation &operation) {
  return {operation.instances,
          operation.parts,
          operation.rights.identity,
          operation.rights.authority.owner,
          operation.rights.authority.plot,
          operation.geometry,
          operation.transforms,
          operation.parents,
          operation.physical,
          operation.changes,
          operation.lifecycle,
          operation.membership,
          operation.rights.plots,
          operation.credits};
}

drive::NativeCompletion replace_instance(void *context, const drive::NativeCall &call) {
  auto &operation = *static_cast<ReplaceOperation *>(context);
  const auto &rights = operation.rights;
  const auto selected = lifecycle::collect(operation.parts, rights.identity, call.request.target);
  const auto error = edit_rights(operation, call, selected);
  if (!error.empty())
    return structural_denied(
        "edit", to_string(call.request.target), error,
        "ask p:" + std::to_string(rights.authority.owner[call.request.target.slot()]) +
            " for Edit on this object and its declared parts");
  const auto input = native_input(call);
  const auto maker = std::holds_alternative<PlayerId>(call.request.caller)
                         ? std::get<PlayerId>(call.request.caller)
                         : rights.authority.owner[input.receiver.slot()];
  const auto binding =
      instance_binding(call.arguments, operation.catalog, operation.geometry.values.terms);
  auto assembly = lifecycle::expand(operation.catalog.models, operation.catalog.parameters,
                                    operation.catalog.assets, binding.model, binding.params);
  assembly.material = binding.material;
  auto plan = lifecycle::plan_replace(operation.catalog.models, operation.parts, rights.identity,
                                      rights.transforms, operation.geometry.values.params,
                                      call.request.target, assembly, binding.material);
  drive::NativeCompletion out;
  out.reply = {{"ok", true}, {"effect", "none"}};
  if (plan.retained.front().before == plan.retained.front().after)
    return out;
  auto edit_context = replacement_edit(operation);
  auto prepared = prepare_replacement(edit_context, call, std::move(plan), operation.catalog,
                                      operation.geometry.values);
  if (prepared.capacity.status != space::Status::Ok &&
      (prepared.in_scene || prepared.capacity.status != space::Status::Capacity))
    return structural_denied("physical_capacity", to_string(call.request.target),
                             space::status_text(prepared.capacity.status),
                             "choose a compatible model or remove unused physical objects");
  std::string retirement_error;
  if (!can_retire_objects(operation.lifecycle, prepared.removed, retirement_error))
    return structural_denied("retirement", to_string(call.request.target),
                             std::move(retirement_error),
                             "cancel the object's active offers or attachments before retrying");
  auto edit =
      space::extract_assembly(operation.geometry.space, rights.identity, prepared.mechanical_root,
                              std::span(prepared.before).subspan(1));
  commit_replacement(edit_context, call, prepared, edit, maker, out);
  return out;
}
} // namespace amber::bindings
