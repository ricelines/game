#include "bindings/scene.hpp"
#include "private.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <unordered_set>

namespace amber::bindings {
namespace {
using namespace structural_detail;
struct EditActor {
  perm::Holder holder;
  Handle body;
  bool immediate;
};
EditActor edit_actor(const drive::StructuralCall &call) {
  EditActor actor{perm::Holder::script(call.handler.script, call.invocation.receiver),
                  call.invocation.receiver, false};
  if (call.invocation.action_caller && call.request.target == call.invocation.action_target) {
    actor.holder = permission_holder(*call.invocation.action_caller);
    actor.immediate = true;
    if (std::holds_alternative<PlayerId>(*call.invocation.action_caller))
      actor.body = call.invocation.action_body;
  }
  return actor;
}
std::string edit_rights(const ReplaceOperation &operation, const drive::StructuralCall &call,
                        const EditActor &actor, std::span<const Handle> selected) {
  const auto &rights = operation.rights;
  for (auto h : selected) {
    if (rights.identity.flags[h.slot()] & Escrow)
      return "edit: object belongs to an active offer or trade";
    const auto target = perm::target(rights.identity, rights.authority, rights.limits, h);
    // Approved scripts have their normal own-object/declared-part mechanical rights.
    const bool own =
        actor.holder.kind == perm::HolderKind::Script &&
        declared_descendant(rights.declared_parts, rights.identity, actor.holder.installed_on, h);
    const auto decision = own ? perm::Decision{perm::Reason::Allowed}
                              : perm::check(actor.holder, perm::Verb::Edit, target, rights.players,
                                            rights.grants, call.invocation.step);
    if (!decision)
      return "edit: " + std::string(perm::reason_text(decision.reason));
  }
  if (!actor.immediate)
    return {};
  if (!rights.identity.valid(actor.body))
    return "edit actor body is no longer alive";
  const auto target = call.request.target;
  const bool own_inventory = actor.holder.kind == perm::HolderKind::Player &&
                             rights.authority.owner[target.slot()] == actor.holder.id &&
                             operation.parents.parent[target.slot()] == actor.body &&
                             (rights.identity.flags[target.slot()] & Held);
  if (own_inventory)
    return {};
  const auto bounds = space::bounds(rights.space, rights.identity, target);
  if (!bounds || !drive::in_reach(rights.transforms.position[actor.body.slot()],
                                  {bounds->min, bounds->max}, rights.reach_m))
    return "edit target is out of reach";
  return {};
}
} // namespace

namespace structural_detail {
PreparedReplacement prepare_replacement(const ReplacementEdit &operation,
                                        const drive::StructuralCall &call,
                                        lifecycle::Replacement plan, ConstructionCatalog catalog,
                                        const react::ValueRead &values) {
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
    auto definition = describe_physics(
        operation.geometry.geometry, catalog.assets.rows.at(effective.hash), values,
        {binding.model, binding.params, binding.material, plot, h}, flags, operation.geometry.roles,
        operation.geometry.defaults, call.invocation.step, call.invocation.hz);
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
void commit_replacement(ReplacementEdit &operation, const drive::StructuralCall &call,
                        PreparedReplacement &prepared, const space::AssemblyEdit &edit,
                        PlayerId maker, drive::StructuralCompletion &out) {
  const auto &plan = prepared.plan;
  const auto mechanical_root = prepared.mechanical_root;
  const auto &removed = prepared.removed;
  const auto &old_internal = prepared.old_internal;
  const auto &rebound = prepared.rebound;
  std::vector<record::Credit> removed_credits;
  if (operation.credits.resolve && !removed.empty())
    removed_credits =
        operation.credits.resolve(operation.credits.context, removed, 0, call.invocation.hz);
  react::DeferredEffects effects;
  if (!removed.empty())
    retire_objects(operation.lifecycle, removed, call.invocation, effects);
  const auto created = operation.instances.replace(plan, maker, call.invocation.step);
  for (std::size_t i = 0; i < created.objects.size(); ++i) {
    auto definition = prepared.added[i];
    definition.body.object = created.objects[i];
    prepared.retained.emplace(created.objects[i].bits, definition);
  }
  const auto after = lifecycle::collect(operation.parts, operation.identity, mechanical_root);
  std::vector<space::BodySpec> bodies;
  std::vector<Handle> logical;
  std::vector<space::VolumeSpec> volumes;
  std::vector<space::Attachment> attachments;
  for (auto h : after) {
    const auto &definition = prepared.retained.at(h.bits);
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
  objects_rebound(operation.lifecycle, plan.retained, prepared.in_scene, call.invocation, effects);
  objects_created(operation.lifecycle, created.objects, prepared.in_scene, call.invocation,
                  effects);
  out.changed = true;
  out.created.insert(out.created.end(), created.objects.begin(), created.objects.end());
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
    event.credits =
        operation.credits.resolve(operation.credits.context, changed, 0, call.invocation.hz);
  out.records.push_back(std::move(event));
  if (!removed.empty()) {
    auto destroyed = structural_record(call, record::Kind::Destroy, "parts", removed);
    destroyed.credits = std::move(removed_credits);
    out.records.push_back(std::move(destroyed));
  }
  for (auto h : created.objects)
    out.records.push_back(birth_record(call, h, operation.identity, operation.owners,
                                       operation.plots, operation.geometry.values.params, maker,
                                       operation.credits));
  completed_effects(out, std::move(effects));
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
          operation.resources,
          operation.lifecycle,
          operation.credits};
}

drive::StructuralCompletion replace_instance(void *context, const drive::StructuralCall &call) {
  auto &operation = *static_cast<ReplaceOperation *>(context);
  const auto &rights = operation.rights;
  drive::StructuralCompletion out;
  if (!call.handler.world_rule || !rights.identity.valid(call.invocation.receiver)) {
    out.error = "replacement requires a live approved world-rule receiver";
    return out;
  }
  if (!rights.identity.valid(call.request.target)) {
    out.error = "edit target is no longer alive";
    return out;
  }
  const auto selected = lifecycle::collect(operation.parts, rights.identity, call.request.target);
  const auto actor = edit_actor(call);
  const auto maker = actor.holder.kind == perm::HolderKind::Player
                         ? PlayerId(actor.holder.id)
                         : rights.authority.owner[call.invocation.receiver.slot()];
  out.error = edit_rights(operation, call, actor, selected);
  if (!out.error.empty())
    return out;
  auto assembly = lifecycle::expand(operation.catalog.models, operation.catalog.parameters,
                                    operation.catalog.assets, call.request.binding.model,
                                    call.request.binding.params);
  assembly.material = call.request.binding.material;
  auto plan = lifecycle::plan_replace(operation.catalog.models, operation.parts, rights.identity,
                                      rights.transforms, operation.geometry.values.params,
                                      call.request.target, assembly, call.request.binding.material);
  if (plan.retained.front().before == plan.retained.front().after)
    return out;
  out.error = check_resources(operation.resources, call);
  if (!out.error.empty())
    return out;
  auto edit_context = replacement_edit(operation);
  auto prepared = prepare_replacement(edit_context, call, std::move(plan), operation.catalog,
                                      operation.geometry.values);
  if (prepared.capacity.status != space::Status::Ok &&
      (prepared.in_scene || prepared.capacity.status != space::Status::Capacity)) {
    out.error = "replacement physics: " + std::string(space::status_text(prepared.capacity.status));
    return out;
  }
  if (!can_retire_objects(operation.lifecycle, prepared.removed, out.error))
    return out;
  react::DeferredEffects effects;
  commit_resources(operation.resources, call, effects);
  auto edit =
      space::extract_assembly(operation.geometry.space, rights.identity, prepared.mechanical_root,
                              std::span(prepared.before).subspan(1));
  completed_effects(out, std::move(effects));
  commit_replacement(edit_context, call, prepared, edit, maker, out);
  return out;
}
} // namespace amber::bindings
