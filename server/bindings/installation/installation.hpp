#pragma once

#include "bindings/allocation/allocation.hpp"
#include "bindings/execution/execution.hpp"
#include "install/install.hpp"
#include "lifecycle/lifecycle.hpp"

namespace amber::bindings {
enum class RuleMode { Validated, Enable, Disable };
struct RegistryPublication {
  void *local = nullptr;
  void (*publish)(void *, ScriptId, bool approved, bool active, Step, double hz,
                  const record::Origin &, react::DeferredEffects &) = nullptr;
};
struct RuleActivation {
  catalog::Scripts &scripts;
  react::RuleVersions &rules;
  react::Triggers &triggers;
  react::Subscriptions &subscriptions;
  install::WatchLinks &links;
  step::WorkQueues &queues;
  react::Vm &vm;
  const react::ValueRead &values;
  const react::AllocationRows &allocations;
  RegistryPublication publication;
};
void update_rule(RuleActivation &, ScriptId, RuleMode, Step, double hz, const record::Origin &,
                 react::DeferredEffects &);
struct InstallReactions {
  const catalog::Models &models;
  const catalog::Scripts &scripts;
  const install::InstanceScripts &instances;
  const react::RuleVersions &rules;
  const react::ValueRead &values;
  react::Triggers &triggers;
  react::Subscriptions &subscriptions;
  install::WatchLinks &links;
  const react::AllocationRows &allocations;
};
void install_reactions(InstallReactions &, std::span<const Handle>, const InvocationInput &,
                       react::DeferredEffects &);
void restore_reactions(InstallReactions &, std::span<const Handle>, const InvocationInput &,
                       react::DeferredEffects &);
void rebind_reactions(InstallReactions &, std::span<const lifecycle::Rebinding>,
                      const InvocationInput &, react::DeferredEffects &);
struct RebindPriorities {
  const react::ValueRead &values;
  AllocationReconciliation &allocation;
};
void rebind_priorities(RebindPriorities &, std::span<const lifecycle::Rebinding>,
                       const InvocationInput &, react::DeferredEffects &);
struct RetireReactions {
  const catalog::Models &models;
  const catalog::Scripts &scripts;
  const react::ValueRead &values;
  install::InstanceScripts &instances;
  react::Triggers &triggers;
  react::Subscriptions &subscriptions;
  install::WatchLinks &links;
  react::Properties &properties;
  react::Processes &processes;
  react::InstanceSettings &settings;
  react::ScriptStates &states;
  react::RandomStates &random;
  react::SharedColumns &shared;
  react::GivesTo &gives_to;
  react::AuthoredRelations &relations;
  AllocationReconciliation &allocation;
};
void retire_reactions(RetireReactions &, std::span<const Handle>, const InvocationInput &,
                      react::DeferredEffects &);
} // namespace amber::bindings
