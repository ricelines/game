#pragma once

#include "step.hpp"

namespace amber::step {
// Execution frames carry call state, never access to world tables.
struct ActionRun {
  NativeCall call;
  react::Resolution resolution;
  std::vector<PlannedInvocation> handlers;
  std::size_t next_handler = 0, handled = 0;
  std::uint64_t previous_intent = 0;
  bool changed = false;
  std::vector<Handle> things, created;
  std::vector<record::Credit> credits;
  std::vector<std::string> errors;
  std::optional<ActionOutcome> outcome;
};
ActionRun begin_action(const amber::catalog::State &catalog_state,
                       const amber::react::State &react_state, const net::Profiles &,
                       const amber::perm::State &perm_state,
                       const amber::ObjectState &objects_state,
                       const amber::space::State &space_state, const Intents &, record::Journal &,
                       float reach_m, NativeCall);
void accept_handler(ActionRun &, const react::CompletedInvocation &);
ActionOutcome finish_action(ActionRun &, const Intents &, const space::Space &, IdentityRead,
                            TransformRead, record::Journal &, std::vector<GapDraft> &);
ActionOutcome finish_native(ActionRun &, const NativeCompletion &, IdentityRead, AttributionRead,
                            const catalog::Models &, const catalog::Scripts &, record::Journal &);
struct HandlerRun {
  PlannedInvocation plan;
  react::Invocation invocation;
  react::CompletedInvocation completed;
  bool started = false;
};
using ExecutionFrame = std::variant<ActionRun, HandlerRun>;
ExecutionFrame *advance_execution(std::deque<ExecutionFrame> &, react::State &, const ObjectState &,
                                  const catalog::State &, record::Journal &);
std::optional<std::pair<InputOrder, ActionOutcome>> return_action(std::deque<ExecutionFrame> &,
                                                                  react::Vm &, IdentityRead,
                                                                  const Relation &declared_parts);
std::optional<NativeCall> finish_handler_segment(std::deque<ExecutionFrame> &, record::Journal &,
                                                 WorkQueues &, TimerWheel &, Step now, double hz,
                                                 std::size_t event_limit);
void invalidate_execution(std::deque<ExecutionFrame> &, IdentityRead, const react::ScriptStates &);

NativeCompletion
native_mechanics(amber::ObjectState &objects_state, amber::react::State &react_state,
                 const amber::perm::State &perm_state, amber::space::State &space_state,
                 MechanicalCauses &mechanical_causes, PhysicalRelationTerms physical_terms,
                 float reach_m, react::MechanicAction operation, const NativeCall &call);
NativeCompletion native_construction(amber::ObjectState &objects_state,
                                     amber::catalog::State &catalog_state,
                                     amber::react::State &react_state,
                                     amber::perm::State &perm_state, const GeometryAssets &geometry,
                                     amber::space::State &space_state,
                                     PhysicsDefaults physics_defaults,
                                     react::ConstructionAction operation, const NativeCall &call);
NativeCompletion native_exchange(amber::ObjectState &objects_state, amber::perm::State &perm_state,
                                 const space::Space &space, const DeliveryTargets &delivery,
                                 std::size_t mailbox_max, react::ExchangeAction operation,
                                 const NativeCall &call);
NativeCompletion native_permissions(const amber::catalog::State &catalog_state,
                                    const amber::react::State &react_state,
                                    amber::perm::State &perm_state,
                                    amber::ObjectState &objects_state, bool architect,
                                    react::PermissionAction operation, const NativeCall &call);
NativeCompletion native_scripts(amber::catalog::State &catalog_state,
                                amber::react::State &react_state, amber::perm::State &perm_state,
                                const amber::ObjectState &objects_state, TermId start_event,
                                std::span<const std::string> roles, react::ScriptAction operation,
                                const NativeCall &call);
NativeCompletion native_values(const perm::Players &players, const catalog::Models &models,
                               const react::ValueRead &values, amber::react::State &react_state,
                               std::array<TermId, 4> relation_terms,
                               std::span<const std::string> roles, react::ValueAction operation,
                               const NativeCall &call);
NativeCompletion native_writing(const perm::Players &players, const catalog::Models &models,
                                catalog::ModelTexts &model_text,
                                catalog::InstanceTexts &instance_text,
                                catalog::AuthoredTexts &authored_text, IdentityRead identity,
                                react::WritingAction operation, const NativeCall &call);
NativeCompletion native_registry(amber::catalog::State &catalog_state,
                                 amber::react::State &react_state,
                                 const std::filesystem::path &registry_directory,
                                 const react::ValueRead &values, const GeometryAssets &geometry,
                                 amber::space::State &space_state, PhysicsDefaults physics_defaults,
                                 std::span<const std::string> roles,
                                 react::RegistryAction operation, const NativeCall &call);
NativeCompletion native_elaboration(amber::catalog::State &catalog_state,
                                    const react::ValueRead &values,
                                    amber::react::State &react_state, amber::net::State &net_state,
                                    WorkQueues &queues, amber::elaborate::State &elaborate_state,
                                    elaborate::FixtureWorker &fixture_worker,
                                    elaborate::FixtureLimits fixture_limits,
                                    react::ElaborationAction operation, const NativeCall &call);
} // namespace amber::step
