#pragma once

#include "catalog/catalog.hpp"

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
space::StepResult advance_motion(space::State &space_state, amber::ObjectState &objects_state,
                                 amber::step::State &step_state,
                                 const amber::perm::State &perm_state,
                                 const amber::step::Clock &clock, float goal_tolerance_m,
                                 float reach_m);
void publish_intent_endings(amber::step::State &step_state, const react::Terms &terms,
                            TermId arrived, TermId blocked, TermId cancelled,
                            const amber::ObjectState &objects_state, net::Sessions &sessions,
                            record::Journal &journal, Step now, std::size_t event_queue_max);
react::DeferredEffects update_frame_illumination(State &, space::State &, TransformRead,
                                                 const react::ValueRead &, react::State &,
                                                 const catalog::Scripts &,
                                                 const space::StepResult &, TermId illuminance);
void sample_motion(std::vector<observe::MotionSample> &motion,
                   SparseColumn<std::pair<Handle, Vec3>> &previous_positions,
                   std::span<const Handle> moved, const space::Space &space, IdentityRead ids,
                   TransformRead poses, double hz);

nlohmann::json state_report(const ObjectState &, const catalog::State &, const react::State &,
                            const amber::step::Clock &clock, std::size_t script_errors,
                            const TimerWheel &pending_timers);
void publish_value_changes(net::ReplicationIndex &, const react::Processes &,
                           std::span<const FactKey>);
void record_frame(record::Journal &journal, record::Writer &writer, net::RecordFeed &feed,
                  IdentityRead ids, TransformRead poses, std::size_t &script_errors);

void advance_permission_deadlines(amber::perm::State &perm_state, net::Sessions &sessions,
                                  record::Journal &journal, Step now, double hz,
                                  double reminder_seconds, std::size_t limit,
                                  std::size_t mailbox_max);
void expire_sale_offers(perm::State &perm_state, IdentityRead ids, PermissionFlags flags,
                        net::Sessions &sessions, record::Journal &journal, Step now,
                        std::size_t limit, std::size_t mailbox_max);

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
                         const amber::ObjectState &objects_state);
} // namespace amber::step

namespace amber::step {
void install_object_physics(State &, const catalog::State &, const react::State &, ObjectState &,
                            space::State &, std::span<const Handle>, PhysicsDefaults);
void admit_operator(const net::Admission &, net::State &, perm::State &, const net::KeyHash &,
                    const amber::Settings &, record::Journal &, const Clock &);
JoinResult create_join(const net::JoinRequest &, net::Profiles &, perm::State &, ObjectState &,
                       const catalog::State &, const nlohmann::json &newcomer, ModelId,
                       const Clock &, const space::State &);
react::DeferredEffects activate_join(const catalog::State &, react::State &, ObjectState &,
                                     space::State &, net::State &, State &, const amber::Settings &,
                                     std::span<const Handle>);
std::vector<Handle>
initialize_physics(amber::step::State &step_state, const amber::catalog::State &catalog_state,
                   const amber::react::State &react_state, amber::ObjectState &objects_state,
                   const perm::Players &players, amber::space::State &space_state,
                   std::span<const Handle> initial, const checkpoint::Decoded &decoded,
                   PhysicsDefaults defaults, bool restored);
} // namespace amber::step

namespace amber::step::checkpoint {
void restore_pending_captures(elaborate::CaptureRefs &captures,
                              const std::filesystem::path &catalog_root);
} // namespace amber::step::checkpoint

namespace amber::step {
void refresh_rule_reactions(const ObjectState &, const catalog::State &, const react::State &,
                            space::State &, step::State &, std::span<const ScriptId>);
std::optional<std::vector<react::PlannedInvocation>>
prepare_reactive(amber::react::State &react_state, const react::ValueRead &values,
                 const catalog::Scripts &scripts, amber::step::State &step_state,
                 ForegroundItem item, Step now, double hz);

std::variant<NativeCall, InputReply>
prepare_input(step::authoring::Worker &worker, const amber::react::State &react_state,
              const catalog::RegistryDocument &registry, amber::step::State &step_state,
              record::Journal &journal, Input &input, Step now, double hz);

} // namespace amber::step
