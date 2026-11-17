#pragma once
namespace amber {
struct Settings;
}
namespace amber::step {
struct State;
} // namespace amber::step

#include "catalog/catalog.hpp"
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
struct LandSettings {
  float street_width_m, port_spacing_m, block_m, way_m, lot_front_m, lot_min_m;
  std::uint32_t pool_lots_min;
  GroupId way_group = 0;
};
struct Block {
  std::int32_t i, j;
};
void survey_block(perm::State &, space::State &, const LandSettings &, Block);
void fill_pool(perm::State &, space::State &, const LandSettings &);

struct InputWork {
  std::uint64_t id = 0;
};
using Work = std::variant<react::EventWork, react::WatchWork, InputWork>;

struct ForegroundItem {
  Work work;
  std::optional<FactKey> threshold;
};
struct WorkQueues {
  std::deque<ForegroundItem> causal, turns;
  std::deque<react::BackgroundItem> background;
  std::unordered_set<FactKey, FactKeyHash> watch_queued;
  std::size_t emitted_events = 0;
};
struct TimerLocation {
  unsigned level = 0, bucket = 0;
  std::size_t index = 0;
};
struct TimerWheel {
  Step now = 0;
  std::array<std::array<std::vector<react::Timer>, 256>, 4> levels;
  // Derived from pending threshold rows, never serialized. Ordinary timers
  // retain independent entries, including multiple events on the same target.
  std::unordered_map<FactKey, TimerLocation, FactKeyHash> thresholds;
};
enum class PushResult { Queued, Coalesced, Overflow };
using IntentId = std::uint64_t;
using MoveGoal = std::variant<Vec3, Handle>;
enum class IntentEnd { Arrived, Blocked, Cancelled, Replaced };
struct Intent {
  IntentId id = 0;
  MoveGoal goal;
  amber::Origin origin;
  std::optional<float> speed_m_s;
};
struct IntentEnding {
  Handle body;
  Intent intent;
  IntentEnd reason;
};
struct Intents {
  IntentId next_id = 1;
  std::unordered_map<std::uint64_t, Intent> moving;
};
struct IntentStart {
  IntentId id;
  std::optional<IntentEnding> replaced;
};

using Monotonic = std::chrono::steady_clock;
struct Settings {
  double step_hz = 0, margin_ms = 0;
  std::size_t average_steps = 0;
};
struct Clock {
  Step step = 0;
  double hz = 0;
  Monotonic::time_point next_start{};
  std::uint64_t overruns = 0;
};
struct Frame {
  Step step = 0;
  Monotonic::time_point started, handler_deadline, boundary;
  std::size_t foreground = 0, background = 0;
};
struct Input {
  std::uint64_t id = 0;
  net::ControlKind kind;
  Caller caller = PlayerId{0};
  Handle body;
  std::uint64_t session = 0, sequence = 0, request = 0;
  nlohmann::json fields = nlohmann::json::object();
  amber::Origin origin;
  bool awaiting = false;
};
struct Inputs {
  std::uint64_t next_id = 1;
  std::map<std::uint64_t, Input> rows;
};
struct InputReply {
  bool deferred = false;
  nlohmann::json fields = nlohmann::json::object();
  std::optional<amber::Origin> origin{};
};
struct InputCompletion {
  std::uint64_t id = 0, session = 0, sequence = 0, request = 0;
  nlohmann::json fields;
};
struct Bounds {
  Vec3 minimum, maximum;
};
struct InputOrder {
  std::uint64_t input = 0, session = 0, sequence = 0, request = 0;
};
struct GapDraft {
  Handle target;
  TermId term, action;
  PlayerId requester;
  amber::Origin origin;
};
struct NativeCall {
  ActionRequest request;
  nlohmann::json arguments;
  Handle body;
  Step step = 0;
  double hz = 0;
  amber::Origin origin;
  std::optional<InputOrder> order{};
  RunDeadline deadline{};
};
struct NativeCompletion {
  record::Result result = record::Result::NoEffect;
  bool changed = false;
  nlohmann::json reply = nlohmann::json::object();
  react::DeferredEffects effects;
  std::vector<record::Event> records;
  std::vector<std::pair<PlayerId, nlohmann::json>> notices;
  bool deferred = false;
  std::vector<Handle> changed_objects;
  std::vector<Handle> crossed_plots;
  std::vector<ModelId> changed_models;
  std::vector<ScriptId> changed_scripts;
  std::vector<TermId> changed_terms, changed_materials;
  bool permissions_changed = false;
  bool world_ambient_changed = false;
  std::vector<perm::CustodyChange> custody;
  std::vector<Handle> created_objects, removed_objects, visible_objects, hidden_objects;
  std::vector<lifecycle::Rebinding> rebound_objects;
};

struct DeliveryTargets {
  std::unordered_map<PlayerId, Handle> present_bodies;
};

struct ActionOutcome {
  nlohmann::json reply;
  amber::Origin origin;
  bool deferred = false;
};
} // namespace amber::step

namespace amber::step::authoring {
using Json = nlohmann::json;
struct AssetRequest {
  catalog::Hash asset;
  std::string canonical_params;
};
struct ModelRequest {
  catalog::Hash asset;
  std::string canonical_params;
  TermId material = 0;
  catalog::Text text;
  std::vector<AssetRequest> variants;
};
struct ScriptRequest {
  std::string source;
};
struct VariantRequest {
  ModelId model = 0;
  catalog::Hash asset;
  std::string canonical_params;
};
ModelRequest model_request(const Json &, const react::Terms &);
VariantRequest variant_request(const Json &);
ScriptRequest script_request(const Json &);
struct RegistrySnapshot {
  react::Terms terms;
  catalog::Hash pin;
};
struct WorkerSettings {
  std::filesystem::path asset_root;
  catalog::RuntimePins pins;
  react::VmSettings limits;
};
struct PreparedModel {
  std::vector<catalog::Asset> assets; // root first, then its real declared parts
  TermId material = 0;
  catalog::Text text;
  std::vector<std::vector<catalog::Asset>> variants;
};
struct PreparedVariant {
  ModelId model = 0;
  std::vector<catalog::Asset> assets;
};
struct Failure {
  std::string error;
};
struct Completion {
  std::uint64_t input_id = 0;
  std::variant<PreparedModel, PreparedVariant, catalog::PreparedScript, Failure> result;
};
class Worker {
public:
  explicit Worker(WorkerSettings);
  ~Worker();
  Worker(const Worker &) = delete;
  Worker &operator=(const Worker &) = delete;
  void submit(std::uint64_t input_id, ModelRequest, RegistrySnapshot);
  void submit(std::uint64_t input_id, VariantRequest, RegistrySnapshot);
  void submit(std::uint64_t input_id, ScriptRequest, RegistrySnapshot);
  std::vector<Completion> poll();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// These writer functions install only validated immutable submissions. Geometry
// preparation and record/publication policy belong to step's direct caller.
step::NativeCompletion install_model(PreparedModel, PlayerId author, double created_s,
                                     amber::catalog::State &catalog_state,
                                     amber::react::State &react_state,
                                     catalog::MeasurementBindings);
step::NativeCompletion install_variant(PreparedVariant, amber::catalog::State &catalog_state,
                                       amber::react::State &react_state,
                                       catalog::MeasurementBindings);
step::NativeCompletion install_script(catalog::PreparedScript, PlayerId author, double created_s,
                                      catalog::Scripts &, react::Vm &, const react::NativeActions &,
                                      const std::filesystem::path &script_root);
Json failure(std::string);
} // namespace amber::step::authoring

namespace amber::step {
inline constexpr char runtime_version[] = "ambergris-0.8";

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
void append_inputs(WorkQueues &, std::vector<Work>);
void promote_due(TimerWheel &, Step now, WorkQueues &);
std::optional<ForegroundItem> pop_foreground(WorkQueues &);
std::optional<react::BackgroundItem> pop_background(WorkQueues &);
PushResult enqueue_event(WorkQueues &, react::EventWork, std::size_t limit);
PushResult enqueue_watch(WorkQueues &, react::WatchWork);
void enqueue_background(WorkQueues &, react::BackgroundItem);
void schedule(TimerWheel &, react::Timer);
void discard_script_work(WorkQueues &, TimerWheel &, std::span<const Handle>);
void cancel_threshold(TimerWheel &, FactKey);
void apply_thresholds(TimerWheel &, std::vector<react::ThresholdChange>);
void refresh_rule_thresholds(TimerWheel &, const react::State &, const react::ValueRead &,
                             const catalog::Scripts &, std::span<const ScriptId>, const Clock &);
void rebuild_queue_index(WorkQueues &);
std::vector<react::Timer> timers(const TimerWheel &);

IntentStart start_move(Intents &, Handle, MoveGoal, amber::Origin,
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

bool in_reach(Vec3 actor_point, Bounds target, float reach_m);

EventPayload normalize_arguments(const react::Terms &, TermId action, EventPayload);
std::vector<react::PlannedInvocation> plan_event(const react::Triggers &, const react::ValueRead &,
                                                 const react::EventWork &, Step, double hz);
std::vector<react::PlannedInvocation> plan_action(const react::Triggers &, const react::ValueRead &,
                                                  const react::Resolution &, const ActionRequest &,
                                                  Handle authenticated_body, const amber::Origin &,
                                                  Step, double hz);

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
                      std::size_t event_limit, const space::StepResult &,
                      const amber::step::Clock &clock);
const amber::Origin &move_origin(const Intents &, Handle);
void finish_motion(State &, std::span<const space::IntentProgress>);

step::NativeCompletion grasp_action(amber::ObjectState &objects_state,
                                    amber::space::State &space_state,
                                    const PhysicalRelationTerms &relation_terms, float reach_m,
                                    const perm::Plots &plots, const step::NativeCall &call);
step::NativeCompletion attach_action(amber::ObjectState &objects_state,
                                     amber::space::State &space_state,
                                     const PhysicalRelationTerms &relation_terms,
                                     const amber::perm::State &perm_state, float reach_m,
                                     const step::NativeCall &call);
step::NativeCompletion release_action(amber::ObjectState &objects_state,
                                      amber::space::State &space_state,
                                      const PhysicalRelationTerms &relation_terms,
                                      const amber::perm::State &perm_state,
                                      const step::NativeCall &call);
step::NativeCompletion impulse_action(space::Space &world, IdentityRead identity,
                                      const step::NativeCall &call);
step::NativeCompletion transfer_action(amber::react::State &react_state,
                                       const amber::ObjectState &objects_state,
                                       const amber::perm::State &perm_state,
                                       const step::NativeCall &call);
void bind_physical(react::State &, amber::ObjectState &objects_state,
                   amber::step::State &step_state, const amber::perm::State &perm_state,
                   std::size_t part_batch_max, amber::space::State &space_state,
                   const amber::catalog::State &catalog_state);
void declare_physical_actions(react::Terms &, react::NativeActions &);

std::vector<Handle> plot_things(const ObjectState &, PlotId, const RunDeadline *deadline = nullptr);
void bind_metadata(const amber::ObjectState &objects_state,
                   const amber::catalog::State &catalog_state, amber::react::State &react_state,
                   const perm::State &, float port_spacing_m);

using Json = nlohmann::json;
struct CommerceDeclarations {
  TermId notify = 0, pay = 0, balance = 0, inventory = 0, give = 0, offer = 0, offers = 0,
         accept = 0, cancel_offer = 0, trade_open = 0, trade_change = 0, trade_confirm = 0,
         trade_cancel = 0, place_existing = 0, offer_plot = 0, set_offer_price = 0,
         return_goods = 0, abandon_plot = 0, split_plot = 0, join_plots = 0;
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
void notify(perm::Players &, net::Sessions &, std::size_t mailbox_max, PlayerId, Json);
step::NativeCompletion deliver_mailbox(perm::Players &, net::Sessions &,
                                       const amber::ObjectState &objects_state,
                                       const space::Space &, PlayerId, Handle authenticated_body,
                                       Step);
std::string check_input(const Json &schema, const Json &arguments);
NativeCompletion script_notice(const perm::State &, const NativeCall &);
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
step::NativeCompletion offer_plot(perm::State &, const NativeCall &);
step::NativeCompletion set_offer_price(perm::Offers &, const NativeCall &);
step::NativeCompletion offers(const perm::Offers &offers, const perm::Players &players,
                              IdentityRead identity, AuthorityRead authority,
                              const step::NativeCall &call);
step::NativeCompletion accept(amber::perm::State &perm_state, amber::ObjectState &objects_state,
                              space::State &space_state, const DeliveryTargets &delivery,
                              const amber::Settings &settings, const step::NativeCall &call,
                              TermId plot_changed);
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
               PhysicsRoles roles, PhysicsDefaults defaults, const amber::react::State &react_state,
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
step::NativeCompletion grant(amber::perm::State &perm_state,
                             const amber::ObjectState &objects_state,
                             const amber::catalog::State &catalog_state,
                             const amber::react::State &react_state, const step::NativeCall &call);
step::NativeCompletion revoke(amber::perm::State &perm_state,
                              const amber::ObjectState &objects_state, bool architect,
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
step::NativeCompletion plot_permissions(perm::State &, const LandSettings &,
                                        const step::NativeCall &, TermId plot_changed);

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
  amber::Origin origin;
  react::DeferredEffects effects;
  std::vector<ScriptId> changed_scripts;
};
FixtureCompletion complete_fixture(amber::elaborate::State &elaborate_state,
                                   amber::step::State &step_state, catalog::Scripts &scripts,
                                   amber::react::State &react_state, const react::ValueRead &values,
                                   elaborate::RoleAccess current_role,
                                   const elaborate::FixtureResult &result,
                                   const amber::step::Clock &clock);
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
         set_setting = 0, model_setting = 0, world_setting = 0, patch_state = 0, relate = 0,
         define_term = 0, define_material = 0, edit_detail = 0;
};
Declarations declare_editing(react::Terms &, react::NativeActions &);
step::NativeCompletion edit_text(const step::NativeCall &call,
                                 catalog::InstanceTexts &instance_texts,
                                 const catalog::ModelTexts &model_texts, IdentityRead identity);
step::NativeCompletion world_text(const step::NativeCall &, std::vector<react::WorldScript> &);
step::NativeCompletion model_text(const step::NativeCall &call, catalog::ModelTexts &model_texts,
                                  const catalog::Models &models, IdentityRead identity,
                                  const perm::Players &players);
step::NativeCompletion edit_detail(const step::NativeCall &call, catalog::AuthoredTexts &details);
step::NativeCompletion set_setting(const step::NativeCall &call, react::State &react_state,
                                   const react::ValueRead &values);
step::NativeCompletion model_setting(const step::NativeCall &call, react::State &react_state,
                                     const react::ValueRead &values, const catalog::Models &models,
                                     const perm::Players &players);
step::NativeCompletion world_setting(const step::NativeCall &, react::State &,
                                     const react::ValueRead &, std::span<const std::string> roles);
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
                    const amber::step::Clock &clock, const amber::Origin &, NativeCompletion &);

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
    std::uint64_t next_record, const CaptureSelection *selection);
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
void invalidate_world_illumination(space::State &);
void advance_illumination(IlluminationSources &, amber::space::State &space_state,
                          const react::ValueRead &, Step, double hz);
void sync_illuminance_reads(observe::LightSubscriptions &, const react::State &,
                            const react::ValueRead &, const catalog::Scripts &, TransformRead,
                            TermId illuminance, std::span<const Handle>, const Clock &);
void refresh_illuminance_reads(observe::LightSubscriptions &, const react::State &,
                               const react::ValueRead &, const catalog::Scripts &, TransformRead,
                               TermId illuminance, const Clock &, bool discover);
void move_illuminance_reads(observe::LightSubscriptions &, TransformRead, TermId illuminance,
                            std::span<const Handle> moved);
void retire_illuminance_reads(observe::LightSubscriptions &, react::RuntimeMeasurements &,
                              TermId illuminance, std::span<const Handle>);
void sample_illuminance(observe::LightSubscriptions &, const space::Space &, IdentityRead,
                        const observe::Lights &, const observe::ZoneAmbients &,
                        react::RuntimeMeasurements &, double world_ambient_lux,
                        react::DeferredEffects &);

void write_timers(snap::Output &, const TimerWheel &, const snap::ObjectSelection & = {});
void read_timers(snap::Input &, TimerWheel &);
void write_queues(snap::Output &, const WorkQueues &, const Intents &, std::uint64_t next_record,
                  const snap::ObjectSelection & = {});
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
TermId declare_model_edit(react::Terms &, react::NativeActions &);
// Direct lifecycle domain operations; callers compose existing owner functions in order.
void retire_commerce(amber::perm::State &perm_state, amber::ObjectState &objects_state,
                     std::span<const Handle> removed, Step, NativeCompletion &);
void retire_physics(space::Space &, amber::ObjectState &objects_state, space::PhysicalRelations &,
                    amber::step::State &step_state, std::span<const Handle> removed,
                    std::array<TermId, 4> relation_terms, const amber::Origin &,
                    NativeCompletion &);
void retire_publication(net::ReplicationIndex &, net::SessionViews &, catalog::InstanceTexts &,
                        std::span<const Handle> removed);
void refresh_object_plots(const space::Space &, const ObjectState &, react::State &,
                          const catalog::State &, const Clock &, const amber::Origin &,
                          NativeCompletion &);

double outdoor_lux(const react::State &);
void publish_catalog_changes(net::Sessions &, net::SessionViews &, const catalog::Models &,
                             const catalog::ModelTexts &, const react::Terms &,
                             const react::Materials &, net::MaterialAppearanceRoles,
                             double world_ambient_lux, NativeCompletion &);
void publish_object_changes(net::Sessions &, net::SessionViews &, IdentityRead, AuthorityRead,
                            ParentRead, const catalog::ModelTexts &, const catalog::InstanceTexts &,
                            std::span<const Handle> changed);

void reconcile_effects(const react::ValueRead &, react::State &, step::State &, const Clock &,
                       react::DeferredEffects &);
void record_send_overflow(record::Journal &, const net::SendOverflow &, Step);
void publish_effects(react::DeferredEffects &&, const amber::Origin &, Step, WorkQueues &,
                     TimerWheel &, record::Journal &, std::size_t event_limit);
amber::Origin publish_invocation(record::Journal &, WorkQueues &, TimerWheel &,
                                 react::CompletedInvocation &&, const amber::Origin &,
                                 std::size_t event_limit);
using AdmittedWork = std::variant<ForegroundItem, react::BackgroundItem, elaborate::FixtureResult,
                                  step::authoring::Completion>;
std::optional<AdmittedWork> next_work(WorkQueues &, Frame &, std::deque<elaborate::FixtureResult> &,
                                      std::deque<step::authoring::Completion> &,
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
void install_land(const Json &, perm::State &, space::State &, const LandSettings &);
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
  std::string profile = "production";
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
InputReply submit_authoring(step::authoring::Worker &, const react::Terms &,
                            const catalog::RegistryDocument &, Input &);
void deliver_input(Inputs &, net::Sessions &, observe::Hearing &, std::uint32_t event_limit,
                   std::uint64_t input, ActionOutcome);
std::pair<NativeCompletion, amber::Origin>
complete_authoring(amber::catalog::State &catalog_state, amber::react::State &react_state,
                   amber::step::State &step_state, space::Space &space, record::Journal &journal,
                   const Input &input, step::authoring::Completion completion, bool session_live,
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
NativeCompletion abandon_plot(perm::State &, ObjectState &, const DeliveryTargets &,
                              const space::Space &, const LandSettings &, const NativeCall &,
                              TermId plot_changed);
NativeCompletion split_plot(perm::State &, ObjectState &, const LandSettings &, const NativeCall &,
                            TermId plot_changed);
NativeCompletion join_plots(perm::State &, ObjectState &, const LandSettings &, const NativeCall &,
                            TermId plot_changed);
step::NativeCompletion return_goods(perm::State &, ObjectState &, const DeliveryTargets &,
                                    const space::Space &, const NativeCall &, bool forced = false);

struct State {
  WorkQueues queues;
  TimerWheel timers;
  Intents intents;
  Inputs inputs;
  std::vector<IntentEnding> intent_endings;
  Clock clock;
  GeometryAssets geometry;
  DeliveryTargets delivery;
  IlluminationSources illumination_sources;
  NavigationRequests navigation_requests;
  std::vector<FactKey> mutated;
  std::vector<GapDraft> gaps;
  checkpoint::SnapshotProgress snapshot_progress;
};
} // namespace amber::step
