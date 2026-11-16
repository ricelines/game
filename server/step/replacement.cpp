#include "replacement_private.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <unordered_set>

namespace amber::step {
namespace {
using namespace structural_detail;
std::string edit_rights(IdentityRead identity, AuthorityRead authority,
                        const perm::OwnerLimits &limits, const perm::Plots &plots,
                        const perm::Players &players, const perm::Grants &grants,
                        const step::NativeCall &call, std::span<const Handle> selected) {
  const auto actor = permission_holder(call.request.caller);
  for (auto h : selected) {
    if (identity.flags[h.slot()] & Escrow)
      return "edit: object belongs to an active offer or trade";
    const auto decision =
        perm::check(actor, perm::Verb::Edit, perm::target(identity, authority, limits, plots, h),
                    players, grants, call.step);
    if (!decision)
      return "edit: " + std::string(perm::reason_text(decision.reason));
  }
  return {};
}
} // namespace

namespace structural_detail {
ReplacementGeometry replacement_geometry(const amber::space::State &space_state,
                                         const amber::ObjectState &objects_state,
                                         const std::vector<PlotId> &plots,
                                         const GeometryAssets &geometry,
                                         const amber::catalog::State &catalog_state,
                                         const react::ValueRead &values, PhysicsDefaults defaults,
                                         const step::NativeCall &call, lifecycle::Replacement plan,
                                         bool preserve_defaults) {
  auto &space = space_state.space;
  const auto &physics = catalog_state.mechanical.values.at("physics");
  const PhysicsRoles roles{
      physics.at("density").get<TermId>(), physics.at("transmission").get<TermId>(),
      physics.value("friction", TermId{}), physics.value("restitution", TermId{})};

  auto &parts = objects_state.parts;
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto transforms = amber::transforms(std::as_const(objects_state.objects));
  auto parents = amber::parents(std::as_const(objects_state.objects));
  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;
  auto &assets = catalog_state.assets;

  ReplacementGeometry prepared;
  prepared.rows = std::move(plan);
  const auto &plan_ref = prepared.rows;
  const auto mechanical_root = space::assembly_root(space, identity, plan_ref.root);
  const auto before = lifecycle::collect(parts, identity, mechanical_root);
  const auto removed = lifecycle::collect(parts, identity, plan_ref.removed);
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
        catalog::resolve_asset(models, parameters, binding.model, binding.params);
    if (preserve_defaults && h)
      binding.material = identity.material[h.slot()];
    auto definition = describe_physics(geometry, assets.rows.at(effective.hash), values,
                                       {binding.model, binding.params, binding.material, plot, h,
                                        preserve_defaults && h ? identity.model[h.slot()] : 0},
                                       flags, roles, defaults,
                                       amber::step::Clock{.step = call.step, .hz = call.hz});
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
      const auto parent = parents.parent[h.slot()];
      old_internal[h.bits] = {h, parent, relative_pose(transforms, h, parent), false};
    }
    if (removed_set.contains(h.bits))
      continue;
    auto changed = rebound.find(h.bits);
    const auto binding = changed == rebound.end() ? lifecycle::Binding{identity.model[h.slot()],
                                                                       identity.material[h.slot()],
                                                                       values.params[h.slot()]}
                                                  : changed->second->after;
    prepared.retained.emplace(h.bits,
                              prepare(h, binding, plots[h.slot()], identity.flags[h.slot()]));
  }
  const auto plot = plots[plan_ref.root.slot()];
  for (const auto &part : plan_ref.added)
    prepared.added.push_back(prepare({}, {part.model, part.material, 0}, plot, Static, true));
  const auto root_motion = space::body_motion(space, identity, mechanical_root);
  const bool in_scene = root_motion || !(identity.flags[mechanical_root.slot()] & Held);
  const bool was_promoted = std::ranges::any_of(before, [&](Handle h) {
    return h != mechanical_root && space::assembly_root(space, identity, h) == mechanical_root;
  });
  const bool physical_parts = std::ranges::any_of(
      prepared.bodies, [&](const space::BodySpec &body) { return body.object != mechanical_root; });
  const bool promote = was_promoted || (physical_parts && root_motion &&
                                        (root_motion->motion == space::Motion::Dynamic ||
                                         (identity.flags[mechanical_root.slot()] & Held)));
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
  return prepared;
}
void rebuild_instances(amber::ObjectState &objects_state, amber::space::State &space_state,
                       const perm::Plots &land, const amber::catalog::State &catalog_state,
                       std::span<ReplacementGeometry *> replacements,
                       const space::AssemblyEdit &edit, PlayerId maker, const NativeCall &call,
                       NativeCompletion &out) {

  auto &objects = objects_state.objects;
  auto &parts = objects_state.parts;
  auto &declared_parts = objects_state.declared_parts;
  auto transforms = amber::transforms(objects_state.objects);
  auto parents = amber::parents(objects_state.objects);

  auto membership = amber::plot_membership(objects_state.objects);
  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto attribution = amber::attribution(std::as_const(objects_state.objects));
  auto &models = catalog_state.models;
  auto &scripts = catalog_state.scripts;

  const auto mechanical_root = edit.root;
  auto definitions = replacements.front()->retained;
  auto old_internal = replacements.front()->old_internal;
  std::unordered_set<std::uint64_t> rebound;
  std::vector<lifecycle::Created> created;
  std::vector<std::vector<record::Credit>> removed_credits;
  for (auto *prepared : replacements) {
    const auto &plan = prepared->rows;
    for (const auto &row : plan.retained)
      definitions[row.object.bits] = prepared->retained.at(row.object.bits);
    rebound.insert(prepared->rebound.begin(), prepared->rebound.end());
    removed_credits.push_back(record::credits(identity(objects), attribution, models, scripts,
                                              prepared->removed, 0, call.hz));
    created.push_back(
        lifecycle::apply_replacement(objects, parts, declared_parts, plan, maker, call.step));
    for (std::size_t i = 0; i < created.back().objects.size(); ++i) {
      auto definition = prepared->added[i];
      definition.body.object = created.back().objects[i];
      definitions.emplace(created.back().objects[i].bits, definition);
    }
  }
  const auto after = lifecycle::collect(parts, identity(objects), mechanical_root);
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
      const auto parent = parents.parent[h.slot()];
      const auto old = old_internal.find(h.bits);
      attachments.push_back(
          old != old_internal.end() && !rebound.contains(h.bits) && old->second.parent == parent
              ? old->second
              : space::Attachment{h, parent, relative_pose(transforms, h, parent), false});
    }
  }
  const auto rebuilt = space::rebuild_assembly(objects_state, space_state, edit, bodies, logical,
                                               volumes, attachments);
  if (rebuilt.status != space::Status::Ok)
    throw std::invalid_argument(space::status_text(rebuilt.status));
  perm::update_plots(identity(objects), transforms, membership, land, after, edit.in_scene);
  for (std::size_t i = 0; i < replacements.size(); ++i) {
    const auto &plan = replacements[i]->rows;
    const auto &removed = replacements[i]->removed;
    const auto &made = created[i];
    out.rebound_objects.insert(out.rebound_objects.end(), plan.retained.begin(),
                               plan.retained.end());
    out.created_objects.insert(out.created_objects.end(), made.objects.begin(), made.objects.end());
    out.removed_objects.insert(out.removed_objects.end(), removed.begin(), removed.end());
    auto &visibility = edit.in_scene ? out.visible_objects : out.hidden_objects;
    for (const auto &row : plan.retained)
      visibility.push_back(row.object);
    visibility.insert(visibility.end(), made.objects.begin(), made.objects.end());
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
    event.credits =
        record::credits(identity(objects), attribution, models, scripts, changed, 0, call.hz);
    out.changed_objects.insert(out.changed_objects.end(), changed.begin(), changed.end());
    out.records.push_back(std::move(event));
    if (!removed.empty()) {
      auto destroyed = structural_record(call, record::Kind::Destroy, "parts", removed);
      destroyed.credits = std::move(removed_credits[i]);
      out.records.push_back(std::move(destroyed));
    }
    for (auto h : made.objects)
      out.records.push_back(birth_record(call, h, objects_state, authority.owner, authority.plot,
                                         maker, catalog_state));
  }
}
} // namespace structural_detail

NativeCompletion replace_instance(amber::ObjectState &objects_state,
                                  const amber::catalog::State &catalog_state,
                                  const GeometryAssets &geometry, const react::ValueRead &values,
                                  amber::space::State &space_state,
                                  const amber::perm::State &perm_state, PhysicsDefaults defaults,
                                  const NativeCall &call) {
  auto &space = space_state.space;

  auto &parts = objects_state.parts;

  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;
  auto &assets = catalog_state.assets;
  auto transforms = amber::transforms(objects_state.objects);

  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto &limits = perm_state.owner_limits;
  auto &players = perm_state.players;
  auto &grants = perm_state.grants;
  auto &plots = perm_state.plots;
  auto &ledger = perm_state.ledger;

  using namespace structural_detail;
  const auto identity = values.identity;
  const auto selected = lifecycle::collect(parts, identity, call.request.target);
  const auto error =
      edit_rights(identity, authority, limits, perm_state.plots, players, grants, call, selected);
  if (!error.empty())
    return structural_denied(
        "edit", to_string(call.request.target), error,
        "ask p:" + std::to_string(authority.owner[call.request.target.slot()]) +
            " for Edit on this object and its declared parts");
  const auto input = native_input(call);
  const auto maker = std::holds_alternative<PlayerId>(call.request.caller)
                         ? std::get<PlayerId>(call.request.caller)
                         : authority.owner[input.receiver.slot()];
  const auto binding = instance_binding(call.arguments, models, parameters, values.terms);
  auto assembly = lifecycle::expand(models, parameters, assets, binding.model, binding.params);
  assembly.material = binding.material;
  auto rows = lifecycle::plan_replace(models, parts, identity, transforms, values.params,
                                      call.request.target, assembly, binding.material);
  NativeCompletion out;
  out.reply = {{"ok", true}, {"effect", "none"}};
  if (rows.retained.front().before == rows.retained.front().after)
    return out;
  auto replacement = replacement_geometry(space_state, objects_state, authority.plot, geometry,
                                          catalog_state, values, defaults, call, std::move(rows));
  const auto capacity = space::can_rebuild_assembly(space, replacement.bodies, replacement.sensors,
                                                    replacement.before, replacement.promote);
  if (capacity.status != space::Status::Ok &&
      (replacement.in_scene || capacity.status != space::Status::Capacity))
    return structural_denied("physical_capacity", to_string(call.request.target),
                             space::status_text(capacity.status),
                             "choose a compatible model or remove unused physical objects");
  const auto money = perm::can_refund_object_accounts(ledger, authority, replacement.removed);
  std::string retirement_error;
  if (money != perm::Reason::Allowed)
    retirement_error = perm::reason_text(money);
  if (!retirement_error.empty())
    return structural_denied("retirement", to_string(call.request.target),
                             std::move(retirement_error),
                             "reduce the recipient balance before destroying this account");
  auto edit = space::extract_assembly(space, identity, replacement.mechanical_root,
                                      std::span(replacement.before).subspan(1));
  ReplacementGeometry *row = &replacement;
  rebuild_instances(objects_state, space_state, plots, catalog_state, std::span(&row, 1), edit,
                    maker, call, out);
  return out;
}
} // namespace amber::step
