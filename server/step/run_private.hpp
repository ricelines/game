#pragma once

#include "settings.hpp"
#include "step.hpp"

namespace amber::step::fixture {
void extend_vocabulary(react::Terms &, react::Materials &, const catalog::MechanicalBindings &,
                       const std::filesystem::path &registry_root,
                       const catalog::Hash &candidate_registry);
react::RuleChange install_candidate(catalog::Scripts &, amber::react::State &react_state,
                                    const catalog::RuntimePins &,
                                    const std::filesystem::path &source_root,
                                    const std::filesystem::path &destination_root,
                                    const catalog::Hash &artifact,
                                    const catalog::Hash &registry_pin, PlayerId author, bool active,
                                    const amber::step::Clock &clock);
void initialize_case(const Case &, amber::react::State &react_state, const react::ValueRead &,
                     const catalog::Scripts &, const react::RuleChange &, WorkQueues &,
                     const amber::step::Clock &clock, react::DeferredEffects &);
std::optional<NativeCall> prepare_action(const Case &, const react::Terms &,
                                         const react::NativeActions &, IdentityRead, AuthorityRead,
                                         const net::Profiles &, Step now, double hz);
void write_report(const std::filesystem::path &, const react::ValueRead &, const Case &, Step now,
                  double hz, std::size_t script_errors);
} // namespace amber::step::fixture

namespace amber::step {
space::StepResult advance_motion(space::Space &space, amber::ObjectState &objects_state,
                                 space::PhysicalRelations &physical_relations,
                                 amber::step::State &step_state,
                                 const amber::perm::State &perm_state,
                                 const amber::step::Clock &clock, float goal_tolerance_m,
                                 float reach_m);
void publish_intent_endings(amber::step::State &step_state, const react::Terms &terms,
                            TermId arrived, TermId blocked, TermId cancelled,
                            const amber::ObjectState &objects_state, net::Sessions &sessions,
                            record::Journal &journal, Step now, std::size_t event_queue_max);
void update_frame_illumination(amber::step::State &step_state, amber::space::State &space_state,
                               TransformRead, const react::ValueRead &,
                               amber::react::State &react_state, std::span<const Handle> moved,
                               std::span<const space::Bounds> dirty_bounds, TermId illuminance,
                               react::DeferredEffects &);
void sample_motion(std::vector<observe::MotionSample> &motion,
                   SparseColumn<std::pair<Handle, Vec3>> &previous_positions,
                   std::span<const Handle> moved, const space::Space &space, IdentityRead ids,
                   TransformRead poses, double hz);
} // namespace amber::step

namespace amber::step {
void write_state_report(const std::filesystem::path &path, const react::ValueRead &values,
                        std::span<const TermId> inspection_terms, TransformRead poses,
                        const amber::step::Clock &clock, std::size_t script_errors,
                        const TimerWheel &pending_timers, const react::AllocationRows &allocations);
void publish_value_changes(net::ReplicationIndex &, net::SessionViews &, const react::Processes &,
                           std::span<const FactKey>);
void record_frame(record::Journal &journal, record::Writer &writer, net::RecordFeed &feed,
                  IdentityRead ids, TransformRead poses, std::size_t &script_errors);
} // namespace amber::step

namespace amber::step {
void advance_permission_deadlines(amber::perm::State &perm_state, TravelAccess &travel_access,
                                  net::Sessions &sessions, record::Journal &journal, Step now,
                                  double hz, double reminder_seconds, std::size_t limit);
void expire_sale_offers(perm::State &perm_state, IdentityRead ids, PermissionFlags flags,
                        net::SessionViews &session_views, net::Sessions &sessions,
                        record::Journal &journal, Step now, std::size_t limit);
} // namespace amber::step

namespace amber::step {
std::optional<net::Authenticated>
connection_notice(const net::SessionNotice &notice, amber::net::State &net_state,
                  amber::space::State &space_state, DeliveryTargets &delivery,
                  const amber::catalog::State &catalog_state,
                  const amber::react::State &react_state, net::MaterialAppearanceRoles appearance,
                  const amber::ObjectState &objects_state, const amber::step::Clock &clock,
                  const amber::Settings &settings);

bool queue_connection_mutation(Inputs &inputs, WorkQueues &queues, net::Invocation &request);

void answer_query(amber::net::State &net_state, amber::space::State &space_state,
                  record::Journal &journal, const amber::catalog::State &catalog_state,
                  const amber::react::State &react_state, const amber::ObjectState &objects_state,
                  const amber::step::State &step_state, const amber::perm::State &perm_state,
                  const amber::Settings &loaded_settings, const net::Invocation &request);

void send_connection_outputs(std::span<net::SessionOutput> outputs, net::Server &server,
                             amber::net::State &net_state, const amber::ObjectState &objects_state,
                             const amber::catalog::State &catalog_state);

void flush_connections(amber::net::State &net_state, net::Server &server,
                       amber::step::State &step_state, const amber::ObjectState &objects_state,
                       const amber::catalog::State &catalog_state);

void refresh_connections(amber::net::State &net_state, amber::space::State &space_state,
                         amber::step::State &step_state, const amber::catalog::State &catalog_state,
                         const amber::react::State &react_state,
                         const amber::ObjectState &objects_state, observe::Settings settings,
                         const amber::step::Clock &clock);
} // namespace amber::step

namespace amber::step::start {
void install_catalog(const Json &, amber::react::State &react_state, const catalog::Hash &,
                     const catalog::RuntimePins &, react::VmSettings,
                     const std::filesystem::path &asset_root,
                     const std::filesystem::path &script_root, amber::catalog::State &catalog_state,
                     Symbols &);
void bind_relation_reads(react::RelationsRead &, react::AuthoredRelations &, const react::Terms &,
                         const space::PhysicalRelations &, std::array<TermId, 4> physical_terms);
void record_seed(record::Journal &journal, const react::Terms &terms, const catalog::Models &models,
                 const catalog::Scripts &scripts, std::span<const Handle> initial, IdentityRead ids,
                 AuthorityRead auth, AttributionRead makers);
} // namespace amber::step::start

namespace amber::step {
std::vector<Handle>
initialize_physics(amber::step::State &step_state, const amber::catalog::State &catalog_state,
                   const amber::react::State &react_state, amber::ObjectState &objects_state,
                   const perm::Players &players, amber::space::State &space_state,
                   std::span<const Handle> initial, const checkpoint::Decoded &decoded,
                   PhysicsDefaults defaults, bool restored);
} // namespace amber::step

namespace amber::step::checkpoint {
void restore_pending_work(Inputs &inputs, elaborate::CaptureRefs &captures,
                          const std::filesystem::path &catalog_root);
} // namespace amber::step::checkpoint

namespace amber::step {
std::optional<std::vector<PlannedInvocation>>
prepare_reactive(amber::react::State &react_state, const react::ValueRead &values,
                 const catalog::Scripts &scripts, amber::step::State &step_state,
                 ForegroundItem item, Step now, double hz);

std::variant<NativeCall, InputReply>
prepare_input(react::authoring::Worker &worker, const amber::react::State &react_state,
              const catalog::RegistryDocument &registry, amber::step::State &step_state,
              record::Journal &journal, Input &input, Step now, double hz);

} // namespace amber::step
