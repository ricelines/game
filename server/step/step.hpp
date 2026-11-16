#pragma once
namespace amber {
struct Settings;
}
namespace amber::step {
struct State;
}

#include "elaborate/elaborate.hpp"
#include "net/net.hpp"
#include "perm/perm.hpp"
#include "react/react.hpp"
#include "record/record.hpp"
#include "snap/snap.hpp"
#include "space/space.hpp"

#include <array>
#include <csignal>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace amber::step {
inline constexpr char runtime_version[] = "ambergris-0.4";

struct NavigationRequests {
  struct Request {
    Handle body;
    Vec3 reference;
    std::map<Handle, space::Bounds> regions;
    std::vector<std::string> errors;
  };
  std::map<PlayerId, Request> rows;
};
struct NavigationNotice {
  PlayerId author;
  nlohmann::json event;
};
void queue_navigation(NavigationRequests &, const space::Space &, IdentityRead, TransformRead,
                      std::span<const Handle>, PlayerId author, Handle body);
// Unfinished tile rebuilds retain their requests.
std::vector<NavigationNotice> publish_navigation(NavigationRequests &, const space::Space &,
                                                 IdentityRead, TransformRead,
                                                 const space::NavigationInstall &);
struct TravelAccess {
  struct Row {
    PlayerId player;
    std::vector<space::TravelBoundary> forbidden;
  };
  std::unordered_map<std::uint64_t, Row> bodies;
  Step next_expiry = 0;
};
void invalidate_travel(TravelAccess &, const perm::Grants &, Step);
std::span<const space::TravelBoundary> travel_boundaries(TravelAccess &, IdentityRead,
                                                         AuthorityRead, Handle body,
                                                         const perm::Plots &, const perm::Players &,
                                                         const perm::Grants &, Step);

void append_inputs(WorkQueues &, std::vector<Work>);
void promote_due(TimerWheel &, Step now, WorkQueues &);
std::optional<ForegroundItem> pop_foreground(WorkQueues &);
std::optional<BackgroundItem> pop_background(WorkQueues &);
PushResult enqueue_event(WorkQueues &, EventWork, std::size_t limit);
PushResult enqueue_watch(WorkQueues &, WatchWork);
void enqueue_background(WorkQueues &, BackgroundItem);
void schedule(TimerWheel &, Timer);
void cancel_threshold(TimerWheel &, const ThresholdKey &);
void cancel_source_thresholds(TimerWheel &, const react::Subscriptions &, FactKey);
void rebuild_queue_index(WorkQueues &);
std::vector<Timer> timers(const TimerWheel &);

IntentStart start_move(Intents &, Handle, MoveGoal, record::Origin,
                       std::optional<float> speed_m_s = {});
std::optional<IntentEnding> cancel_move(Intents &, Handle);
std::optional<IntentEnding> finish_move(Intents &, Handle, IntentEnd);

class PhaseCosts {
public:
  explicit PhaseCosts(std::size_t window);
  void observe(double milliseconds);
  double mean_ms() const;

private:
  std::vector<double> samples_;
  std::size_t next_ = 0, count_ = 0;
  double total_ = 0;
};

Frame begin_frame(Clock &, const Settings &, const PhaseCosts &, step::WorkQueues &,
                  step::TimerWheel &, std::vector<step::Work> arrived, Monotonic::time_point now);
bool admits(const Frame &, Monotonic::time_point now);
Monotonic::time_point finish_frame(Clock &, PhaseCosts &, const Frame &, double other_phase_ms,
                                   Monotonic::time_point now);

step::InputWork enqueue_input(Inputs &, Input);
InputCompletion complete_input(Inputs &, std::uint64_t, nlohmann::json reply);
void write_inputs(snap::Output &, const Inputs &,
                  std::optional<std::span<const std::uint64_t>> selected_ids = {});
void read_inputs(snap::Input &, Inputs &);

bool in_reach(Vec3 actor_point, Bounds target, float reach_m);

bool valid_event(const EventTurn &, IdentityRead, const react::Processes &,
                 const react::Subscriptions &, Step now, double hz);
EventPayload normalize_arguments(const react::Terms &, TermId action, EventPayload);
std::vector<PlannedInvocation> plan_event(const react::Triggers &, const react::ValueRead &,
                                          const step::EventWork &, Step, double hz);
std::vector<PlannedInvocation> plan_action(const react::Triggers &, const react::ValueRead &,
                                           const react::Resolution &, const ActionRequest &,
                                           Handle authenticated_body, const record::Origin &, Step,
                                           double hz);

WatchPlan plan_watch(react::Subscriptions &, const react::ValueRead &,
                     const react::AllocationRows &, const react::Triggers &,
                     const catalog::Scripts &, const step::WatchWork &, Step, double hz);
WatchPlan plan_background(amber::react::State &react_state, const react::ValueRead &,
                          const catalog::Scripts &, const step::BackgroundItem &, Step, double hz);

struct VolumeGeometry {
  space::GeometryId geometry;
  space::Pose local;
  space::VolumeKind kind;
};
struct GeometryAsset {
  space::GeometryId collision = 0;
  std::vector<VolumeGeometry> volumes;
};
struct GeometryAssets {
  std::unordered_map<catalog::Hash, GeometryAsset> rows;
};
// Run when importing immutable artifacts, before admitting a placement request.
void import_geometry_asset(GeometryAssets &, space::Space &, const catalog::Asset &,
                           const std::filesystem::path &asset_root);
struct PhysicsRoles {
  TermId density, transmission;
  TermId friction = 0, restitution = 0;
};
struct PhysicsDefaults {
  float friction, restitution;
};
struct PhysicalSubject {
  ModelId model;
  std::uint32_t params;
  TermId material;
  PlotId plot;
  Handle instance;
  ModelId settings_model = 0;
};
struct PhysicalDefinition {
  space::BodySpec body;
  std::uint32_t mode_flags;
  const std::vector<VolumeGeometry> *volumes;
};
PhysicalDefinition describe_physics(const GeometryAssets &, const catalog::Asset &,
                                    const react::ValueRead &, PhysicalSubject, std::uint32_t flags,
                                    PhysicsRoles, PhysicsDefaults, const amber::step::Clock &clock);
// Install the current measured binding on the single writer.
void install_physics(space::Space &, IdentityRead, TransformRead, SpaceFlags, Handle,
                     const PhysicalDefinition &);
// Refresh actual bindings after an ordinary material edit; earlier writes are retained on error.
void refresh_material(space::Space &, const GeometryAssets &,
                      const amber::catalog::State &catalog_state, const react::ValueRead &,
                      PhysicsRoles, PhysicsDefaults, std::span<const TermId>,
                      const amber::step::Clock &clock);

struct CauseSummary {
  record::Origin origin;
  bool ambiguous = false;
};
class MechanicalCauses {
public:
  explicit MechanicalCauses(std::size_t authors_max = 8);
  ~MechanicalCauses();
  MechanicalCauses(const MechanicalCauses &) = delete;
  MechanicalCauses &operator=(const MechanicalCauses &) = delete;
  space::CauseToken intern(const record::Origin &, space::CauseToken previous = 0);
  space::CauseToken restore(CauseSummary);
  CauseSummary lookup(space::CauseToken) const;
  CauseSummary combine(const CauseSummary &, const CauseSummary &) const;
  void retain(std::span<const space::CauseToken>);

private:
  struct Data;
  std::unique_ptr<Data> data;
};
struct PhysicalRelationTerms {
  TermId contains, supports, attached_to, in_zone;
};
void relation_effects(react::DeferredEffects &, const PhysicalRelationTerms &,
                      std::span<const space::RelationChange>);
struct PhysicalEventTerms {
  TermId contact;
  PhysicalRelationTerms relations;
};
void publish_physical(amber::step::State &step_state, record::Journal &, const PhysicalEventTerms &,
                      std::size_t event_limit, const amber::ObjectState &objects_state,
                      const amber::catalog::State &catalog_state, const space::StepResult &,
                      const amber::step::Clock &clock);
void collect_physical_causes(MechanicalCauses &, const space::Space &);

struct BodyCauseRow {
  Handle object;
  CauseSummary summary;
};
void capture_motion_causes(const MechanicalCauses &, const space::Space &, IdentityRead,
                           std::span<const Handle>, std::vector<BodyCauseRow> &);
void restore_motion_causes(MechanicalCauses &, space::Space &, IdentityRead,
                           std::span<const BodyCauseRow>);
void write_motion_causes(snap::Output &, const std::vector<BodyCauseRow> &,
                         const snap::ObjectSelection & = {});
void read_motion_causes(snap::Input &, std::vector<BodyCauseRow> &);

step::NativeCompletion grasp_action(amber::ObjectState &objects_state,
                                    amber::space::State &space_state,
                                    const PhysicalRelationTerms &relation_terms, float reach_m,
                                    MechanicalCauses &causes, const perm::Plots &plots,
                                    const step::NativeCall &call);
step::NativeCompletion attach_action(amber::ObjectState &objects_state,
                                     amber::space::State &space_state,
                                     const PhysicalRelationTerms &relation_terms,
                                     const amber::perm::State &perm_state, float reach_m,
                                     MechanicalCauses &causes, const step::NativeCall &call);
step::NativeCompletion release_action(amber::ObjectState &objects_state,
                                      amber::space::State &space_state,
                                      const PhysicalRelationTerms &relation_terms,
                                      const amber::perm::State &perm_state,
                                      MechanicalCauses &causes, const step::NativeCall &call);
step::NativeCompletion impulse_action(space::Space &world, IdentityRead identity,
                                      MechanicalCauses &causes, const step::NativeCall &call);
step::NativeCompletion transfer_action(amber::react::State &react_state,
                                       const amber::ObjectState &objects_state,
                                       const amber::perm::State &perm_state,
                                       const step::NativeCall &call);
void bind_physical(react::Vm &, amber::ObjectState &objects_state, amber::step::State &step_state,
                   const amber::perm::State &perm_state, std::size_t part_batch_max,
                   amber::space::State &space_state, const amber::catalog::State &catalog_state);
void declare_physical_actions(react::Terms &, react::NativeActions &);

void bind_metadata(const amber::ObjectState &objects_state,
                   const amber::catalog::State &catalog_state, amber::react::State &react_state,
                   const perm::Plots &);

using Json = nlohmann::json;
struct CommerceDeclarations {
  TermId pay = 0, balance = 0, inventory = 0, give = 0, offer = 0, offers = 0, accept = 0,
         cancel_offer = 0, trade_open = 0, trade_change = 0, trade_confirm = 0, trade_cancel = 0,
         place_existing = 0;
};
CommerceDeclarations declare_commerce(react::Terms &, react::NativeActions &);
struct OfferExpired {
  perm::OfferId offer;
  PlayerId seller;
  Handle goods;
  std::vector<Handle> cleared;
};
std::vector<OfferExpired> advance_offer_expiry(perm::Offers &, IdentityRead, PermissionFlags, Step,
                                               std::size_t max_entries);
step::NativeCompletion retire_goods(perm::Offers &, perm::Trades &, IdentityRead, PermissionFlags,
                                    std::span<const Handle>);
step::NativeCompletion rebind_offers(perm::Offers &, perm::Trades &, PermissionFlags, IdentityRead,
                                     const lifecycle::Parts &, std::span<const Handle>, Step);
step::NativeCompletion deliver_mailbox(perm::Players &, const amber::ObjectState &objects_state,
                                       const space::Space &, PlayerId, Handle authenticated_body,
                                       Step);
std::string validate_pay(const Json &);
std::string validate_balance(const Json &);
std::string validate_inventory(const Json &);
std::string validate_give(const Json &);
std::string validate_offer(const Json &);
std::string validate_offers(const Json &);
std::string validate_accept(const Json &);
std::string validate_cancel_offer(const Json &);
std::string validate_trade_open(const Json &);
std::string validate_trade_change(const Json &);
std::string validate_trade_confirm(const Json &);
std::string validate_trade_cancel(const Json &);
std::string validate_place_existing(const Json &);
step::NativeCompletion pay(perm::Ledger &ledger, perm::Grants &grants, const perm::Players &players,
                           IdentityRead identity, AuthorityRead authority,
                           const perm::Groups &groups, const step::NativeCall &call);
step::NativeCompletion balance(const perm::Ledger &ledger, const perm::Players &players,
                               IdentityRead identity, AuthorityRead authority,
                               const perm::Groups &groups, const step::NativeCall &call);
step::NativeCompletion inventory(const perm::Players &players, const step::NativeCall &call);
step::NativeCompletion give(amber::perm::State &perm_state, amber::ObjectState &objects_state,
                            const space::Space &space, const DeliveryTargets &delivery,
                            std::size_t mailbox_max, const step::NativeCall &call);
step::NativeCompletion offer(amber::perm::State &perm_state, amber::ObjectState &objects_state,
                             const space::Space &space, const step::NativeCall &call);
step::NativeCompletion offers(const perm::Offers &offers, const perm::Players &players,
                              IdentityRead identity, AuthorityRead authority,
                              const step::NativeCall &call);
step::NativeCompletion accept(amber::perm::State &perm_state, amber::ObjectState &objects_state,
                              const space::Space &space, const DeliveryTargets &delivery,
                              std::size_t mailbox_max, const step::NativeCall &call);
step::NativeCompletion cancel_offer(amber::perm::State &perm_state,
                                    amber::ObjectState &objects_state,
                                    const step::NativeCall &call);
step::NativeCompletion trade_open(perm::Trades &trades, const perm::Players &players,
                                  IdentityRead identity, const DeliveryTargets &delivery,
                                  const step::NativeCall &call);
step::NativeCompletion trade_change(amber::perm::State &perm_state,
                                    amber::ObjectState &objects_state, const space::Space &space,
                                    const step::NativeCall &call);
step::NativeCompletion trade_confirm(amber::perm::State &perm_state,
                                     amber::ObjectState &objects_state, const space::Space &space,
                                     const DeliveryTargets &delivery, const step::NativeCall &call);
step::NativeCompletion trade_cancel(perm::Trades &trades, perm::Offers &offers,
                                    PermissionFlags flags, IdentityRead identity,
                                    const lifecycle::Parts &parts, const step::NativeCall &call);
step::NativeCompletion
place_existing(amber::ObjectState &objects_state, amber::space::State &space_state,
               std::array<TermId, 4> relation_terms, amber::perm::State &perm_state,
               const GeometryAssets &geometry, const amber::catalog::State &catalog_state,
               PhysicsRoles roles, PhysicsDefaults defaults, amber::react::State &react_state,
               const step::NativeCall &call);

using Json = nlohmann::json;
struct AdminDeclarations {
  TermId grant = 0, revoke = 0, list_grants = 0, preset = 0, group_create = 0, group_member = 0,
         plot_permissions = 0;
};
AdminDeclarations declare_admin(react::Terms &, react::NativeActions &);
struct GrantNotice {
  PlayerId recipient;
  Json event;
};
struct GrantUpdates {
  std::vector<GrantNotice> notices;
  std::vector<record::Event> records;
};
GrantUpdates advance_grants(perm::Grants &, perm::Players &, Step now, Step reminder_steps,
                            double hz, std::size_t max_entries);
std::string validate_grant(const Json &);
std::string validate_revoke(const Json &);
std::string validate_list_grants(const Json &);
std::string validate_preset(const Json &);
std::string validate_group_create(const Json &);
std::string validate_group_member(const Json &);
std::string validate_plot_permissions(const Json &);
step::NativeCompletion grant(amber::perm::State &perm_state,
                             const amber::ObjectState &objects_state,
                             const amber::catalog::State &catalog_state,
                             const amber::react::State &react_state, const step::NativeCall &call);
step::NativeCompletion revoke(amber::perm::State &perm_state,
                              const amber::ObjectState &objects_state, bool issuer_is_architect,
                              const step::NativeCall &call);
step::NativeCompletion list_grants(const perm::Grants &grants, const perm::Players &players,
                                   const step::NativeCall &call);
step::NativeCompletion preset(amber::ObjectState &objects_state,
                              std::vector<std::uint32_t> &presets,
                              const amber::perm::State &perm_state, const step::NativeCall &call);
step::NativeCompletion group_create(perm::Groups &groups, perm::Players &players,
                                    const step::NativeCall &call);
step::NativeCompletion group_member(const perm::Groups &groups, perm::Players &players,
                                    const step::NativeCall &call);
step::NativeCompletion plot_permissions(perm::Plots &plots, const perm::Players &players,
                                        const perm::Groups &groups, const step::NativeCall &call);

using Json = nlohmann::json;
struct ElaborationDeclarations {
  TermId request_model = 0, requests = 0, claim = 0, answer = 0, refuse = 0, rule_activation = 0;
};
ElaborationDeclarations declare_elaboration(react::Terms &, react::NativeActions &);
struct AnswerCompletion {
  step::NativeCompletion action;
  std::optional<elaborate::CloseOutcome> notification;
};
struct FixtureCompletion {
  std::optional<step::InputCompletion> input;
  std::optional<elaborate::CloseOutcome> notification;
  record::Origin origin;
  react::DeferredEffects effects;
  std::vector<ScriptId> changed_scripts;
};
FixtureCompletion complete_fixture(amber::elaborate::State &elaborate_state,
                                   amber::step::State &step_state, catalog::Scripts &scripts,
                                   amber::react::State &react_state, const react::ValueRead &values,
                                   elaborate::RoleAccess current_role,
                                   const elaborate::FixtureResult &result,
                                   const amber::step::Clock &clock);
std::string validate_request_model(const Json &);
std::string validate_requests(const Json &);
std::string validate_claim(const Json &);
std::string validate_answer(const Json &);
std::string validate_answer_rule(const Json &);
std::string validate_answer_patch(const Json &);
std::string validate_answer_detail(const Json &);
std::string validate_refuse(const Json &);
std::string validate_rule_activation(const Json &);
step::NativeCompletion request_model(elaborate::Requests &requests, const elaborate::LastGaps &last,
                                     const step::NativeCall &call);
step::NativeCompletion requests(const elaborate::Requests &requests,
                                const elaborate::GapCounts &gaps, bool is_elaborator,
                                std::uint32_t page_size, const step::NativeCall &call);
step::NativeCompletion claim(elaborate::Requests &requests, bool is_elaborator,
                             const step::NativeCall &call);
step::NativeCompletion answer_rule(amber::elaborate::State &elaborate_state,
                                   const catalog::Scripts &scripts,
                                   elaborate::FixtureWorker &worker, std::string runtime,
                                   elaborate::FixtureLimits limits, bool is_elaborator,
                                   Handle requester_body, const step::NativeCall &call);
AnswerCompletion answer_patch(elaborate::Requests &requests, bool is_elaborator,
                              const step::NativeCall &call);
AnswerCompletion answer_detail(elaborate::Requests &requests, catalog::AuthoredTexts &texts,
                               bool is_elaborator, const step::NativeCall &call);
AnswerCompletion refuse(elaborate::Requests &requests, catalog::AuthoredTexts &texts,
                        bool is_elaborator, const step::NativeCall &call);
step::NativeCompletion rule_activation(catalog::Scripts &scripts, amber::react::State &react_state,
                                       step::WorkQueues &queues, const react::ValueRead &values,
                                       bool may_edit_rules, const step::NativeCall &call);

using Json = nlohmann::json;
std::uint32_t identifier(const Json &, std::string_view prefix);
Handle live_handle(const Json &, IdentityRead);
Step duration_steps(double seconds, double hz);
perm::AccountTarget account(const Json &, IdentityRead, AuthorityRead, const perm::Players &,
                            const perm::Groups &);
perm::ScopeControl scope(const Json &, IdentityRead, AuthorityRead, const perm::OwnerLimits &,
                         const perm::Players &, const perm::Plots &, const perm::Groups &);
perm::Rights rights(const Json &, perm::ScopeKind);
} // namespace amber::step

namespace amber::step::editing {
using Json = nlohmann::json;
struct Declarations {
  TermId install = 0, uninstall = 0, model_script = 0, edit_text = 0, model_text = 0,
         set_setting = 0, model_setting = 0, patch_state = 0, relate = 0, define_term = 0,
         define_material = 0, edit_detail = 0;
};
Declarations declare_editing(react::Terms &, react::NativeActions &);
std::string validate_install(const Json &);
std::string validate_uninstall(const Json &);
std::string validate_model_script(const Json &);
std::string validate_edit_text(const Json &);
std::string validate_model_text(const Json &);
std::string validate_edit_detail(const Json &);
std::string validate_set_setting(const Json &);
std::string validate_model_setting(const Json &);
std::string validate_patch_state(const Json &);
std::string validate_relate(const Json &);
std::string validate_define_term(const Json &);
std::string validate_define_material(const Json &);
step::NativeCompletion edit_text(const step::NativeCall &call,
                                 catalog::InstanceTexts &instance_texts,
                                 const catalog::ModelTexts &model_texts, IdentityRead identity);
step::NativeCompletion model_text(const step::NativeCall &call, catalog::ModelTexts &model_texts,
                                  const catalog::Models &models, IdentityRead identity);
step::NativeCompletion edit_detail(const step::NativeCall &call, catalog::AuthoredTexts &details);
step::NativeCompletion set_setting(const step::NativeCall &call,
                                   react::InstanceSettings &instance_settings,
                                   const react::ValueRead &values);
step::NativeCompletion model_setting(const step::NativeCall &call,
                                     react::ModelSettings &model_settings,
                                     const react::ValueRead &values, const catalog::Models &models);
step::NativeCompletion patch_state(const step::NativeCall &call, react::Properties &properties,
                                   react::Processes &processes, const react::ValueRead &values,
                                   std::span<const std::string> roles);
step::NativeCompletion relate(const step::NativeCall &call, react::AuthoredRelations &relations,
                              react::RelationsRead &projection, const react::ValueRead &values,
                              std::array<TermId, 4> mechanical_relations);
catalog::Hash persist_registry(catalog::RegistryDocument &document,
                               const std::filesystem::path &directory, const react::Terms &terms,
                               const react::Materials &materials, const react::RuleVersions &rules,
                               const catalog::MechanicalBindings &mechanical);
step::NativeCompletion define_term(const step::NativeCall &call, std::span<const std::string> roles,
                                   react::Terms &terms, const react::Materials &materials,
                                   const react::RuleVersions &rules,
                                   const catalog::MechanicalBindings &mechanical,
                                   catalog::RegistryDocument &document,
                                   const std::filesystem::path &directory);
step::NativeCompletion
define_material(const step::NativeCall &call, std::span<const std::string> roles,
                amber::react::State &react_state, const react::ValueRead &values,
                amber::catalog::State &catalog_state, const GeometryAssets &geometry,
                space::Space &space, PhysicsRoles physical_roles, PhysicsDefaults defaults,
                const std::filesystem::path &directory);
step::NativeCompletion install_script(const step::NativeCall &call,
                                      amber::react::State &react_state,
                                      const amber::catalog::State &catalog_state,
                                      const amber::ObjectState &objects_state,
                                      amber::perm::State &perm_state, TermId start_event);
step::NativeCompletion uninstall_script(const step::NativeCall &call,
                                        amber::react::State &react_state,
                                        const react::ValueRead &values,
                                        const amber::catalog::State &catalog_state,
                                        amber::perm::State &perm_state, TermId start_event);
step::NativeCompletion model_script(const step::NativeCall &call, amber::react::State &react_state,
                                    const react::ValueRead &values,
                                    amber::catalog::State &catalog_state,
                                    amber::perm::State &perm_state, TermId start_event);
void rebind_scripts(const amber::catalog::State &catalog_state, amber::react::State &react_state,
                    const react::ValueRead &, amber::perm::State &perm_state, TermId start_event,
                    const amber::step::Clock &clock, const record::Origin &, NativeCompletion &);

} // namespace amber::step::editing

namespace amber::step {
InputReply failed_authoring(record::Journal &, Step, const Input &);
} // namespace amber::step

namespace amber::step::checkpoint {
struct CatalogSelection {
  std::vector<ModelId> models;
  std::vector<std::uint32_t> parameters;
  std::vector<ScriptId> scripts;
};
struct CaptureSelection {
  std::vector<Handle> objects;
  CatalogSelection catalog;
  perm::SnapshotSelection permissions;
  std::size_t priority_dependencies = 0;
};
struct SnapshotProgress {
  std::optional<Step> last_step;
  double pause_max_ms = 0;
};
struct Decoded {
  std::uint64_t next_record;
  std::vector<space::MotionRow> motion;
  std::vector<BodyCauseRow> causes;
};
std::vector<Handle> neighborhood(IdentityRead, ParentRead, const lifecycle::Parts &,
                                 std::span<const Relation *const>, std::span<const Handle> seeds);
std::vector<Handle> priority_neighborhood(IdentityRead, ParentRead, const lifecycle::Parts &,
                                          const react::Terms &, const react::SharedColumns &,
                                          std::span<const Handle>);
CatalogSelection catalog_selection(const catalog::Models &, const catalog::ParameterBlocks &,
                                   const react::InstanceScripts &, IdentityRead,
                                   const std::vector<std::uint32_t> &, const react::RuleVersions &,
                                   std::span<const Handle>);
void encode_checkpoint(
    snap::Buffer &bytes, const snap::Header &header, const amber::ObjectState &objects_state,
    const amber::react::State &react_state, const amber::space::State &space_state,
    const amber::catalog::State &catalog_state, const amber::perm::State &perm_state,
    const amber::net::State &net_state, const amber::step::State &step_state,
    const amber::elaborate::State &elaborate_state, const std::vector<space::MotionRow> &motion,
    const std::vector<BodyCauseRow> &causes, std::uint64_t next_record,
    const CaptureSelection *selection);
Decoded decode_checkpoint(const snap::View &view, amber::ObjectState &objects_state,
                          amber::react::State &react_state, amber::space::State &space_state,
                          amber::catalog::State &catalog_state, amber::perm::State &perm_state,
                          amber::net::State &net_state, amber::step::State &step_state,
                          amber::elaborate::State &elaborate_state);
bool save_snapshot(snap::Writer &writer, const snap::Header &header,
                   const amber::ObjectState &objects_state, const amber::react::State &react_state,
                   const amber::space::State &space_state,
                   const amber::catalog::State &catalog_state, const amber::perm::State &perm_state,
                   const amber::net::State &net_state, amber::step::State &step_state,
                   const amber::elaborate::State &elaborate_state, std::uint64_t next_record);
void flush_gaps(amber::step::State &step_state, record::Journal &journal,
                elaborate::CaptureWriter &writer, const snap::Header &header,
                const amber::Settings &settings, const amber::net::State &net_state,
                const amber::react::State &react_state, const amber::ObjectState &objects_state,
                const amber::space::State &space_state, const amber::catalog::State &catalog_state,
                const amber::perm::State &perm_state, amber::elaborate::State &elaborate_state);
CaptureSelection capture_selection(const GapDraft &gap, const amber::ObjectState &objects_state,
                                   const amber::catalog::State &catalog_state,
                                   const amber::react::State &react_state,
                                   const amber::perm::State &perm_state,
                                   const amber::net::State &net_state, const space::Space &space,
                                   float radius_m);

} // namespace amber::step::checkpoint

namespace amber::step {
// Derived sampling state. Constants live only in the numerical Space rows.
struct IlluminationSources {
  std::unordered_map<std::uint64_t, TermId> lights, zones;
  std::unordered_set<std::uint64_t> running_lights, running_zones;
};
void register_illumination(IlluminationSources &, amber::space::State &space_state, TransformRead,
                           const amber::catalog::State &catalog_state, const react::ValueRead &,
                           std::span<const Handle>, float default_source_radius_m,
                           const amber::step::Clock &clock);
void move_illumination(observe::Lights &, observe::LightSubscriptions &, observe::ActiveViews &,
                       TransformRead, std::span<const Handle>);
void retire_illumination(IlluminationSources &, observe::Lights &, observe::ZoneAmbients &,
                         observe::LightSubscriptions &, observe::ActiveViews &,
                         const space::Space &, std::span<const Handle>);
void illumination_values_changed(IlluminationSources &, amber::space::State &space_state,
                                 const react::ValueRead &, std::span<const FactKey>, Step,
                                 double hz);
void advance_illumination(IlluminationSources &, amber::space::State &space_state,
                          const react::ValueRead &, Step, double hz);
void sync_illuminance_reads(observe::LightSubscriptions &, const react::Subscriptions &,
                            IdentityRead, TransformRead, TermId illuminance,
                            std::span<const Handle> consumers);
void move_illuminance_reads(observe::LightSubscriptions &, TransformRead, TermId illuminance,
                            std::span<const Handle> moved);
void retire_illuminance_reads(observe::LightSubscriptions &, react::RuntimeMeasurements &,
                              TermId illuminance, std::span<const Handle>);
void sample_illuminance(observe::LightSubscriptions &, const space::Space &, IdentityRead,
                        const observe::Lights &, const observe::ZoneAmbients &,
                        react::RuntimeMeasurements &, double world_ambient_lux,
                        react::DeferredEffects &);

struct MassRoles {
  TermId own_mass, assembly_mass;
};
struct AssemblyMasses {
  struct Row {
    double own_kg, total_kg;
  };
  MassRoles roles;
  std::unordered_map<std::uint64_t, Row> rows;
  std::unordered_set<std::uint64_t> dirty;
};
void validate_mass_roles(const react::Terms &, MassRoles);
void invalidate_mass(AssemblyMasses &, IdentityRead, const Relation &declared_parts,
                     std::span<const Handle> changed);
void refresh_mass(AssemblyMasses &, const react::ValueRead &, const Relation &declared_parts,
                  react::RuntimeMeasurements &, Step, double hz, const record::Origin &,
                  react::DeferredEffects &);
void retire_mass(AssemblyMasses &, react::RuntimeMeasurements &, Handle);
void reconcile_mass(AssemblyMasses &, const react::ValueRead &, const Relation &declared_parts,
                    react::RuntimeMeasurements &, std::span<const FactKey>,
                    const amber::step::Clock &clock, const record::Origin &,
                    react::DeferredEffects &);

void write_timers(snap::Output &, const TimerWheel &, const snap::ObjectSelection & = {});
void read_timers(snap::Input &, TimerWheel &);
void write_queues(snap::Output &, const WorkQueues &, const Intents &, std::uint64_t next_record,
                  const snap::ObjectSelection & = {},
                  std::span<const std::uint64_t> selected_inputs = {});
std::uint64_t read_queues(snap::Input &, WorkQueues &, Intents &);

NativeCompletion replace_instance(amber::ObjectState &objects_state,
                                  const amber::catalog::State &catalog_state,
                                  const GeometryAssets &geometry, const react::ValueRead &values,
                                  amber::space::State &space_state,
                                  const amber::perm::State &perm_state, PhysicsDefaults defaults,
                                  const NativeCall &call);

void apply_custody(amber::ObjectState &objects_state, amber::space::State &space_state,
                   std::array<TermId, 4> relation_terms, const perm::Plots &,
                   std::span<const perm::CustodyChange>, NativeCompletion &);
void change_custody(amber::ObjectState &objects_state, amber::space::State &space_state,
                    std::array<TermId, 4>, const perm::Plots &, IlluminationSources &,
                    NativeCompletion &);
void project_visibility(IlluminationSources &, amber::space::State &space_state,
                        net::ReplicationIndex &, TransformRead,
                        const amber::catalog::State &catalog_state, const react::ValueRead &,
                        const NativeCompletion &, float source_radius_m,
                        const amber::step::Clock &clock);

NativeCompletion
revise_model(amber::ObjectState &objects_state, amber::catalog::State &catalog_state,
             amber::react::State &react_state, catalog::MeasurementBindings measurement_roles,
             const GeometryAssets &geometry, amber::space::State &space_state,
             amber::perm::State &perm_state, PhysicsDefaults defaults, const NativeCall &call);
std::string validate_model_edit(const nlohmann::json &);
TermId declare_model_edit(react::Terms &, react::NativeActions &);
// Direct lifecycle domain operations; callers compose existing owner functions in order.
void retire_commerce(amber::perm::State &perm_state, amber::ObjectState &objects_state,
                     std::span<const Handle> removed, Step, NativeCompletion &);
void retire_physics(space::Space &, amber::ObjectState &objects_state, space::PhysicalRelations &,
                    amber::step::State &step_state, std::span<const Handle> removed,
                    std::array<TermId, 4> relation_terms, const record::Origin &,
                    NativeCompletion &);
void retire_publication(net::ReplicationIndex &, net::SessionViews &, catalog::InstanceTexts &,
                        std::span<const Handle> removed);
void refresh_object_plots(const space::Space &, amber::ObjectState &objects_state,
                          const perm::Plots &, const amber::react::State &react_state,
                          const record::Origin &, NativeCompletion &);
void publish_catalog_changes(net::Sessions &, net::SessionViews &, const catalog::Models &,
                             const catalog::ModelTexts &, const react::Terms &,
                             const react::Materials &, net::MaterialAppearanceRoles,
                             NativeCompletion &);
void publish_object_changes(net::Sessions &, net::SessionViews &, IdentityRead, AuthorityRead,
                            ParentRead, const catalog::ModelTexts &, const catalog::InstanceTexts &,
                            std::span<const Handle> changed);

void reconcile_effects(const react::ValueRead &values, amber::react::State &react_state,
                       amber::step::State &step_state, const Relation &declared_parts,
                       const catalog::Scripts &scripts, const amber::step::Clock &clock,
                       react::DeferredEffects &effects, const record::Origin &origin);
void record_send_overflow(record::Journal &, const net::SendOverflow &, Step);
void publish_effects(react::DeferredEffects &&, const record::Origin &, Step, WorkQueues &,
                     TimerWheel &, record::Journal &, std::size_t event_limit);
record::Origin publish_invocation(record::Journal &, WorkQueues &, TimerWheel &,
                                  react::CompletedInvocation &&, const record::Origin &,
                                  std::size_t event_limit);
using AdmittedWork = std::variant<ForegroundItem, BackgroundItem, elaborate::FixtureResult,
                                  react::authoring::Completion>;
std::optional<AdmittedWork> next_work(WorkQueues &, Frame &, std::deque<elaborate::FixtureResult> &,
                                      std::deque<react::authoring::Completion> &,
                                      Monotonic::time_point now);
} // namespace amber::step

namespace amber::step::start {
using Json = nlohmann::json;
struct Symbols {
  std::unordered_map<std::string, ModelId> models;
  std::unordered_map<std::string, ScriptId> scripts;
  std::unordered_map<std::string, Handle> objects;
};
struct SeedRuleApproval {
  std::string key, evidence;
};
struct PreparedSeedScript {
  std::string key;
  PlayerId author = 0;
  bool requested_rule = false;
  catalog::PreparedScript artifact;
};

Json read_plan(const std::filesystem::path &);
catalog::RegistryDocument load_registry(const Json &, const std::filesystem::path &registry_root,
                                        react::Terms &, react::Materials &,
                                        catalog::MechanicalBindings &);
std::vector<PreparedSeedScript> prepare_scripts(const Json &, const react::Terms &,
                                                const catalog::Hash &registry,
                                                const catalog::RuntimePins &, react::VmSettings);
void install_scripts(std::vector<PreparedSeedScript>, catalog::Scripts &, react::Vm &, Symbols &,
                     std::span<const SeedRuleApproval>);
std::vector<catalog::Asset> prepare_assets(const Json &, const std::filesystem::path &,
                                           const catalog::RuntimePins &, const react::Terms &);
void import_assets(std::vector<catalog::Asset>, catalog::Assets &);
void install_models(const Json &, const catalog::Assets &, const catalog::Scripts &,
                    const react::Terms &, catalog::Models &, catalog::ModelTexts &,
                    react::ModelSettings &, Symbols &);
std::vector<Handle> install_objects(const Json &, Symbols &,
                                    const amber::catalog::State &catalog_state,
                                    const react::Terms &, amber::ObjectState &objects_state, Step);
void install_properties(const Json &, const Symbols &, amber::react::State &react_state, Step,
                        double hz);
void install_relations(const Json &, const Symbols &, const react::Terms &,
                       react::AuthoredRelations &, react::DeferredEffects &);
void install_profiles(const Json &, const Symbols &, net::Profiles &, perm::Players &,
                      const std::filesystem::path &key_root);
void install_land(const Json &, perm::Plots &, perm::Groups &, perm::Players &);
void install_money(const Json &, const Symbols &, IdentityRead, AuthorityRead,
                   const perm::Players &, const perm::Groups &, perm::Ledger &);
void install_grants(const Json &, const Symbols &, const amber::ObjectState &objects_state,
                    amber::perm::State &perm_state, const amber::catalog::State &catalog_state,
                    const amber::react::State &react_state, const amber::step::Clock &clock);
void install_script_state(const Json &, const Symbols &, react::ScriptStates &);
void queue_bootstrap(const Json &, const Symbols &, const react::Terms &, step::WorkQueues &,
                     std::size_t event_limit);
} // namespace amber::step::start

namespace amber::step::fixture {
struct Case {
  std::map<std::string, Handle> subjects;
  nlohmann::json test;
};
Case read_case(const std::filesystem::path &);
void apply_given(amber::react::State &react_state, const react::ValueRead &, const Case &, Step,
                 double, react::DeferredEffects &);
std::uint64_t steps(const Case &);
elaborate::TestRun observe(const react::ValueRead &, const Case &, Step, double);
} // namespace amber::step::fixture

namespace amber::step {
struct RunOptions {
  std::filesystem::path config = "config.toml", plan = ".cache/content/seeded-plan.json";
  std::filesystem::path assets = "assets", data = ".runtime", client = "client/dist";
  std::filesystem::path report;
  std::filesystem::path restore, catalog, fixture_case;
  catalog::Hash candidate, candidate_registry;
  PlayerId candidate_author = 0;
  bool candidate_active = false;
  bool snapshot_at_end = false;
  std::optional<Step> steps;
  std::uint16_t port = 8080;
  bool network = true, realtime = true;
};
int run(const RunOptions &, const volatile std::sig_atomic_t &);

EventPayload decode_arguments(const react::Term &, const nlohmann::json &, Handle);
std::variant<NativeCall, InputReply> parse_action_input(const Input &, const react::Terms &,
                                                        const react::NativeActions &,
                                                        record::Journal &, Step, double hz);
ActionOutcome cancel_input(Intents &, std::vector<IntentEnding> &, record::Journal &, const Input &,
                           Step);
InputReply submit_authoring(react::authoring::Worker &, const react::Terms &,
                            const catalog::RegistryDocument &, Input &);
void deliver_input(Inputs &, net::Sessions &, observe::Hearing &, std::uint32_t event_limit,
                   std::uint64_t input, ActionOutcome);
std::pair<NativeCompletion, record::Origin>
complete_authoring(amber::catalog::State &catalog_state, amber::react::State &react_state,
                   amber::step::State &step_state, space::Space &space, record::Journal &journal,
                   const Input &input, react::authoring::Completion completion, bool session_live,
                   const std::filesystem::path &asset_root,
                   const std::filesystem::path &script_root);

void declare_construction_actions(react::Terms &, react::NativeActions &);
NativeCompletion create_instance(amber::ObjectState &objects_state,
                                 const amber::catalog::State &catalog_state,
                                 const GeometryAssets &geometry, const react::ValueRead &values,
                                 amber::space::State &space_state,
                                 const amber::perm::State &perm_state, PhysicsRoles roles,
                                 PhysicsDefaults defaults, const NativeCall &call);
NativeCompletion remove_instance(const lifecycle::Parts &, const react::ValueRead &, AuthorityRead,
                                 AttributionRead, const perm::Ledger &, const catalog::Models &,
                                 const catalog::Scripts &, const NativeCall &);
void install_declared_attachments(amber::ObjectState &objects_state,
                                  amber::space::State &space_state,
                                  std::span<const Handle> objects);
step::NativeCompletion hold_goods(amber::perm::State &perm_state, amber::ObjectState &objects_state,
                                  const DeliveryTargets &delivery, const space::Space &space,
                                  std::size_t mailbox_max, const step::NativeCall &call);
void declare_hold_goods_action(react::Terms &terms, react::NativeActions &declarations);
std::string validate_hold_goods(const nlohmann::json &input);
} // namespace amber::step

namespace amber::step {
struct State {
  WorkQueues queues;
  TimerWheel timers;
  Intents intents;
  Inputs inputs;
  std::vector<IntentEnding> intent_endings;
  Clock clock;
  GeometryAssets geometry;
  MechanicalCauses mechanical_causes;
  TravelAccess travel_access;
  DeliveryTargets delivery;
  AssemblyMasses masses;
  IlluminationSources illumination_sources;
  NavigationRequests navigation_requests;
  std::vector<FactKey> mutated;
  std::vector<GapDraft> gaps;
  checkpoint::SnapshotProgress snapshot_progress;
  explicit State(std::size_t authors_max) : mechanical_causes(authors_max) {}
};
} // namespace amber::step
