#pragma once

#include "settings.hpp"
#include "step.hpp"

namespace amber::step::fixture {
void extend_vocabulary(react::Terms &, react::Materials &, const catalog::MechanicalBindings &,
                       const std::filesystem::path &registry_root,
                       const catalog::Hash &candidate_registry);
react::RuleChange install_candidate(catalog::Scripts &, react::Vm &, react::RuleVersions &,
                                    const react::Terms &, const catalog::RuntimePins &,
                                    const std::filesystem::path &source_root,
                                    const std::filesystem::path &destination_root,
                                    const catalog::Hash &artifact,
                                    const catalog::Hash &registry_pin, PlayerId author, bool active,
                                    Step now, double hz);
void initialize_case(const Case &, react::Properties &, react::Processes &, react::SharedColumns &,
                     react::GivesTo &, react::AllocationRows &, react::MeterRows &,
                     const react::ValueRead &, const catalog::Scripts &, const react::RuleChange &,
                     WorkQueues &, Step now, double hz, react::DeferredEffects &);
std::optional<NativeCall> prepare_action(const Case &, const react::Terms &,
                                         const react::NativeActions &, IdentityRead, AuthorityRead,
                                         const net::Profiles &, Step now, double hz);
void write_report(const std::filesystem::path &, const react::ValueRead &, const Case &, Step now,
                  double hz, std::size_t script_errors);
} // namespace amber::step::fixture

namespace amber::step {
space::StepResult advance_motion(space::Space &space, IdentityRead ids, TransformWrite poses,
                                 ParentWrite parent, SpaceFlags physical_flags,
                                 space::PhysicalRelations &physical_relations, Intents &intents,
                                 std::vector<IntentEnding> &intent_endings,
                                 TravelAccess &travel_access, const perm::Plots &plots,
                                 const perm::Players &players, const perm::Grants &grants,
                                 AuthorityRead auth, MechanicalCauses &mechanical_causes, Step now,
                                 double hz, float goal_tolerance_m, float reach_m);
void publish_intent_endings(std::vector<IntentEnding> &intent_endings, const react::Terms &terms,
                            TermId arrived, TermId blocked, TermId cancelled, IdentityRead ids,
                            AuthorityRead auth, net::Sessions &sessions, WorkQueues &queues,
                            record::Journal &journal, Step now, std::size_t event_queue_max);
void update_frame_illumination(IlluminationSources &, observe::Lights &, observe::ZoneAmbients &,
                               observe::LightSubscriptions &, observe::ActiveViews &,
                               const space::Space &, TransformRead, const react::ValueRead &,
                               const react::Subscriptions &, react::RuntimeMeasurements &,
                               std::span<const Handle> moved,
                               std::span<const space::Bounds> dirty_bounds,
                               std::span<const FactKey> changed, TermId illuminance, Step now,
                               double hz, react::DeferredEffects &);
void sample_motion(std::vector<observe::MotionSample> &motion,
                   SparseColumn<std::pair<Handle, Vec3>> &previous_positions,
                   std::span<const Handle> moved, const space::Space &space, IdentityRead ids,
                   TransformRead poses, double hz);
} // namespace amber::step

namespace amber::step {
void write_state_report(const std::filesystem::path &path, const react::ValueRead &values,
                        std::span<const TermId> inspection_terms, TransformRead poses, Step now,
                        double hz, std::size_t script_errors, const TimerWheel &pending_timers,
                        const react::AllocationRows &allocations);
void publish_value_changes(net::ReplicationIndex &, net::SessionViews &, const react::Processes &,
                           std::span<const FactKey>);
void record_frame(record::Journal &journal, record::Writer &writer, net::RecordFeed &feed,
                  IdentityRead ids, TransformRead poses, std::size_t &script_errors);
} // namespace amber::step

namespace amber::step {
void advance_permission_deadlines(perm::Grants &grants, perm::Players &players,
                                  TravelAccess &travel_access, net::Sessions &sessions,
                                  record::Journal &journal, Step now, double hz,
                                  double reminder_seconds, std::size_t limit);
void expire_sale_offers(perm::Offers &offers, IdentityRead ids, PermissionFlags flags,
                        net::SessionViews &session_views, net::Sessions &sessions,
                        record::Journal &journal, Step now, std::size_t limit);
} // namespace amber::step

namespace amber::step {
std::optional<net::Authenticated>
connection_notice(const net::SessionNotice &notice, net::Sessions &sessions,
                  net::SessionViews &views, observe::ActiveViews &active, observe::Hearing &hearing,
                  net::ReplicationIndex &replication, std::vector<observe::Listener> &listeners,
                  DeliveryTargets &delivery, const catalog::Models &models,
                  const catalog::ModelTexts &model_text, const react::Terms &terms,
                  const react::Materials &materials, net::MaterialAppearanceRoles appearance,
                  IdentityRead identity, TransformRead poses, Step now, double hz, float cell_m,
                  std::uint32_t repl_delay_ms);

bool queue_connection_mutation(Inputs &inputs, WorkQueues &queues, net::Invocation &request);

void answer_query(net::Sessions &sessions, net::Server &server, net::ReplicationIndex &replication,
                  net::SessionViews &views, observe::ActiveViews &active, observe::Hearing &hearing,
                  record::Journal &journal, const catalog::Models &models,
                  const catalog::ParameterBlocks &parameters, const catalog::Assets &assets,
                  const catalog::Scripts &scripts, const catalog::ModelTexts &model_text,
                  const catalog::InstanceTexts &instance_text,
                  const react::InstanceScripts &installations, const react::NativeActions &native,
                  const react::ValueRead &values, const std::vector<TermId> &inspection_terms,
                  const catalog::AuthoredTexts &authored_text, const space::Space &space,
                  TransformRead poses, AuthorityRead authority, ParentRead parents,
                  AttributionRead attribution, const Relation &declared_parts,
                  const Relation &attached_to, const observe::Lights &lights,
                  const observe::ZoneAmbients &zones,
                  const std::vector<observe::MotionSample> &motion, const Intents &intents,
                  const perm::OwnerLimits &owner_limits, const perm::Players &players,
                  const perm::Grants &grants, const perm::Plots &plots, const perm::Ledger &ledger,
                  const react::ScriptStates &states, const react::RuleVersions &rules,
                  const net::RecordFeed &feed, observe::Settings settings, std::size_t actions_page,
                  Step now, double hz, const net::Invocation &request);

void send_connection_outputs(std::span<net::SessionOutput> outputs, net::Server &server,
                             net::ReplicationIndex &replication, net::SessionViews &views,
                             IdentityRead identity, AuthorityRead authority, ParentRead parents,
                             const catalog::ModelTexts &model_text,
                             const catalog::InstanceTexts &instance_text);

void flush_connections(net::Sessions &sessions, net::Server &server,
                       net::ReplicationIndex &replication, net::SessionViews &views, Inputs &inputs,
                       WorkQueues &queues, IdentityRead identity, AuthorityRead authority,
                       ParentRead parents, const catalog::ModelTexts &model_text,
                       const catalog::InstanceTexts &instance_text);

void refresh_connections(
    const net::Sessions &sessions, net::SessionViews &views, observe::ActiveViews &active,
    std::vector<observe::Listener> &listeners, DeliveryTargets &delivery,
    const catalog::Models &models, const catalog::ParameterBlocks &parameters,
    const catalog::Assets &assets, const catalog::Scripts &scripts,
    const catalog::ModelTexts &model_text, const catalog::InstanceTexts &instance_text,
    const react::InstanceScripts &installations, const react::NativeActions &native,
    const react::ValueRead &values, const std::vector<TermId> &inspection_terms,
    const catalog::AuthoredTexts &authored_text, const space::Space &space, TransformRead poses,
    const Relation &declared_parts, const Relation &attached_to, const observe::Lights &lights,
    const observe::ZoneAmbients &zones, const std::vector<observe::MotionSample> &motion,
    const Intents &intents, observe::Settings settings, Step now, double hz);
} // namespace amber::step

namespace amber::step::start {
void install_catalog(const Json &, const react::Terms &, const catalog::Hash &,
                     const catalog::RuntimePins &, react::VmSettings,
                     const std::filesystem::path &asset_root,
                     const std::filesystem::path &script_root, catalog::Assets &, catalog::Models &,
                     catalog::Scripts &, catalog::ModelTexts &, react::ModelSettings &, react::Vm &,
                     react::RuleVersions &, Symbols &);
void bind_relation_reads(react::RelationsRead &, react::AuthoredRelations &, const react::Terms &,
                         const space::PhysicalRelations &, std::array<TermId, 4> physical_terms);
void record_seed(record::Journal &journal, const react::Terms &terms, const catalog::Models &models,
                 const catalog::Scripts &scripts, std::span<const Handle> initial, IdentityRead ids,
                 AuthorityRead auth, AttributionRead makers);
} // namespace amber::step::start

namespace amber::step {
std::vector<Handle> initialize_physics(
    space::Space &space, const GeometryAssets &geometry, const catalog::Models &models,
    const catalog::ParameterBlocks &parameters, const catalog::Assets &assets,
    const react::ValueRead &values, TransformWrite transforms, ParentWrite parents,
    SpaceFlags flags, const lifecycle::Parts &parts, const Relation &declared_parts,
    const perm::Players &players, space::PhysicalRelations &physical,
    std::vector<space::RelationChange> &changes, MechanicalCauses &causes,
    std::span<const Handle> initial, std::span<const space::MotionRow> restored_motion,
    std::span<const BodyCauseRow> restored_causes, PhysicsRoles roles, PhysicsDefaults defaults,
    bool restored, Step now, double hz);
} // namespace amber::step

namespace amber::step::checkpoint {
void restore_pending_work(Inputs &inputs, elaborate::CaptureRefs &captures,
                          const std::filesystem::path &catalog_root);
} // namespace amber::step::checkpoint

namespace amber::step {
std::optional<std::vector<PlannedInvocation>>
prepare_reactive(react::Subscriptions &subscriptions, const react::ValueRead &values,
                 const react::AllocationRows &allocations, const react::Triggers &triggers,
                 const catalog::Scripts &scripts, WorkQueues &queues, TimerWheel &timers,
                 std::vector<GapDraft> &gaps, ForegroundItem item, Step now, double hz);

std::variant<NativeCall, InputReply>
prepare_input(react::authoring::Worker &worker, const react::Terms &terms,
              const react::NativeActions &native, const catalog::RegistryDocument &registry,
              Intents &intents, std::vector<IntentEnding> &endings, record::Journal &journal,
              Input &input, Step now, double hz);

} // namespace amber::step
