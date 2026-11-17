#include "private.hpp"

#include <algorithm>
#include <stdexcept>

namespace amber::space {
namespace {
bool member(Handle h, Handle root, std::span<const Handle> descendants) {
  return h == root || std::ranges::find(descendants, h) != descendants.end();
}
} // namespace
OperationResult can_release_occupants(const Space &space, Handle root,
                                      std::span<const Handle> descendants, bool retiring_body,
                                      bool stay_static) {
  const auto &n = *space.native;
  for (const auto &[bits, attachment] : n.attachments) {
    if (!member(attachment.parent, root, descendants) || member({bits}, root, descendants))
      continue;
    const auto body = n.bodies.find(bits);
    if (body == n.bodies.end() || body->second.character ||
        body->second.spec.motion == Motion::Dynamic)
      continue;
    if (stay_static)
      continue;
    if (!n.geometries[body->second.spec.geometry]->hull)
      return {Status::UnsupportedMotion, {bits}};
    if (!retiring_body && capacity(space).native_available == 0)
      return {Status::Capacity, {bits}};
  }
  return {Status::Ok, {}};
}
OperationResult release_occupants(Space &space, IdentityRead identity, Handle root,
                                  std::span<const Handle> descendants, ParentWrite parents,
                                  SpaceFlags flags, PhysicalRelations &relations,
                                  std::vector<RelationChange> &changes, bool stay_static) {
  auto &n = *space.native;
  for (auto it = n.attachments.begin(); it != n.attachments.end();) {
    const Handle h{it->first};
    if (!member(it->second.parent, root, descendants) || member(h, root, descendants)) {
      ++it;
      continue;
    }
    const auto parent = it->second.parent;
    it = n.attachments.erase(it);
    parents.parent[h.slot()] = {};
    const auto found = n.bodies.find(h.bits);
    auto mode = Motion::Static;
    if (found != n.bodies.end()) {
      auto &body = found->second;
      const auto old = n.world_bounds(body);
      mode = body.character ? Motion::Character : Motion::Dynamic;
      if (stay_static && !body.character && body.spec.motion != Motion::Dynamic &&
          (!n.geometries[body.spec.geometry]->hull || capacity(space).native_available == 0)) {
        mode = Motion::Static;
        n.physics.GetBodyInterface().SetMotionType(body.body, JPH::EMotionType::Static,
                                                   JPH::EActivation::DontActivate);
        body.spec.motion = mode;
        body.kinematic_start.reset();
      }
      const auto ready = update_motion(n, body, mode);
      if (ready.status != Status::Ok)
        return ready;
      n.physics.GetBodyInterface().SetObjectLayer(
          body.body, body.character ? 3 : (body.spec.collision_solid ? layer(mode) : 4));
      n.changed(h, old, n.world_bounds(body), navigates(body));
    } else if (identity.valid(h))
      n.dirty_objects.insert(h.bits);
    flags_for(flags, h, mode);
    edge(relations.attached_to, RelationKind::AttachedTo, h, parent, false, changes);
  }
  return {Status::Ok, {}};
}
OperationResult can_visible_custody(const Space &space, IdentityRead identity, ParentRead parents,
                                    const VisibleCustody &change,
                                    std::span<const Handle> descendants) {
  const auto checked =
      can_custody(space, identity, change.goods, change.parent, parents, descendants);
  if (checked.status != Status::Ok)
    return checked;
  const auto &n = *space.native;
  if (n.compound_parent.contains(change.goods.bits))
    return {Status::UnsupportedMotion, {}};
  const auto body = n.bodies.find(change.goods.bits);
  if (body == n.bodies.end()) {
    if (!n.logical_roots.contains(change.goods.bits) ||
        ((identity.flags[change.goods.slot()] & Held) &&
         !n.attachments.contains(change.goods.bits) &&
         std::ranges::none_of(descendants, [&](Handle h) { return n.bodies.contains(h.bits); })))
      return {Status::MissingBody, {}};
  } else if (change.parent && !body->second.character) {
    if (!n.geometries[body->second.spec.geometry]->hull)
      return {Status::UnsupportedMotion, {}};
    if (body->second.spec.motion != Motion::Kinematic && capacity(space).native_available == 0)
      return {Status::Capacity, {}};
  }
  return can_release_occupants(space, change.goods, descendants, false);
}
OperationResult visible_custody(amber::ObjectState &objects_state, amber::space::State &space_state,
                                const VisibleCustody &change, std::span<const Handle> descendants) {
  auto &space = space_state.space;

  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto transforms = amber::transforms(std::as_const(objects_state.objects));
  auto parents = amber::parents(objects_state.objects);
  auto flags = amber::space_flags(objects_state.objects);
  auto &relations = space_state.physical_relations;
  auto &changes = space_state.relation_changes;

  const auto checked = can_visible_custody(space, identity, parents, change, descendants);
  if (checked.status != Status::Ok)
    return checked;
  auto &n = *space.native;
  const auto released = release_occupants(space, identity, change.goods, descendants, parents,
                                          flags, relations, changes);
  if (released.status != Status::Ok)
    return released;
  const auto old = n.attachments.find(change.goods.bits);
  if (old != n.attachments.end()) {
    edge(relations.attached_to, RelationKind::AttachedTo, change.goods, old->second.parent, false,
         changes);
    n.attachments.erase(old);
  }
  erase_edges(relations.contains, RelationKind::Contains, change.goods, changes);
  erase_edges(relations.supports, RelationKind::Supports, change.goods, changes);
  const auto found = n.bodies.find(change.goods.bits);
  auto mode = Motion::Static;
  if (found != n.bodies.end()) {
    auto &body = found->second;
    const auto bounds = n.world_bounds(body);
    mode = change.parent && !body.character ? Motion::Kinematic : body.spec.motion;
    const auto ready = update_motion(n, body, mode);
    if (ready.status != Status::Ok)
      return ready;
    const bool attached_query = change.parent && (change.grasp || body.character);
    n.physics.GetBodyInterface().SetObjectLayer(body.body,
                                                attached_query || !body.spec.collision_solid ? 4
                                                : body.character ? 3
                                                                 : layer(mode));
    n.changed(change.goods, bounds, n.world_bounds(body), navigates(body));
  } else
    mode = change.parent ? Motion::Kinematic : Motion::Static;
  parents.parent[change.goods.slot()] = change.parent;
  flags_for(flags, change.goods, mode, change.parent && change.grasp);
  if (change.parent) {
    const auto local = relative(
        {transforms.position[change.parent.slot()], transforms.rotation[change.parent.slot()]},
        {transforms.position[change.goods.slot()], transforms.rotation[change.goods.slot()]});
    n.attachments[change.goods.bits] = {change.goods, change.parent, local, change.grasp};
    edge(relations.attached_to, RelationKind::AttachedTo, change.goods, change.parent, true,
         changes);
  }
  n.dirty_objects.insert(change.goods.bits);
  return {Status::Ok, {}};
}
} // namespace amber::space
