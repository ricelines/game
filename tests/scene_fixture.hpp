#pragma once

#include "bindings/commerce/commerce.hpp"
#include "bindings/illumination.hpp"
#include "bindings/installation/installation.hpp"
#include "bindings/metadata/metadata.hpp"
#include "bindings/navigation.hpp"
#include "bindings/scene.hpp"
#include "bindings/viewing/viewing.hpp"
#include "runtime_fixture.hpp"

namespace amber::testscene {
// These are actual derived owners, shared by integration fixtures rather than
// callback spies. The tests inspect their normal outputs after lifecycle work.
struct Projection {
  observe::Lights lights{8};
  observe::LightSubscriptions points{8};
  observe::ZoneAmbients zones;
  bindings::AssemblyMasses masses;
  bindings::ReplicationIndex replicas{{32, 2, 3, 65536, 1048576}};
  bindings::viewing::SessionViews views;
  bindings::RunningLights running_lights;
  bindings::RunningZones running_zones;
  bindings::IlluminanceReads illuminance;
  bindings::IlluminationProcesses running;
  bindings::MetadataChanges metadata;
  bindings::LightProjection light;
  bindings::ZoneProjection zone;
  bindings::MassProjection mass;
  bindings::ReplicationProjection replication;
  bindings::SceneProjection scene;
  Projection(Objects &objects, const catalog::Models &models,
             const catalog::ParameterBlocks &parameters, const catalog::Assets &assets,
             const react::ValueRead &values, react::RuntimeMeasurements &runtime,
             const react::Subscriptions &subscriptions, const Relation &declared,
             bindings::MassRoles roles)
      : masses{roles, {}, {}},
        illuminance{points, subscriptions, identity(objects), transforms(objects), 0},
        running{running_lights, running_zones, lights, zones, values.processes},
        metadata{views, illuminance, {}, {}},
        light{lights,         points, identity(objects), transforms(objects),
              objects.params, models, parameters,        assets,
              values.terms,   .05f},
        zone{zones, identity(objects), objects.params, models, parameters, assets, values.terms},
        mass{masses, runtime, identity(objects), declared},
        replication{replicas, transforms(objects), models, parameters, assets, values},
        scene({light, zone, mass, replication, running, illuminance, values, metadata}) {}
};
struct Lifecycle {
  react::GivesTo gives_to;
  react::MeterRows meters;
  react::PriorityLinks priorities;
  bindings::AllocationReconciliation allocation;
  bindings::InstallReactions reactions;
  bindings::RebindPriorities priority_rebind;
  bindings::RetireReactions reaction_retirement;
  bindings::NavigationDiagnostics navigation;
  step::Intents intents;
  std::vector<step::IntentEnding> endings;
  bindings::SpatialRetirement spatial;
  bindings::commerce::Custody custody;
  bindings::commerce::RetirementBinding commerce;
  bindings::ObjectLifecycle lifecycle;
  Lifecycle(test_runtime::Fixture &f, space::Space &world, lifecycle::Instances &instances,
            const lifecycle::Parts &parts, const Relation &declared,
            space::PhysicalRelations &physical, std::vector<space::RelationChange> &changes,
            Projection &projection, perm::Players &players, perm::Grants &grants,
            perm::OwnerLimits &limits, perm::Offers &offers, perm::Trades &trades)
      : allocation{f.allocations, meters, priorities},
        reactions{f.models,   f.scripts,       f.installed, f.rules,      *f.projection,
                  f.triggers, f.subscriptions, f.links,     f.allocations},
        priority_rebind{*f.projection, allocation},
        reaction_retirement{
            f.models, f.scripts,    *f.projection, f.installed,         f.triggers, f.subscriptions,
            f.links,  f.properties, f.processes,   f.instance_settings, f.states,   f.random,
            f.shared, gives_to,     f.relations,   allocation},
        navigation{world, identity(f.objects), transforms(f.objects)},
        spatial{world,
                identity(f.objects),
                parents(f.objects),
                space_flags(f.objects),
                physical,
                changes,
                intents,
                endings},
        custody{world,
                identity(f.objects),
                transforms(f.objects),
                parents(f.objects),
                space_flags(f.objects),
                physical,
                parts,
                declared,
                {},
                projection.scene},
        commerce{offers, trades, permission_flags(f.objects), custody},
        lifecycle({instances, projection.scene, reactions, priority_rebind, reaction_retirement,
                   commerce, navigation, spatial, grants, players, limits, authority(f.objects),
                   6}) {}
};
} // namespace amber::testscene
