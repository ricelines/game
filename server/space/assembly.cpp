#include "private.hpp"
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace amber::space {
namespace {
OperationResult assemble_at(Space &space, IdentityRead identity, Pose root_pose, Handle root,
                            std::span<const Handle> descendants, std::optional<Motion> mode) {
  auto &n = *space.native;
  if (!identity.valid(root))
    return {Status::StaleHandle, {}};
  auto own = n.bodies.find(root.bits);
  if (own == n.bodies.end() && !n.logical_roots.contains(root.bits))
    return {Status::MissingGeometry, {}};
  if (own != n.bodies.end() && !own->second.compound_parts.empty())
    return {Status::Ok, {}};
  if (n.compound_parent.contains(root.bits) || (own != n.bodies.end() && own->second.character))
    return {Status::UnsupportedMotion, {}};
  JPH::StaticCompoundShapeSettings solid_shapes, query_shapes;
  std::vector<Handle> physical;
  float mass = 0, solid_mass = 0, friction = 0, restitution = 0;
  auto append = [&](const Mirror &body, Pose local) -> bool {
    const auto &hull = n.geometries[body.spec.geometry]->hull;
    if (!hull || hull->MustBeStatic())
      return false;
    query_shapes.AddShape(jv(local.position), jq(local.rotation), hull.GetPtr(),
                          body.spec.object.slot());
    mass += body.spec.mass_kg;
    if (body.spec.collision_solid) {
      solid_shapes.AddShape(jv(local.position), jq(local.rotation), hull.GetPtr(),
                            body.spec.object.slot());
      solid_mass += body.spec.mass_kg;
      friction += body.spec.mass_kg * body.spec.surface.friction;
      restitution = std::max(restitution, body.spec.surface.restitution);
    }
    return true;
  };
  if (own != n.bodies.end() && !append(own->second, {}))
    return {Status::UnsupportedMotion, root};
  for (auto h : descendants) {
    if (!identity.valid(h))
      return {Status::StaleHandle, h};
    auto found = n.bodies.find(h.bits);
    if (found == n.bodies.end()) {
      if (n.logical_roots.contains(h.bits))
        continue;
      return {Status::MissingBody, h};
    }
    auto &body = found->second;
    if (body.character || n.compound_parent.contains(h.bits) ||
        !n.geometries[body.spec.geometry]->hull)
      return {Status::UnsupportedMotion, h};
    auto ancestor = h;
    while (ancestor != root) {
      auto attached = n.attachments.find(ancestor.bits);
      if (attached == n.attachments.end())
        return {Status::NotAttached, h};
      ancestor = attached->second.parent;
    }
    if (!append(body, relative(root_pose, body.pose)))
      return {Status::UnsupportedMotion, h};
    physical.push_back(h);
  }
  if (physical.empty())
    return {own == n.bodies.end() ? Status::MissingGeometry : Status::Ok, {}};
  if (!(mass > 0) || !std::isfinite(mass))
    return {Status::UnsupportedMotion, {}};
  if (capacity(space).native_available == 0)
    return {Status::Capacity, {}};
  auto shape = (solid_mass > 0 ? solid_shapes : query_shapes).Create(n.temp);
  if (shape.HasError() || shape.Get()->MustBeStatic())
    return {Status::UnsupportedMotion, {}};
  auto geometry = std::make_shared<Geometry>();
  geometry->mesh = shape.Get();
  geometry->hull = shape.Get();
  geometry->bounds = shape_bounds(shape.Get(), {});
  GeometryId id;
  if (auto cached = n.compound_geometry.find(root.bits); cached != n.compound_geometry.end()) {
    id = cached->second;
    n.geometries[id] = geometry;
  } else {
    if (!n.free_compound_geometry.empty()) {
      id = n.free_compound_geometry.back();
      n.free_compound_geometry.pop_back();
      n.geometries[id] = geometry;
    } else {
      id = GeometryId(n.geometries.size());
      n.geometries.push_back(geometry);
    }
    n.compound_geometry[root.bits] = id;
  }
  BodySpec combined{root,
                    id,
                    mode.value_or(own == n.bodies.end() ? Motion::Static : own->second.spec.motion),
                    {solid_mass > 0 ? friction / solid_mass : 0, restitution, 1},
                    mass,
                    false,
                    false,
                    {},
                    solid_mass > 0};
  if (combined.motion == Motion::Character)
    return {Status::UnsupportedMotion, root};
  if (own == n.bodies.end()) {
    Mirror aggregate{};
    aggregate.spec = combined;
    aggregate.pose = root_pose;
    aggregate.body = create_rigid(n, aggregate, combined.motion);
    if (aggregate.body.IsInvalid()) {
      return {Status::Capacity, {}};
    }
    n.handles[aggregate.body.GetIndexAndSequenceNumber()] = root;
    auto [inserted, _] = n.bodies.emplace(root.bits, std::move(aggregate));
    const auto bounds = n.world_bounds(inserted->second);
    n.changed(root, bounds, bounds, false);
  } else {
    Mirror combined_body{};
    combined_body.spec = combined;
    combined_body.pose = root_pose;
    combined_body.body = create_rigid(n, combined_body, combined.motion);
    if (combined_body.body.IsInvalid()) {
      return {Status::Capacity, {}};
    }
    const auto previous = own->second.spec;
    const auto previous_bounds = n.world_bounds(own->second);
    auto linear = n.physics.GetBodyInterface().GetLinearVelocity(own->second.body),
         angular = n.physics.GetBodyInterface().GetAngularVelocity(own->second.body);
    query_shape(n, own->second, true);
    combined_body.own_geometry = Mirror::OwnGeometry{previous, own->second.body};
    n.handles[combined_body.body.GetIndexAndSequenceNumber()] = root;
    own->second = std::move(combined_body);
    if (previous.motion == combined.motion)
      n.physics.GetBodyInterface().SetLinearAndAngularVelocity(own->second.body, linear, angular);
    n.changed(root, previous_bounds, n.world_bounds(own->second), false);
    if (previous.navigation_surface || previous.navigation_obstacle)
      n.dirty_navigation(previous_bounds);
  }
  auto &body = n.bodies.at(root.bits);
  body.compound_parts.assign(descendants.begin(), descendants.end());
  if (auto attached = n.attachments.find(root.bits); attached != n.attachments.end())
    n.physics.GetBodyInterface().SetObjectLayer(
        body.body, attached->second.grasp || !body.spec.collision_solid ? 4 : 1);
  for (auto h : descendants) {
    n.compound_parent[h.bits] = root.bits;
    auto found = n.bodies.find(h.bits);
    if (found == n.bodies.end())
      continue;
    auto &part = found->second;
    query_shape(n, part, true);
    if (navigates(part))
      n.dirty_navigation(n.world_bounds(part));
  }
  return {Status::Ok, {}};
}
} // namespace
OperationResult assemble(Space &space, IdentityRead identity, TransformRead transforms, Handle root,
                         std::span<const Handle> descendants, std::optional<Motion> mode) {
  if (!identity.valid(root))
    return {Status::StaleHandle, {}};
  return assemble_at(space, identity,
                     {transforms.position[root.slot()], transforms.rotation[root.slot()]}, root,
                     descendants, mode);
}

void remove_compound_member(Space &space, IdentityRead identity, SpaceFlags flags, Handle member) {
  auto &n = *space.native;
  auto parent = n.compound_parent.find(member.bits);
  if (parent == n.compound_parent.end())
    return;
  Handle root{parent->second};
  auto &aggregate = n.bodies.at(root.bits);
  const auto mode = aggregate.spec.motion;
  const auto pose = aggregate.pose;
  const auto own = aggregate.own_geometry;
  auto &interface = n.physics.GetBodyInterface();
  const auto linear = interface.GetLinearVelocity(aggregate.body),
             angular = interface.GetAngularVelocity(aggregate.body);
  std::vector<Handle> remaining;
  for (auto child : aggregate.compound_parts) {
    auto ancestor = child;
    while (ancestor && ancestor != root && ancestor != member) {
      auto attached = n.attachments.find(ancestor.bits);
      ancestor = attached == n.attachments.end() ? Handle{} : attached->second.parent;
    }
    if (ancestor != member)
      remaining.push_back(child);
  }
  extract_native(n, root);
  if (own) {
    Mirror restored{};
    restored.spec = own->spec;
    restored.spec.motion = mode;
    restored.pose = pose;
    restored.body = create_rigid(n, restored, mode);
    if (restored.body.IsInvalid())
      throw std::runtime_error("preexisting compound capacity unavailable during removal");
    n.handles[restored.body.GetIndexAndSequenceNumber()] = root;
    auto [inserted, _] = n.bodies.emplace(root.bits, std::move(restored));
    const auto bounds = n.world_bounds(inserted->second);
    n.changed(root, bounds, bounds, navigates(inserted->second));
  }
  const bool physical =
      std::ranges::any_of(remaining, [&](Handle h) { return n.bodies.contains(h.bits); });
  if (physical) {
    auto result = assemble_at(space, identity, pose, root, remaining, mode);
    if (result.status != Status::Ok)
      throw std::runtime_error("preexisting assembly could not rebuild after member removal");
  }
  auto current = n.bodies.find(root.bits);
  if (current != n.bodies.end()) {
    interface.SetLinearAndAngularVelocity(current->second.body, linear, angular);
    const auto attachment = n.attachments.find(root.bits);
    const bool grasp = attachment != n.attachments.end() && attachment->second.grasp;
    if (grasp)
      interface.SetObjectLayer(current->second.body, 4);
    flags_for(flags, root, mode, grasp);
  } else {
    const auto attachment = n.attachments.find(root.bits);
    flags_for(flags, root, attachment == n.attachments.end() ? Motion::Static : Motion::Kinematic,
              attachment != n.attachments.end() && attachment->second.grasp);
  }
}
Handle assembly_root(const Space &space, IdentityRead identity, Handle object) {
  if (!identity.valid(object))
    return {};
  const auto &n = *space.native;
  auto parent = n.compound_parent.find(object.bits);
  return parent == n.compound_parent.end() ? object : Handle{parent->second};
}

namespace {
bool complete_scope(const Space::Native &n, const std::unordered_set<std::uint64_t> &scope) {
  for (auto bits : scope) {
    if (auto parent = n.compound_parent.find(bits);
        parent != n.compound_parent.end() && !scope.contains(parent->second))
      return false;
    if (auto body = n.bodies.find(bits); body != n.bodies.end())
      for (auto child : body->second.compound_parts)
        if (!scope.contains(child.bits))
          return false;
  }
  return true;
}
} // namespace

BodyPreflight can_rebuild_assembly(const Space &space, std::span<const BodySpec> bodies,
                                   std::size_t sensors, std::span<const Handle> before,
                                   bool promoted, std::optional<Motion> root_motion) {
  const auto &n = *space.native;
  std::unordered_set<std::uint64_t> scope;
  for (auto h : before)
    scope.insert(h.bits);
  if (!complete_scope(n, scope))
    return {Status::UnsupportedMotion, 0, bodies.size() + sensors,
            capacity(space).native_available};
  if (promoted && bodies.empty())
    return {Status::MissingGeometry, 0, sensors, capacity(space).native_available};
  const bool physical_root = !before.empty() && std::ranges::any_of(bodies, [&](const BodySpec &b) {
    return b.object == before.front();
  });
  const bool aggregate = promoted && (bodies.size() > (physical_root ? 1u : 0u));
  std::vector<BodySpec> effective(bodies.begin(), bodies.end());
  double mass = 0;
  for (auto &spec : effective) {
    if (aggregate) {
      if (spec.motion == Motion::Character)
        return {Status::UnsupportedMotion, std::size_t(&spec - effective.data()),
                bodies.size() + sensors + 1, capacity(space).native_available};
      spec.motion = Motion::Kinematic;
      mass += spec.mass_kg;
    } else if (root_motion && !before.empty() && spec.object == before.front())
      spec.motion = *root_motion;
    else if (auto old = n.bodies.find(spec.object.bits); old != n.bodies.end())
      spec.motion = old->second.spec.motion;
  }
  if (aggregate && (!std::isfinite(mass) || mass <= 0 || mass > std::numeric_limits<float>::max()))
    return {Status::UnsupportedMotion, 0, bodies.size() + sensors + 1,
            capacity(space).native_available};
  return can_add(space, effective, sensors + (aggregate ? 1 : 0), before);
}

AssemblyEdit extract_assembly(Space &space, IdentityRead identity, Handle root,
                              std::span<const Handle> descendants) {
  auto &n = *space.native;
  AssemblyEdit edit{root, false, false, {root}, {}};
  edit.before.insert(edit.before.end(), descendants.begin(), descendants.end());
  std::unordered_set<std::uint64_t> scope;
  for (auto h : edit.before)
    if (!identity.valid(h) || !scope.insert(h.bits).second)
      throw std::invalid_argument("assembly extraction requires unique live handles");
  if (!complete_scope(n, scope))
    throw std::invalid_argument("assembly extraction requires the complete rigid assembly");
  const auto body = n.bodies.find(root.bits);
  edit.in_scene = body != n.bodies.end() || !(identity.flags[root.slot()] & Held);
  edit.promoted = body != n.bodies.end() && !body->second.compound_parts.empty();
  auto &interface = n.physics.GetBodyInterface();
  for (auto h : edit.before) {
    EditedMotion motion{h, Motion::Static, {}, {}, true};
    auto found = n.bodies.find(h.bits);
    if (found != n.bodies.end()) {
      motion.motion = found->second.spec.motion;
      if (found->second.character) {
        motion.linear_velocity_m_s = av(found->second.character->GetLinearVelocity());
        motion.asleep = false;
      } else if (motion.motion == Motion::Dynamic) {
        motion.linear_velocity_m_s = av(interface.GetLinearVelocity(found->second.body));
        motion.angular_velocity_rad_s = av(interface.GetAngularVelocity(found->second.body));
        motion.asleep = !interface.IsActive(found->second.body);
      }
    } else if (identity.flags[h.slot()] & ScriptedMotion)
      motion.motion = Motion::Kinematic;
    edit.motion.push_back(motion);
  }
  // Root first: restore individual query shapes before removing their own bodies.
  for (auto h : edit.before)
    extract_native(n, h);
  for (auto bits : scope)
    remove_volumes(n, {bits});
  ++n.volume_revision;
  return edit;
}

OperationResult rebuild_assembly(amber::ObjectState &objects_state,
                                 amber::space::State &space_state, const AssemblyEdit &edit,
                                 std::span<const BodySpec> bodies,
                                 std::span<const Handle> logical_roots,
                                 std::span<const VolumeSpec> volumes,
                                 std::span<const Attachment> internal) {
  auto &space = space_state.space;

  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto transforms = amber::transforms(objects_state.objects);
  auto parents = amber::parents(objects_state.objects);
  auto flags = amber::space_flags(objects_state.objects);
  auto &relations = space_state.physical_relations;
  auto &changes = space_state.relation_changes;

  auto &n = *space.native;
  if (!identity.valid(edit.root))
    return {Status::StaleHandle, edit.root};
  std::vector<Handle> members{edit.root};
  std::unordered_set<std::uint64_t> scope{edit.root.bits};
  for (const auto &spec : bodies)
    if (scope.insert(spec.object.bits).second)
      members.push_back(spec.object);
  for (auto h : logical_roots)
    if (scope.insert(h.bits).second)
      members.push_back(h);
  for (auto h : members)
    if (!identity.valid(h))
      return {Status::StaleHandle, h};
  for (auto h : edit.before) {
    n.logical_roots.erase(h.bits);
    if (h != edit.root)
      n.attachments.erase(h.bits);
  }
  for (auto h : logical_roots)
    n.logical_roots.insert(h.bits);
  auto root_attachment = n.attachments.find(edit.root.bits);
  const bool root_attached = root_attachment != n.attachments.end();
  const bool root_grasped = root_attached && root_attachment->second.grasp;
  auto saved = [&](Handle h) -> const EditedMotion * {
    auto found = std::ranges::find(edit.motion, h, &EditedMotion::object);
    return found == edit.motion.end() ? nullptr : &*found;
  };
  auto root_mode = saved(edit.root) ? saved(edit.root)->motion : Motion::Static;
  const bool physical_descendants =
      std::ranges::any_of(bodies, [&](const BodySpec &body) { return body.object != edit.root; });
  const bool promote =
      edit.promoted || (physical_descendants && (root_grasped || root_mode == Motion::Dynamic));
  std::unordered_set<std::uint64_t> internal_subjects;
  for (const auto &attachment : internal) {
    if (!scope.contains(attachment.subject.bits) || !scope.contains(attachment.parent.bits) ||
        attachment.subject == edit.root)
      return {Status::UnsupportedMotion, attachment.subject};
    internal_subjects.insert(attachment.subject.bits);
  }
  // Parent/edge rows are authoritative even for assemblies stored off-scene.
  for (auto h : members) {
    if (h == edit.root)
      continue;
    const auto *row = relations.attached_to.out.get(h.slot());
    const auto old = row ? *row : std::vector<Handle>{};
    for (auto parent : old)
      edge(relations.attached_to, RelationKind::AttachedTo, h, parent, false, changes);
  }
  if (!edit.in_scene) {
    n.attachments.erase(edit.root.bits);
    for (auto h : members)
      flags.set(h.slot(), Held);
    for (const auto &attachment : internal) {
      parents.parent[attachment.subject.slot()] = attachment.parent;
      edge(relations.attached_to, RelationKind::AttachedTo, attachment.subject, attachment.parent,
           true, changes);
    }
    return {Status::Ok, {}};
  }
  for (auto spec : bodies) {
    if (const auto *previous = saved(spec.object))
      spec.motion = previous->motion;
    if (internal_subjects.contains(spec.object.bits) || (spec.object == edit.root && root_attached))
      if (spec.motion != Motion::Character)
        spec.motion = Motion::Kinematic;
    auto result = add(space, identity, {transforms.position, transforms.rotation}, spec);
    if (result.status != Status::Ok)
      return result;
    flags_for(flags, spec.object, spec.motion);
  }
  for (auto h : logical_roots)
    flags_for(flags, h,
              internal_subjects.contains(h.bits) || (h == edit.root && root_attached)
                  ? Motion::Kinematic
                  : Motion::Static);
  for (const auto &attachment : internal) {
    auto result = attach(objects_state, space_state, attachment);
    if (result.status != Status::Ok)
      return result;
  }
  // The old external root attachment record was deliberately retained by extraction.
  if (root_attached) {
    auto root = n.bodies.find(edit.root.bits);
    if (root != n.bodies.end())
      n.physics.GetBodyInterface().SetObjectLayer(
          root->second.body,
          root_grasped || root->second.character || !root->second.spec.collision_solid ? 4 : 1);
    flags_for(flags, edit.root, root_mode, root_grasped);
  }
  if (promote) {
    const auto desired = root_attached ? Motion::Kinematic : root_mode;
    auto result = assemble(space, identity, {transforms.position, transforms.rotation}, edit.root,
                           std::span(members).subspan(1), desired);
    if (result.status != Status::Ok)
      return result;
    flags_for(flags, edit.root, desired, root_grasped);
  }
  for (const auto &volume : volumes) {
    auto result = add_volume(space, identity, {transforms.position, transforms.rotation}, volume);
    if (result.status != Status::Ok)
      return result;
  }
  std::vector<MotionRow> velocities;
  for (const auto &motion : edit.motion)
    if (scope.contains(motion.object.bits) &&
        (motion.motion == Motion::Dynamic || motion.motion == Motion::Character))
      velocities.push_back({motion.object, motion.linear_velocity_m_s,
                            motion.angular_velocity_rad_s, motion.asleep});
  restore_motion(space, identity, velocities);
  // Follow retained occupants immediately; no global physics step or world scan is needed.
  std::unordered_set<std::uint64_t> followed;
  auto follow = [&](auto &&self, Handle subject) -> void {
    if (!followed.insert(subject.bits).second)
      return;
    auto attached = n.attachments.find(subject.bits);
    if (attached == n.attachments.end() || !identity.valid(attached->second.parent))
      return;
    auto parent = attached->second.parent;
    self(self, parent);
    auto target = compose({transforms.position[parent.slot()], transforms.rotation[parent.slot()]},
                          attached->second.local_pose);
    if (auto body = n.bodies.find(subject.bits); body != n.bodies.end()) {
      auto previous = n.world_bounds(body->second);
      native_pose(n, body->second, target);
      n.changed(subject, previous, n.world_bounds(body->second), navigates(body->second));
    } else
      n.dirty_objects.insert(subject.bits);
    transforms.position[subject.slot()] = target.position;
    transforms.rotation[subject.slot()] = target.rotation;
  };
  for (std::size_t i = 0; i < members.size(); ++i) {
    follow(follow, members[i]);
    if (const auto *children = relations.attached_to.in.get(members[i].slot()))
      for (auto child : *children)
        if (scope.insert(child.bits).second)
          members.push_back(child);
  }
  return {Status::Ok, {}};
}

} // namespace amber::space
