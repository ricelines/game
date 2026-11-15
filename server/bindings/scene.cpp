#include "scene.hpp"

#include "commerce/commerce.hpp"
#include "illumination.hpp"
#include "installation/installation.hpp"
#include "metadata/metadata.hpp"
#include "navigation.hpp"
#include "viewing/viewing.hpp"

#include <algorithm>

namespace amber::bindings {
namespace {
void hidden(SceneProjection::Owners &b, std::span<const Handle> objects) {
  retire_light_projection(b.lights, objects);
  retire_zone_projection(b.zones, objects);
  retire_replication_projection(b.replication, objects);
  for (auto h : objects) {
    b.illumination.lights.sources.erase(h.bits);
    b.illumination.zones.sources.erase(h.bits);
  }
}
} // namespace
void scene_visible(SceneProjection &scene, std::span<const Handle> objects, bool in_world) {
  auto &b = scene.owners_;
  if (in_world) {
    register_light_projection(b.lights, objects);
    register_zone_projection(b.zones, objects);
    register_illumination_processes(b.illumination, objects);
    register_replication_projection(b.replication, objects);
  } else {
    hidden(b, objects);
  }
  metadata_changed(b.metadata, objects, {});
}
void scene_moved(SceneProjection &scene, std::span<const Handle> objects) {
  if (objects.empty())
    return;
  auto &b = scene.owners_;
  move_replication_projection(b.replication, objects);
  move_light_projection(b.lights, objects);
  move_illuminance_reads(b.illuminance, objects);
  viewing::invalidate(b.metadata.views, objects);
}
void scene_values_changed(SceneProjection &scene, const space::Space &space,
                          observe::ActiveViews &views, std::span<const FactKey> facts) {
  if (facts.empty())
    return;
  auto &b = scene.owners_;
  std::vector<Handle> objects;
  objects.reserve(facts.size());
  for (const auto fact : facts) {
    value_changed(b.replication.index, fact.subject, fact.term, b.values.processes);
    light_value_changed(b.illumination.lights, b.lights.lights, b.lights.subscriptions, views,
                        b.values.processes, fact);
    zone_value_changed(b.illumination.zones, b.zones.zones, b.lights.subscriptions, views, space,
                       b.values.processes, fact);
    objects.push_back(fact.subject);
  }
  std::sort(objects.begin(), objects.end());
  objects.erase(std::unique(objects.begin(), objects.end()), objects.end());
  sync_illuminance_reads(b.illuminance, objects);
  viewing::invalidate(b.metadata.views, objects);
}
void advance_scene(SceneProjection &scene, IlluminanceSampling &sampling,
                   observe::ActiveViews &views, std::span<const space::Bounds> dirty_bounds,
                   Step now, double hz, react::DeferredEffects &effects) {
  auto &b = scene.owners_;
  observe::dirty_geometry(b.lights.subscriptions, b.lights.lights, dirty_bounds, &views);
  observe::dirty_views(views, dirty_bounds);
  advance_running_lights(b.illumination.lights, b.lights.lights, b.lights.subscriptions, views,
                         b.values.processes, now, hz);
  advance_running_zones(b.illumination.zones, b.zones.zones, b.lights.subscriptions, views,
                        sampling.space, b.values.processes, now, hz);
  sample_illuminance(sampling, now, hz, effects);
}
void ObjectLifecycle::project(std::span<const Handle> objects, bool in_world,
                              const InvocationInput &input, react::DeferredEffects &effects) {
  auto &b = owners_.scene.owners_;
  sync_illuminance_reads(b.illuminance, objects);
  if (in_world) {
    register_light_projection(b.lights, objects);
    register_zone_projection(b.zones, objects);
    register_illumination_processes(b.illumination, objects);
  } else {
    hidden(b, objects);
  }
  invalidate_mass_projection(b.mass, objects);
  refresh_mass(b.mass.masses, b.values, b.mass.declared_parts, b.mass.measurements, input.step,
               input.hz, input.origin, effects);
  if (in_world)
    register_replication_projection(b.replication, objects);
  invalidate_metadata(b.metadata, objects);
}
void objects_created(ObjectLifecycle &lifecycle, std::span<const Handle> objects, bool in_world,
                     const InvocationInput &input, react::DeferredEffects &effects) {
  if (objects.empty())
    return;
  auto &b = lifecycle.owners_;
  if (in_world)
    navigation_birth(b.navigation, objects, input, effects);
  install_reactions(b.reactions, objects, input, effects);
  lifecycle.project(objects, in_world, input, effects);
  for (auto h : objects)
    effects.ordered.emplace_back(step::EventWork{h, {}, b.start, {}, input.origin});
}
void objects_rebound(ObjectLifecycle &lifecycle, std::span<const lifecycle::Rebinding> changes,
                     bool in_world, const InvocationInput &input, react::DeferredEffects &effects) {
  if (changes.empty())
    return;
  auto &b = lifecycle.owners_;
  if (in_world)
    navigation_rebind(b.navigation, changes, input, effects);
  rebind_priorities(b.priorities, changes, input, effects);
  rebind_reactions(b.reactions, changes, input, effects);
  std::vector<Handle> objects;
  objects.reserve(changes.size());
  for (const auto &change : changes)
    objects.push_back(change.object);
  lifecycle.project(objects, in_world, input, effects);
}
void initialize_objects(ObjectLifecycle &lifecycle, std::span<const Handle> all,
                        std::span<const Handle> visible, bool restored,
                        const InvocationInput &input, react::DeferredEffects &effects) {
  auto &b = lifecycle.owners_;
  if (restored)
    restore_reactions(b.reactions, all, input, effects);
  else
    install_reactions(b.reactions, all, input, effects);
  // Initialize non-spatial answers for hidden inventory as well. Only the
  // explicitly visible subset enters lights/zones/replication.
  lifecycle.project(all, false, input, effects);
  scene_visible(b.scene, visible, true);
}
bool can_retire_objects(const ObjectLifecycle &lifecycle, std::span<const Handle> objects,
                        std::string &error) {
  return objects.empty() || commerce::can_retire_goods(lifecycle.owners_.commerce, objects, error);
}
void retire_objects(ObjectLifecycle &lifecycle, std::span<const Handle> objects,
                    const InvocationInput &input, react::DeferredEffects &effects) {
  if (objects.empty())
    return;
  auto &b = lifecycle.owners_;
  auto &scene = b.scene.owners_;
  commerce::retire_goods(b.commerce, objects, effects);
  retire_reactions(b.reaction_retirement, objects, input, effects);
  perm::revoke_objects(b.grants, b.players, objects);
  for (auto h : objects) {
    const auto owner = b.authority.owner[h.slot()];
    if (owner < b.players.rows.size()) {
      std::erase(b.players.rows[owner].inventory, h);
      std::erase(b.players.rows[owner].mailbox, h);
    }
    b.limits.erase(h.slot());
    if (auto ending = step::cancel_move(b.spatial.intents, h))
      b.spatial.endings.push_back(std::move(*ending));
    space::remove(b.spatial.space, b.spatial.identity, h, b.spatial.parents, b.spatial.flags,
                  b.spatial.relations, b.spatial.changes);
  }
  retire_illuminance_reads(scene.illuminance, scene.mass.measurements, objects);
  hidden(scene, objects);
  retire_mass_projection(scene.mass, objects);
  invalidate_metadata(scene.metadata, objects);
  b.instances.release_rows(objects);
  // Retiring a named child also changes its surviving ancestors' disjoint mass.
  refresh_mass(scene.mass.masses, scene.values, scene.mass.declared_parts, scene.mass.measurements,
               input.step, input.hz, input.origin, effects);
}
} // namespace amber::bindings
