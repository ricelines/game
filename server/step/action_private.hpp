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
  Monotonic::time_point deadline;
  std::optional<ActionOutcome> outcome;
};
ActionRun begin_action(const react::ValueRead &, const catalog::Models &, const catalog::Scripts &,
                       const react::InstanceScripts &, const react::RuleVersions &,
                       const react::Triggers &, const react::NativeActions &, const net::Profiles &,
                       const perm::Players &, const perm::Grants &, const perm::OwnerLimits &,
                       AuthorityRead, AttributionRead, TransformRead, const space::Space &,
                       const Intents &, record::Journal &, float reach_m, NativeCall,
                       const record::Origin &, Monotonic::time_point);
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
ExecutionFrame *advance_execution(std::deque<ExecutionFrame> &, react::Vm &, react::ScriptStates &,
                                  react::RandomStates &, react::Subscriptions &,
                                  const react::ValueRead &, const react::Triggers &,
                                  const Relation &declared_parts, AttributionRead,
                                  const catalog::Models &, const catalog::Scripts &,
                                  record::Journal &);
std::optional<std::pair<InputOrder, ActionOutcome>> return_action(std::deque<ExecutionFrame> &,
                                                                  react::Vm &, IdentityRead,
                                                                  const Relation &declared_parts);
std::optional<NativeCall> finish_handler_segment(std::deque<ExecutionFrame> &, record::Journal &,
                                                 WorkQueues &, TimerWheel &, Step now, double hz,
                                                 std::size_t event_limit);
void invalidate_execution(std::deque<ExecutionFrame> &, IdentityRead, const react::ScriptStates &);

NativeCompletion native_mechanics(
    const Relation &declared_parts, const react::Terms &terms, react::Properties &properties,
    react::Processes &processes, const perm::Players &players, const perm::Plots &plots,
    const perm::Grants &grants, const perm::OwnerLimits &owner_limits, space::Space &space,
    space::PhysicalRelations &physical_relations, MechanicalCauses &mechanical_causes,
    IdentityRead identity, TransformWrite poses, ParentWrite parents, AuthorityRead authority_read,
    SpaceFlags physical_flags, PlotWrite membership, PhysicalRelationTerms physical_terms,
    float reach_m, react::MechanicAction operation, const NativeCall &call);
NativeCompletion native_construction(
    Objects &objects, lifecycle::Parts &parts, Relation &declared_parts,
    const catalog::Assets &assets, catalog::Models &models, const catalog::Scripts &scripts,
    catalog::ParameterBlocks &parameters, catalog::ModelTexts &model_text,
    const react::ValueRead &values, react::ModelSettings &model_settings,
    react::AllocationRows &allocations, react::MeterRows &meters,
    react::AssetMeasurements &baked_measurements, perm::Players &players, const perm::Plots &plots,
    const perm::Grants &grants, const perm::Ledger &ledger, const perm::OwnerLimits &owner_limits,
    perm::Offers &offers, perm::Trades &trades, const GeometryAssets &geometry, space::Space &space,
    space::PhysicalRelations &physical_relations,
    std::vector<space::RelationChange> &relation_changes, std::array<TermId, 4> relation_terms,
    PhysicsRoles physics_roles, PhysicsDefaults physics_defaults,
    catalog::MeasurementBindings measurement_roles, react::ConstructionAction operation,
    const NativeCall &call);
NativeCompletion native_exchange(const lifecycle::Parts &parts, const Relation &declared_parts,
                                 perm::Players &players, const perm::Groups &groups,
                                 perm::Grants &grants, perm::Ledger &ledger,
                                 perm::OwnerLimits &owner_limits, perm::Offers &offers,
                                 perm::Trades &trades, const space::Space &space,
                                 const DeliveryTargets &delivery, IdentityRead identity,
                                 ParentRead parents, AuthorityRead authority_read,
                                 AuthorityWrite authority_write, SpaceFlags physical_flags,
                                 PermissionFlags permission_bits, std::size_t mailbox_max,
                                 react::ExchangeAction operation, const NativeCall &call);
NativeCompletion
native_permissions(const catalog::Models &models, const catalog::Scripts &scripts,
                   const react::ValueRead &values, const react::RuleVersions &rules,
                   const react::InstanceScripts &installations, perm::Players &players,
                   perm::Groups &groups, perm::Plots &plots, perm::Grants &grants,
                   const perm::OwnerLimits &owner_limits, IdentityRead identity,
                   AuthorityRead authority_read, PermissionFlags permission_bits, bool architect,
                   std::vector<GroupId> &object_groups, std::vector<std::uint32_t> &object_presets,
                   react::PermissionAction operation, const NativeCall &call);
NativeCompletion native_scripts(catalog::Models &models, const catalog::Scripts &scripts,
                                const react::ValueRead &values, react::SharedColumns &shared,
                                react::GivesTo &gives_to, react::ScriptStates &states,
                                react::Triggers &triggers, react::Subscriptions &subscriptions,
                                react::InstanceScripts &installations,
                                react::AllocationRows &allocations, perm::Players &players,
                                const perm::Groups &groups, const perm::Plots &plots,
                                perm::Grants &grants, const perm::OwnerLimits &owner_limits,
                                AuthorityRead authority_read, TermId start_event,
                                std::span<const std::string> roles, react::ScriptAction operation,
                                const NativeCall &call);
NativeCompletion
native_values(const catalog::Models &models, const react::ValueRead &values,
              react::Properties &properties, react::Processes &processes,
              react::InstanceSettings &instance_settings, react::ModelSettings &model_settings,
              react::AuthoredRelations &authored_relations, react::RelationsRead &relations,
              std::array<TermId, 4> relation_terms, std::span<const std::string> roles,
              react::ValueAction operation, const NativeCall &call);
NativeCompletion native_writing(const catalog::Models &models, catalog::ModelTexts &model_text,
                                catalog::InstanceTexts &instance_text,
                                catalog::AuthoredTexts &authored_text, IdentityRead identity,
                                react::WritingAction operation, const NativeCall &call);
NativeCompletion native_registry(
    const catalog::Assets &assets, const catalog::Models &models,
    const catalog::ParameterBlocks &parameters, react::Terms &terms, react::Materials &materials,
    const catalog::MechanicalBindings &mechanical, catalog::RegistryDocument &registry,
    const std::filesystem::path &registry_directory, const react::ValueRead &values,
    const react::RuleVersions &rules, const GeometryAssets &geometry, space::Space &space,
    PhysicsRoles physics_roles, PhysicsDefaults physics_defaults,
    std::span<const std::string> roles, react::RegistryAction operation, const NativeCall &call);
NativeCompletion
native_elaboration(catalog::Scripts &scripts, catalog::AuthoredTexts &authored_text,
                   const react::ValueRead &values, react::Vm &vm, react::Triggers &triggers,
                   react::RuleVersions &rules, react::Subscriptions &subscriptions,
                   react::AllocationRows &allocations, const net::Profiles &profiles,
                   net::Sessions &sessions, WorkQueues &queues, elaborate::Requests &requests,
                   elaborate::PendingFixtures &fixtures, elaborate::CaptureRefs &captures,
                   elaborate::GapCounts &gap_counts, elaborate::LastGaps &last_gaps,
                   elaborate::FixtureWorker &fixture_worker, bool architect, bool elaborator,
                   elaborate::FixtureLimits fixture_limits, react::ElaborationAction operation,
                   const NativeCall &call);
} // namespace amber::step
