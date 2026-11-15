#pragma once
#include "react/allocation.hpp"

namespace amber::bindings {
struct AllocationReconciliation {
  react::AllocationRows &allocations;
  react::MeterRows &meters;
  react::PriorityLinks &priorities;
};
void reconcile_allocations(const react::ValueRead &, AllocationReconciliation &,
                           react::DeferredEffects &, Step, double hz);
void reconcile_priority(const react::ValueRead &, AllocationReconciliation &, react::SettingKey,
                        react::DeferredEffects &, Step, double hz);
void reconcile_object_priorities(const react::ValueRead &, AllocationReconciliation &, Handle,
                                 react::DeferredEffects &, Step, double hz);
} // namespace amber::bindings
