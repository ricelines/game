#include "bindings/scene.hpp"
#include "private.hpp"
#include "react/exchange.hpp"

#include <algorithm>
#include <stdexcept>

namespace amber::bindings {
namespace structural_detail {
ScriptKey holder(const drive::StructuralCall &call) {
  return {call.invocation.receiver, call.handler.script};
}
perm::Holder permission_holder(Caller caller) {
  if (const auto *player = std::get_if<PlayerId>(&caller))
    return perm::Holder::player(*player);
  const auto &script = std::get<ScriptHolder>(caller);
  return perm::Holder::script(script.script, script.object);
}
bool declared_descendant(const Relation &parts, IdentityRead identity, Handle root, Handle child) {
  std::vector<Handle> pending{root};
  for (std::size_t i = 0; i < pending.size(); ++i) {
    if (pending[i] == child)
      return true;
    if (const auto *children = parts.out.get(pending[i].slot()))
      for (auto h : *children)
        if (identity.valid(h))
          pending.push_back(h);
  }
  return false;
}
std::string destroy_right(const StructuralRights &rights, const drive::StructuralCall &call) {
  const auto target = call.request.target;
  if (!rights.identity.valid(target))
    return "destroy target is no longer alive";
  if (call.handler.world_rule &&
      declared_descendant(rights.declared_parts, rights.identity, call.invocation.receiver, target))
    return {};
  Caller actor = ScriptHolder{call.handler.script, call.invocation.receiver};
  Handle actor_body = call.invocation.receiver;
  if (call.invocation.action_caller && target == call.invocation.action_target) {
    actor = *call.invocation.action_caller;
    if (std::holds_alternative<PlayerId>(actor))
      actor_body = call.invocation.action_body;
  }
  const auto decision =
      perm::check(permission_holder(actor), perm::Verb::Destroy,
                  perm::target(rights.identity, rights.authority, rights.limits, target),
                  rights.players, rights.grants, call.invocation.step);
  if (!decision)
    return "destroy: " + std::string(perm::reason_text(decision.reason));
  // Scheduled granted scripts may act on their stored goods. Immediate chosen
  // destruction is physical and checks the target's actual full bounds.
  if (call.invocation.action_caller && target == call.invocation.action_target) {
    if (!rights.identity.valid(actor_body))
      return "destroy actor body is no longer alive";
    const auto bounds = space::bounds(rights.space, rights.identity, target);
    if (!bounds || !drive::in_reach(rights.transforms.position[actor_body.slot()],
                                    {bounds->min, bounds->max}, rights.reach_m))
      return "destroy target is out of reach";
  }
  return {};
}
const perm::Plot *placement_plot(const StructuralRights &rights, Vec3 position) {
  for (const auto &plot : rights.plots.rows)
    if (plot.id && position.x >= plot.bounds[0] && position.y >= plot.bounds[1] &&
        position.x < plot.bounds[2] && position.y < plot.bounds[3])
      return &plot;
  return nullptr;
}
record::Event structural_record(const drive::StructuralCall &call, record::Kind kind,
                                std::string name, std::vector<Handle> objects) {
  record::Event event;
  event.step = call.invocation.step;
  event.kind = kind;
  event.actor = record::Actor::script(call.handler.script);
  event.script_author = call.handler.author;
  event.name = std::move(name);
  event.result = record::Result::Applied;
  event.changed = true;
  event.things = std::move(objects);
  return event;
}
void completed_effects(drive::StructuralCompletion &out, react::DeferredEffects effects) {
  out.effects.insert(out.effects.end(), std::make_move_iterator(effects.ordered.begin()),
                     std::make_move_iterator(effects.ordered.end()));
}
space::Pose relative_pose(TransformRead transforms, Handle subject, Handle parent) {
  const auto rotation = transforms.rotation[parent.slot()];
  const Quat inverse{-rotation.x, -rotation.y, -rotation.z, rotation.w};
  return {rotate(inverse, transforms.position[subject.slot()] - transforms.position[parent.slot()]),
          compose(inverse, transforms.rotation[subject.slot()])};
}
record::Event birth_record(const drive::StructuralCall &call, Handle object, IdentityRead identity,
                           const std::vector<PlayerId> &owners, const std::vector<PlotId> &plots,
                           const std::vector<std::uint32_t> &params, PlayerId maker,
                           CreditResolver credits) {
  auto event = structural_record(call, record::Kind::Create, "instance", {object});
  const auto slot = object.slot();
  event.detail = {{"model", "m:" + std::to_string(identity.model[slot])},
                  {"params", params[slot]},
                  {"material", identity.material[slot]},
                  {"maker", "p:" + std::to_string(maker)},
                  {"owner", "p:" + std::to_string(owners[slot])},
                  {"plot", plots[slot]}};
  if (credits.resolve)
    event.credits = credits.resolve(credits.context, std::span(&object, 1), 0, call.invocation.hz);
  return event;
}
} // namespace structural_detail
using namespace structural_detail;
void install_declared_attachments(AssemblyPlacement &placement, std::span<const Handle> objects) {
  for (auto h : objects) {
    const auto parent = placement.parents.parent[h.slot()];
    const auto *declared = placement.declared_parts.in.get(h.slot());
    if (!parent || !declared || std::ranges::find(*declared, parent) == declared->end())
      continue;
    const auto result = space::attach(
        placement.space, placement.identity, placement.transforms, placement.parents,
        placement.flags, placement.physical.contains, placement.physical.supports,
        placement.physical.attached_to,
        {h, parent, relative_pose(placement.transforms, h, parent), false}, placement.changes);
    if (result.status != space::Status::Ok)
      throw std::logic_error("declared attachment installation failed: " +
                             std::string(space::status_text(result.status)));
  }
}
std::string check_resources(const ResourceCommit &resources, const drive::StructuralCall &call) {
  if (!call.request.exchange)
    return {};
  const auto &exchange = *call.request.exchange;
  if (exchange.holder != holder(call) || !resources.identity.valid(exchange.holder.object))
    return "resource exchange holder is no longer the invoking installation";
  const auto found = resources.states.find(exchange.holder);
  if (found == resources.states.end())
    return "resource exchange state is missing";
  return react::check_exchange(resources.properties, resources.processes, found->second,
                               resources.terms, exchange, call.invocation.step, call.invocation.hz,
                               react::state_capacity(resources.vm, exchange.holder.script));
}
void commit_resources(ResourceCommit &resources, const drive::StructuralCall &call,
                      react::DeferredEffects &effects) {
  if (!call.request.exchange)
    return;
  const auto &exchange = *call.request.exchange;
  std::string error;
  if (!react::apply_exchange(
          resources.properties, resources.processes, resources.states.at(exchange.holder),
          resources.terms, exchange, call.invocation.step, call.invocation.hz,
          react::state_capacity(resources.vm, exchange.holder.script), effects, error))
    throw std::logic_error("preflighted resource commit failed: " + error);
}
drive::StructuralCompletion create_instance(void *context, const drive::StructuralCall &call) {
  auto &operation = *static_cast<CreateOperation *>(context);
  drive::StructuralCompletion out;
  const auto &rights = operation.rights;
  if (!call.handler.world_rule || !rights.identity.valid(call.invocation.receiver)) {
    out.error = "creation requires a live approved world-rule receiver";
    return out;
  }
  const auto *plot = placement_plot(rights, call.request.position);
  if (!plot) {
    out.error = "creation position has no land parcel";
    return out;
  }
  Caller actor = ScriptHolder{call.handler.script, call.invocation.receiver};
  PlayerId owner = rights.authority.owner[call.invocation.receiver.slot()];
  if (call.invocation.action_caller) {
    actor = *call.invocation.action_caller;
    if (const auto *player = std::get_if<PlayerId>(&actor))
      owner = *player;
  }
  const auto decision = perm::check_land(permission_holder(actor), perm::LandVerb::Build, *plot,
                                         rights.players, rights.grants, call.invocation.step);
  if (!decision) {
    out.error = "build: " + std::string(perm::reason_text(decision.reason));
    return out;
  }
  out.error = check_resources(operation.resources, call);
  if (!out.error.empty())
    return out;
  auto assembly = lifecycle::expand(operation.catalog.models, operation.catalog.parameters,
                                    operation.catalog.assets, call.request.binding.model,
                                    call.request.binding.params);
  assembly.material = call.request.binding.material;
  std::vector<PhysicalDefinition> definitions;
  std::vector<space::BodySpec> bodies;
  std::size_t sensors = 0;
  auto prepare = [&](ModelId model, std::uint32_t params, TermId material, bool part) {
    const auto effective = catalog::resolve_asset(operation.catalog.models,
                                                  operation.catalog.parameters, model, params);
    const auto &asset = operation.catalog.assets.rows.at(effective.hash);
    auto definition =
        describe_physics(operation.geometry.geometry, asset, operation.geometry.values,
                         {model, params, material, plot->id, {}}, Static, operation.geometry.roles,
                         operation.geometry.defaults, call.invocation.step, call.invocation.hz);
    if (part) {
      definition.body.motion = space::Motion::Kinematic;
      definition.mode_flags = ScriptedMotion;
    }
    if (definition.body.geometry)
      bodies.push_back(definition.body);
    sensors += definition.volumes->size();
    definitions.push_back(definition);
  };
  prepare(assembly.model, assembly.params, assembly.material, false);
  for (const auto &part : assembly.parts)
    prepare(part.model, 0, part.material, true);
  const auto capacity = space::can_add(operation.geometry.space, bodies, sensors);
  if (capacity.status != space::Status::Ok) {
    out.error = "creation physics: " + std::string(space::status_text(capacity.status));
    return out;
  }
  ObjectInit init;
  init.position = call.request.position;
  init.rotation = call.request.rotation;
  init.owner = owner;
  init.maker = owner;
  init.created_step = call.invocation.step;
  init.plot = plot->id;
  init.preset = perm::all_rights << 16;
  auto created = operation.instances.create(assembly, init);
  for (std::size_t i = 0; i < created.objects.size(); ++i)
    install_physics(operation.geometry.space, rights.identity, operation.geometry.transforms,
                    operation.geometry.flags, created.objects[i], definitions[i]);
  install_declared_attachments(operation.placement, created.objects);
  react::DeferredEffects effects;
  commit_resources(operation.resources, call, effects);
  objects_created(operation.lifecycle, created.objects, true, call.invocation, effects);
  out.changed = true;
  out.created = created.objects;
  for (auto h : created.objects)
    out.records.push_back(birth_record(call, h, rights.identity, rights.authority.owner,
                                       rights.authority.plot, operation.geometry.values.params,
                                       owner, operation.credits));
  completed_effects(out, std::move(effects));
  return out;
}
drive::StructuralCompletion remove_instance(void *context, const drive::StructuralCall &call) {
  auto &operation = *static_cast<RemoveOperation *>(context);
  drive::StructuralCompletion out;
  out.error = destroy_right(operation.rights, call);
  if (!out.error.empty())
    return out;
  auto involved = operation.instances.closure(std::span{&call.request.target, 1});
  std::vector<record::Credit> object_credits;
  if (operation.credits.resolve)
    object_credits =
        operation.credits.resolve(operation.credits.context, involved, 0, call.invocation.hz);
  out.error = check_resources(operation.resources, call);
  if (!out.error.empty() || !can_retire_objects(operation.lifecycle, involved, out.error))
    return out;
  react::DeferredEffects effects;
  commit_resources(operation.resources, call, effects);
  retire_objects(operation.lifecycle, involved, call.invocation, effects);
  out.changed = !involved.empty();
  if (out.changed) {
    auto event = structural_record(call, record::Kind::Destroy, "instance", involved);
    event.credits = std::move(object_credits);
    out.records.push_back(std::move(event));
  }
  completed_effects(out, std::move(effects));
  return out;
}
} // namespace amber::bindings
