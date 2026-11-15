#pragma once

#include "perm/perm.hpp"
#include "projection.hpp"

namespace amber::bindings {
struct MetadataChanges;
struct InvocationInput;
struct IlluminationProcesses;
struct IlluminanceReads;
struct IlluminanceSampling;
struct InstallReactions;
struct RebindPriorities;
struct RetireReactions;
struct NavigationDiagnostics;
class ObjectLifecycle;
namespace commerce {
struct RetirementBinding;
}

// One owner for derived scene maintenance. Callers request an operation; they
// cannot reach the retained projection tables through this interface.
class SceneProjection {
public:
  struct Owners {
    LightProjection &lights;
    ZoneProjection &zones;
    MassProjection &mass;
    ReplicationProjection &replication;
    IlluminationProcesses &illumination;
    IlluminanceReads &illuminance;
    const react::ValueRead &values;
    MetadataChanges &metadata;
  };
  explicit SceneProjection(Owners owners) : owners_(owners) {}

private:
  Owners owners_;
  friend void scene_visible(SceneProjection &, std::span<const Handle>, bool);
  friend void scene_moved(SceneProjection &, std::span<const Handle>);
  friend void scene_values_changed(SceneProjection &, const space::Space &, observe::ActiveViews &,
                                   std::span<const FactKey>);
  friend void advance_scene(SceneProjection &, IlluminanceSampling &, observe::ActiveViews &,
                            std::span<const space::Bounds>, Step, double, react::DeferredEffects &);
  friend class ObjectLifecycle;
  friend void retire_objects(ObjectLifecycle &, std::span<const Handle>, const InvocationInput &,
                             react::DeferredEffects &);
};

void scene_visible(SceneProjection &, std::span<const Handle>, bool in_world);
void scene_moved(SceneProjection &, std::span<const Handle>);
void scene_values_changed(SceneProjection &, const space::Space &, observe::ActiveViews &,
                          std::span<const FactKey>);
void advance_scene(SceneProjection &, IlluminanceSampling &, observe::ActiveViews &,
                   std::span<const space::Bounds> dirty_bounds, Step, double hz,
                   react::DeferredEffects &);

// Coordinates only object lifecycle. Engines, action handlers and VM bindings
// receive their existing narrow owners, never this integration object.
class ObjectLifecycle {
public:
  struct Owners {
    lifecycle::Instances &instances;
    SceneProjection &scene;
    InstallReactions &reactions;
    RebindPriorities &priorities;
    RetireReactions &reaction_retirement;
    commerce::RetirementBinding &commerce;
    NavigationDiagnostics &navigation;
    SpatialRetirement &spatial;
    perm::Grants &grants;
    perm::Players &players;
    perm::OwnerLimits &limits;
    AuthorityRead authority;
    TermId start;
    perm::Ledger &ledger;
  };
  explicit ObjectLifecycle(Owners owners) : owners_(owners) {}

private:
  Owners owners_;
  void project(std::span<const Handle>, bool, const InvocationInput &, react::DeferredEffects &);
  friend void objects_created(ObjectLifecycle &, std::span<const Handle>, bool,
                              const InvocationInput &, react::DeferredEffects &);
  friend void objects_rebound(ObjectLifecycle &, std::span<const lifecycle::Rebinding>, bool,
                              const InvocationInput &, react::DeferredEffects &);
  friend void initialize_objects(ObjectLifecycle &, std::span<const Handle>,
                                 std::span<const Handle>, bool, const InvocationInput &,
                                 react::DeferredEffects &);
  friend bool can_retire_objects(const ObjectLifecycle &, std::span<const Handle>, std::string &);
  friend void retire_objects(ObjectLifecycle &, std::span<const Handle>, const InvocationInput &,
                             react::DeferredEffects &);
};

void objects_created(ObjectLifecycle &, std::span<const Handle>, bool in_world,
                     const InvocationInput &, react::DeferredEffects &);
void objects_rebound(ObjectLifecycle &, std::span<const lifecycle::Rebinding>, bool in_world,
                     const InvocationInput &, react::DeferredEffects &);
// Called after physical attachments/compounds/motion have been reconstructed.
// Fresh worlds initialize watches; restored worlds retain saved answers. Neither
// emits start: the authored initial plan supplies its bootstrap events once.
void initialize_objects(ObjectLifecycle &, std::span<const Handle> all,
                        std::span<const Handle> visible, bool restored, const InvocationInput &,
                        react::DeferredEffects &);
// Both operations receive the same collected closure. Preflight is read-only;
// commit finishes every owner's cleanup before releasing object identities.
bool can_retire_objects(const ObjectLifecycle &, std::span<const Handle>, std::string &);
void retire_objects(ObjectLifecycle &, std::span<const Handle>, const InvocationInput &,
                    react::DeferredEffects &);
} // namespace amber::bindings
