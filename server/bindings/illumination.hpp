#pragma once
#include "presentation.hpp"
#include "react/react.hpp"

namespace amber::bindings {
struct IlluminanceReads {
  observe::LightSubscriptions &points;
  const react::Subscriptions &subscriptions;
  IdentityRead identity;
  TransformRead transforms;
  TermId illuminance;
};
// Called when installations, selectors or followed relations change, not by
// scanning objects on the simulation tick.
void sync_illuminance_reads(IlluminanceReads &, std::span<const Handle> consumers);
void move_illuminance_reads(IlluminanceReads &, std::span<const Handle> moved);
void retire_illuminance_reads(IlluminanceReads &, react::RuntimeMeasurements &,
                              std::span<const Handle>);
struct RunningLights {
  std::unordered_set<std::uint64_t> sources;
};
void light_value_changed(RunningLights &, observe::Lights &, observe::LightSubscriptions &,
                         observe::ActiveViews &, const react::Processes &, FactKey);
void advance_running_lights(RunningLights &, const observe::Lights &, observe::LightSubscriptions &,
                            observe::ActiveViews &, const react::Processes &, Step, double hz);
using RunningZones = RunningLights;
struct IlluminationProcesses {
  RunningLights &lights;
  RunningZones &zones;
  const observe::Lights &light_bindings;
  const observe::ZoneAmbients &zone_bindings;
  const react::Processes &processes;
};
void register_illumination_processes(IlluminationProcesses &, std::span<const Handle>);
void zone_value_changed(RunningZones &, const observe::ZoneAmbients &,
                        observe::LightSubscriptions &, observe::ActiveViews &, const space::Space &,
                        const react::Processes &, FactKey);
void advance_running_zones(RunningZones &, const observe::ZoneAmbients &,
                           observe::LightSubscriptions &, observe::ActiveViews &,
                           const space::Space &, const react::Processes &, Step, double hz);
struct IlluminanceSampling {
  observe::LightSubscriptions &points;
  const space::Space &space;
  const react::ValueRead &values;
  const observe::Lights &lights;
  const observe::ZoneAmbients &zones;
  react::RuntimeMeasurements &measurements;
  double world_ambient_lux, query_ms;
};
void sample_illuminance(IlluminanceSampling &, Step, double hz, react::DeferredEffects &);
} // namespace amber::bindings
