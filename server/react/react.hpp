#pragma once
namespace amber::catalog {
struct State;
struct Models;
struct Scripts;
} // namespace amber::catalog
namespace amber::react {
struct State;
}

#include "record/record.hpp"
#include "snap/snap.hpp"
#include "store/store.hpp"

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <unordered_map>

struct lua_State;

namespace amber::catalog {
struct Models;
struct Scripts;
} // namespace amber::catalog

namespace amber::react {
struct EventWork {
  Handle receiver, sender;
  TermId event = 0;
  EventPayload payload;
  amber::Origin origin;
  ScriptId recipient_script = 0; // internal targeted delivery; zero broadcasts normally
};
struct WatchWork {
  FactKey source;
  amber::Origin origin;
};
struct BackgroundItem {
  Handle receiver;
  TermId trigger = 0;
  ScriptId script = 0;
  amber::Origin origin;
  std::uint32_t version = 0;
};
struct ThresholdToken {
  FactKey source;
  Handle consumer;
  ScriptId script = 0;
  std::uint32_t watch_index = 0;
};
struct Timer {
  Step due = 0;
  react::EventWork event;
  std::optional<react::ThresholdToken> threshold;
};

enum class Kind { Quantity, Flag, Event, Relation, Action, Material };

enum class Source { Intrinsic, Measured, State, Setting };

enum class Combine { None, Sum, Max, Any, ShareEqual, SharePriority };

enum class MeasurementRole { Baked, Volume, Mass, Runtime };

enum class InputType { Number, Integer, Boolean, Handle, Vector, String, Term };

struct Input {
  std::string name;
  InputType type = InputType::Number;
  TermId term = 0;
  bool optional = false;
};

struct Term {
  TermId id = 0;
  std::string name;
  Kind kind = Kind::Quantity;
  Source source = Source::State;
  std::array<std::int8_t, 8> dimensions{};
  bool conserved = false;
  Combine combine = Combine::None;
  TermId along = 0, paired = 0, priority = 0;
  PlayerId owner = 0;
  std::string description;
  std::vector<Input> inputs;
  MeasurementRole measurement = MeasurementRole::Baked;
  std::vector<TermId> compared;
  std::string distinction;
};

struct Terms {
  std::vector<Term> rows{Term{}};
  std::unordered_map<std::string, TermId> by_name;
};

using Values = std::map<TermId, Value>;

struct Materials {
  std::unordered_map<TermId, Values> rows;
};

struct AssetMeasurements {
  struct Row {
    double volume = 0;
    Values values;
  };
  std::unordered_map<ModelId, Row> rows;
  std::unordered_map<std::uint32_t, Row> parameter_rows;
};

using Properties = std::vector<SparseColumn<Value>>;

struct Process {
  double v0 = 0;
  Step step0 = 0;
  double rate = 0, lo = 0, hi = 0;
};

using Processes = std::vector<SparseColumn<Process>>;

struct InstanceSettings {
  Properties columns;
};

struct ModelSettings {
  std::unordered_map<ModelId, Values> rows;
};

struct PlotSettings {
  std::unordered_map<PlotId, Values> rows;
};

struct WorldSettings {
  Values values;
};

struct Contribution {
  Handle from;
  double amount = 0;
};

struct Shared {
  double total = 0;
  std::vector<Contribution> parts;
};

using SharedColumns = std::vector<SparseColumn<Shared>>;

struct SubjectTerm {
  Handle subject;
  TermId term = 0;
  bool operator==(const SubjectTerm &) const = default;
};

using GivesTo = SparseColumn<std::vector<SubjectTerm>>;

using AuthoredRelations = std::unordered_map<TermId, Relation>;

struct RelationsRead {
  std::unordered_map<TermId, const Relation *> columns;
};

struct Answer {
  bool present = false;
  Value value;
  bool operator==(const Answer &) const = default;
};

struct RuntimeMeasurements {
  std::unordered_map<FactKey, Value, FactKeyHash> values;
};

struct DerivedFactInput {
  FactKey source;
  Value value;
  amber::Origin origin;
};

struct ValueRead {
  const Terms &terms;
  IdentityRead identity;
  const std::vector<PlotId> &plots;
  const std::vector<std::uint32_t> &params;
  const Materials &materials;
  const AssetMeasurements &assets;
  const Properties &properties;
  const Processes &processes;
  const InstanceSettings &instance_settings;
  const ModelSettings &model_settings;
  const PlotSettings &plot_settings;
  const WorldSettings &world_settings;
  const SharedColumns &shared;
  const RelationsRead &relations;
  const RuntimeMeasurements &runtime;
  TermId density = 0, capacity_term = 0;
};

struct HandlerRef {
  ScriptId script = 0;
  std::string function;
  PlayerId author = 0;
  std::uint32_t version = 0;
  bool enabled = true;
};

struct Trigger {
  std::unordered_map<TermId, HandlerRef> by_material;
  std::vector<std::pair<TermId, HandlerRef>> by_flag;
  std::unordered_map<ModelId, HandlerRef> by_model;
  std::unordered_map<std::uint64_t, std::vector<HandlerRef>> by_instance;
};

using Triggers = std::unordered_map<TermId, Trigger>;

struct RuleVersion {
  HandlerRef handler;
  TermId trigger = 0, selector = 0;
  bool flag_selector = false;
};

struct RuleVersions {
  std::vector<RuleVersion> history;
};

struct Threshold {
  enum class Predicate { Above, AtLeast, Below, AtMost } predicate = Predicate::Above;
  double value = 0;
  bool test(double current) const;
};

struct RelationAnswer {
  std::vector<Handle> out, in;
  bool operator==(const RelationAnswer &) const = default;
};

struct SharedWatchAnswer {
  Answer aggregate;
  double own_allocation = 0;
  bool operator==(const SharedWatchAnswer &) const = default;
};

using WatchAnswer = std::variant<Answer, RelationAnswer, SharedWatchAnswer>;

struct RelationDelta {
  Handle other;
  bool added = false, incoming = false;
};

struct WatchDeclaration {
  TermId term = 0;
  std::vector<Threshold> thresholds;
};

struct WatchBinding {
  ScriptKey consumer;
  std::uint32_t index = 0;
  WatchAnswer last = Answer{};
  WatchDeclaration declaration;
  std::vector<FactKey> dependencies;
};

struct Subscriptions {
  std::unordered_map<FactKey, std::vector<WatchBinding>, FactKeyHash> rows;
};

struct CurrentAnswers {
  std::unordered_map<FactKey, WatchAnswer, FactKeyHash> values;
  std::unordered_map<ScriptKey, WatchAnswer, ScriptKeyHash> by_consumer;
};

struct WatchDelivery {
  ScriptKey consumer;
  std::uint32_t index;
  WatchAnswer value;
  std::vector<RelationDelta> relations;
};

struct Meter {
  double amount0 = 0;
  Step step0 = 0;
  double rate = 0;
};

struct AllocationKey {
  Handle subject, contributor;
  TermId term = 0;
  bool supplier = false;
  bool operator==(const AllocationKey &) const = default;
};

struct AllocationKeyHash {
  std::size_t operator()(const AllocationKey &) const;
};

struct AllocationRows {
  std::unordered_map<AllocationKey, double, AllocationKeyHash> values;
};

using MeterRows = std::unordered_map<AllocationKey, Meter, AllocationKeyHash>;

struct PriorityValues {
  std::unordered_map<std::uint64_t, double> values;
};

struct SettingKey {
  enum class Scope { Instance, Model, Plot, World } scope = Scope::Instance;
  std::uint64_t subject = 0;
  TermId term = 0;
  bool operator==(const SettingKey &) const = default;
};

using StateKey = std::variant<std::string, std::int64_t>;

using StateAtom = std::variant<double, bool, std::string, Handle>;

struct StateKeyHash {
  std::size_t operator()(const StateKey &) const;
};

using ScriptState = std::unordered_map<StateKey, StateAtom, StateKeyHash>;

using ScriptStates = std::unordered_map<ScriptKey, ScriptState, ScriptKeyHash>;

using RandomStates = std::unordered_map<std::uint64_t, std::uint64_t>;

struct Diagnostic {
  std::string name, message;
};

struct ContributionChange {
  FactKey source;
};

using Effect = std::variant<react::EventWork, react::WatchWork, react::Timer, Diagnostic,
                            record::Event, ContributionChange>;

struct DeferredEffects {
  std::vector<Effect> ordered;
  bool changed = false;
  std::vector<Handle> consulted_handles{};
  std::vector<react::BackgroundItem> background{};
};

struct ActionDeclaration {
  TermId action = 0;
  std::string right, function;
  bool reach = false;
};

struct ScriptDeclaration {
  enum class Kind { Model, Instance, Rule } kind = Kind::Model;
  ScriptId id = 0;
  PlayerId author = 0;
  std::uint32_t version = 1;
  bool approved = false, active = true;
  std::vector<WatchDeclaration> watches;
  std::vector<TermId> contributes, sets, events;
  std::vector<std::string> slots;
  std::vector<ActionDeclaration> actions;
  std::string selector;
  std::size_t tests = 0;
  std::vector<TermId> handlers;
};

struct RightsDeclaration {
  std::vector<std::string> names;
  std::vector<std::string> slot_names{};
};

struct ScriptValidation {
  std::vector<std::string> errors;
  explicit operator bool() const { return errors.empty(); }
};

struct CompiledScript {
  std::string bytecode;
  std::vector<std::string> errors;
};

struct Invocation {
  ScriptKey self;
  Step step = 0;
  double hz = 20;
  TermId trigger = 0;
  Handle sender;
  EventPayload payload;
  std::vector<Handle> acquired, event_targets, parts;
  // Descriptive action identity; it never confers permission on the script.
  std::optional<Caller> action_caller;
  Handle action_body, action_target;
  amber::Origin origin;
  ScriptState *state = nullptr;
  std::uint64_t *random = nullptr;
  DeferredEffects *effects = nullptr;
  RunDeadline deadline{};
  const ScriptDeclaration *declaration = nullptr;
};

struct RunOutcome {
  enum class Status { Ok, Error, Cutoff, Missing, Action } status = Status::Missing;
  std::string error;
  std::size_t memory_bytes = 0;
  struct Action {
    ActionRequest request;
    nlohmann::json arguments;
  };
  std::optional<Action> action{};
};

struct WatchRun {
  FactKey source;
  std::uint32_t index;
  WatchAnswer value;
};

struct InvocationInput {
  Handle receiver, sender;
  TermId trigger = 0;
  EventPayload payload;
  Step step = 0;
  double hz = 20;
  amber::Origin origin;
  std::vector<Handle> acquired;
  std::optional<Caller> action_caller;
  Handle action_body, action_target;
  RunDeadline deadline{};
  std::optional<WatchRun> watch;
};

struct CompletedInvocation {
  HandlerRef handler;
  Handle receiver;
  TermId trigger = 0;
  Step step = 0;
  RunOutcome outcome;
  DeferredEffects effects;
  std::vector<Handle> things{};
  std::vector<record::Credit> object_credits{}, script_credits{};
  amber::Origin origin{};
};

struct VmSettings {
  std::size_t script_kb;
  double declaration_ms, handler_ms;
  std::size_t event_fields_max;
};

struct ScriptAnalysis {
  CompiledScript compiled;
  ScriptDeclaration declaration;
  nlohmann::json metadata = nlohmann::json::object();
  std::vector<std::string> errors;
};

using InstanceScripts = SparseColumn<std::vector<ScriptId>>;

struct RuleKey {
  TermId trigger = 0, selector = 0;
  bool flag = false;
  auto operator<=>(const RuleKey &) const = default;
};

struct RuleChange {
  std::vector<RuleVersion> active, retired;
};

enum class SyncMode { Initial, Change, Restore };

struct WatchChanges {
  std::vector<react::WatchWork> dirty;
  std::vector<react::Timer> timers;
};

struct WatchResult {
  FactKey source;
  std::vector<WatchDelivery> deliveries;
  std::vector<react::Timer> timers;
};

struct InvocationPlan {
  HandlerRef handler;
  Handle receiver, sender;
  TermId trigger = 0;
  EventPayload payload;
  std::vector<Handle> acquired;
  std::optional<WatchRun> watch{};
};

using ObjectSelection = std::optional<std::span<const Handle>>;

enum class RuleMode { Validated, Enable, Disable };

enum class MechanicAction { Grasp, Release, Attach, Impulse, Transfer };
enum class ConstructionAction { Create, Destroy, Reconfigure, ReviseModel, PlaceExisting };
enum class ExchangeAction {
  ReturnGoods,
  OfferPlot,
  SetOfferPrice,
  HoldGoods,
  Pay,
  Balance,
  Inventory,
  Give,
  Offer,
  Offers,
  Accept,
  CancelOffer,
  TradeOpen,
  TradeChange,
  TradeConfirm,
  TradeCancel
};
enum class PermissionAction {
  Grant,
  Revoke,
  ListGrants,
  Preset,
  GroupCreate,
  GroupMember,
  PlotPermissions
};
enum class ScriptAction { Install, Uninstall, ModelScript };
enum class WritingAction { EditText, ModelText, EditDetail };
enum class ValueAction { SetSetting, ModelSetting, PatchState, Relate };
enum class RegistryAction { DefineTerm, DefineMaterial };
enum class ElaborationAction { RequestModel, Requests, Claim, Answer, Refuse, RuleActivation };
using NativeOperation =
    std::variant<MechanicAction, ConstructionAction, ExchangeAction, PermissionAction, ScriptAction,
                 ValueAction, WritingAction, RegistryAction, ElaborationAction>;
struct NativeDeclaration {
  TermId action = 0;
  std::string right;
  bool reach = false;
  nlohmann::json inputs_schema = nlohmann::json::object();
  NativeOperation operation;
};
struct NativeActions {
  std::unordered_map<TermId, NativeDeclaration> rows;
};
using InstalledScripts = SparseColumn<std::vector<ScriptId>>;
enum class Route { Native, Body, Selected, Fallback };
enum class Failure { None, UnknownAction, StaleTarget, MissingTarget };
struct Resolution {
  Failure failure = Failure::None;
  Route route = Route::Fallback;
  Handle receiver, permission_target;
  std::string right;
  bool reach = false;
  ScriptId declaration_script = 0;
  NativeOperation native_operation;
  explicit operator bool() const { return failure == Failure::None; }
};
struct ArgumentError {
  std::string input, problem;
};

ValueRead value_read(const ObjectState &, const catalog::State &, const State &);

Answer read_intrinsic(const Materials &, TermId material, const Term &);
Answer read_baked(const AssetMeasurements &, ModelId, const Term &, std::uint32_t params = 0);
double read_mass(double volume, double density);
double process_value(const Process &, Step now, double hz);
Answer read_state(const Properties &, const Processes &, Handle, TermId, Step, double hz);
Answer read_setting(const InstanceSettings &, const ModelSettings &, const PlotSettings &,
                    const WorldSettings &, Handle, ModelId, PlotId, TermId);
Answer read_shared(const SharedColumns &, const RelationsRead &, Handle, const Term &);
Answer read_runtime_measurement(const RuntimeMeasurements &, FactKey);
Answer read(const ValueRead &, Handle, TermId, Step, double hz);

std::vector<HandlerRef> dispatch(const Triggers &, IdentityRead, std::span<const TermId> flags,
                                 Handle receiver, TermId trigger);
std::vector<HandlerRef> dispatch(const Triggers &, const ValueRead &, Handle receiver,
                                 TermId trigger, Step, double hz);

std::optional<std::pair<HandlerRef, std::size_t>> find_handler(const Triggers &, const ValueRead &,
                                                               ScriptKey, TermId, Step, double hz);
void acknowledge_watch(Subscriptions &, ScriptKey, const WatchRun &);

RelationAnswer read_relation(const Relation &, Handle);

std::vector<WatchDelivery> check_watch(Subscriptions &, FactKey, const CurrentAnswers &,
                                       ScriptId recipient = 0);
std::optional<Step> next_crossing(const Process &, const Threshold &, Step now, double hz);
bool valid_threshold(const react::ThresholdToken &, IdentityRead, const Processes &,
                     const Subscriptions &, Step now, double hz);
void bind_watch(Subscriptions &, FactKey, WatchBinding);
void unbind_watch(Subscriptions &, ScriptKey, std::uint32_t index);
void retire_subscriptions(Subscriptions &, Handle);
std::vector<FactKey> watch_sources(const Subscriptions &, ScriptKey);
std::vector<FactKey> watch_sources(const Subscriptions &, Handle subject);
std::vector<ScriptKey> watch_consumers(const Subscriptions &, Handle object);
std::vector<react::Timer> threshold_timers(const Subscriptions &, const Processes &, FactKey,
                                           Step now, double hz, const amber::Origin &);

void validate_shared_terms(const Terms &);
std::vector<FactKey> allocation_inputs(const Terms &, FactKey);
std::vector<FactKey> priority_inputs(const ValueRead &, SettingKey);
bool reconcile_allocation(const ValueRead &, amber::react::State &react_state, FactKey demand, Step,
                          double hz);

double metered(const Meter &, Step now, double hz);
std::vector<AllocationKey> allocate(const Shared &supply, const Shared &demand, Combine,
                                    const PriorityValues &, Handle subject, TermId supply_term,
                                    TermId demand_term, amber::react::State &react_state, Step now,
                                    double hz);
double get_share(const AllocationRows &, Handle subject, TermId demand_term, Handle contributor);
double live_share(const ValueRead &, Handle subject, TermId demand_term, Handle contributor,
                  Step now, double hz);
void retire_allocations(AllocationRows &, MeterRows &, Handle);

bool set_state(Properties &, Processes &, const Term &, Handle, Value, Step, double hz,
               DeferredEffects &);
bool set_rate(amber::react::State &react_state, const Term &, Handle, double rate, double lo,
              double hi, Step now, double hz, DeferredEffects &);
bool transfer_state(amber::react::State &react_state, const Term &, Handle from, Handle to,
                    double amount, Step, double hz, DeferredEffects &);
bool set_contribution(SharedColumns &, GivesTo &, const Term &, Handle from, Handle subject,
                      double amount, DeferredEffects &);
void withdraw_contributor(SharedColumns &, GivesTo &, const Terms &, Handle, DeferredEffects &);
bool set_relation(Relation &, TermId, Handle from, Handle to, bool present, DeferredEffects &);
bool apply_derived_fact(RuntimeMeasurements &, const DerivedFactInput &, DeferredEffects &);
void retire_object(Properties &, Processes &, ScriptStates &, RandomStates &, Handle,
                   std::span<const ScriptId> installed_scripts);

ScriptValidation validate_script(std::string_view, const Terms &, const ScriptDeclaration &,
                                 const RightsDeclaration &);
CompiledScript compile_script(std::string_view);

void consult_object(const Invocation &, Handle);

class Vm;
// Direct VM execution.
bool start_invocation(amber::react::State &react_state, const amber::ObjectState &objects_state,
                      std::uint64_t initial_seed, const amber::catalog::State &catalog_state,
                      record::Journal &, const HandlerRef &, InvocationInput, Invocation &,
                      CompletedInvocation &);
void finish_invocation(IdentityRead, AttributionRead, const catalog::Models &,
                       const catalog::Scripts &, record::Journal &, Invocation &,
                       CompletedInvocation &);
void refresh_parts(Invocation &, IdentityRead, const Relation &declared_parts);

class Vm {
public:
  explicit Vm(VmSettings);
  ~Vm();
  Vm(const Vm &) = delete;
  Vm &operator=(const Vm &) = delete;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  friend void register_binding(Vm &, std::string_view, int (*)(lua_State *), std::shared_ptr<void>);
  friend void bind_values(Vm &, const ValueRead &, State &);
  friend RunOutcome load_script(Vm &, const CompiledScript &, const ScriptDeclaration &);
  friend RunOutcome run_handler(Vm &, const HandlerRef &, Invocation &);
  friend RunOutcome resume_handler(Vm &, Invocation &, const nlohmann::json &);
  friend bool set_activation(Vm &, ScriptId, bool, bool);
  friend ScriptAnalysis analyze_script(std::string_view, const Terms &, const RightsDeclaration &,
                                       VmSettings);
};
void register_binding(Vm &, std::string_view, int (*)(lua_State *),
                      std::shared_ptr<void> binding_local);
void bind_events(Vm &, const Terms &);
ScriptAnalysis analyze_script(std::string_view, const Terms &, const RightsDeclaration &,
                              VmSettings);
RunOutcome load_script(Vm &, const CompiledScript &, const ScriptDeclaration &);
RunOutcome run_handler(Vm &, const HandlerRef &, Invocation &);
// An act call suspends only the currently running handler. The step executes
// its checked action and immediately resumes this same call with the result.
RunOutcome resume_handler(Vm &, Invocation &, const nlohmann::json &);
bool set_activation(Vm &, ScriptId, bool approved, bool active);
Invocation &invocation(lua_State *);
void *binding_data(lua_State *);
void push_handle(lua_State *, Handle);
Handle check_handle(lua_State *, int index);
EventPayload check_event_payload(lua_State *, int index);
// Core value methods keep their required C-callback data private to the VM.
void bind_values(Vm &, const ValueRead &, State &);

void set_instances(InstanceScripts &, Handle, std::span<const ScriptId>, const catalog::Scripts &);
std::vector<ScriptId> installed_scripts(const catalog::Models &, const InstanceScripts &,
                                        IdentityRead, Handle);
void project_model(Triggers &, const catalog::Models &, const catalog::Scripts &, const Terms &,
                   ModelId);
void project_instance(Triggers &, const InstanceScripts &, const catalog::Scripts &, const Terms &,
                      Handle);
RuleChange add_rule(RuleVersions &, const catalog::Scripts &, const Terms &, ScriptId);
RuleChange set_rule_enabled(RuleVersions &, RuleKey, bool);
std::vector<RuleVersion> latest_rules(const RuleVersions &);
void project_rules(Triggers &, const RuleVersions &);
std::vector<ScriptId> rule_scripts(const RuleVersions &, const ValueRead &, Handle, Step,
                                   double hz);
std::vector<TermId> rule_watch_terms(const RuleVersions &, const ValueRead &, Handle, ScriptId,
                                     Step, double hz);

WatchChanges sync_watches(amber::react::State &react_state, const ValueRead &, ScriptKey,
                          const ScriptDeclaration &, Step now, double hz, const amber::Origin &,
                          SyncMode, std::optional<std::span<const TermId>> allowed_terms = {});
void remove_watches(Subscriptions &, ScriptKey);
WatchChanges refresh_dependencies(Subscriptions &, const ValueRead &, FactKey changed, Step,
                                  double hz, const amber::Origin &);
WatchChanges sync_rule_watches(amber::react::State &react_state, const ValueRead &,
                               const catalog::Scripts &, std::span<const Effect>, Step, double hz,
                               const amber::Origin &);
std::vector<Handle> source_consumers(const ValueRead &, FactKey, const WatchDeclaration &);
WatchResult check_changed(Subscriptions &, const ValueRead &, const AllocationRows &, FactKey, Step,
                          double hz, const amber::Origin &, ScriptId recipient = 0);

std::vector<InvocationPlan> plan_deliveries(const WatchResult &, const Triggers &,
                                            const catalog::Scripts &, const ValueRead &, Step,
                                            double hz);
bool has_applicable_watch(const Subscriptions &, const Triggers &, const catalog::Scripts &,
                          const ValueRead &, FactKey, Step, double hz);
std::vector<react::BackgroundItem> catch_up(const SharedColumns &, IdentityRead,
                                            const catalog::Scripts &, const Terms &,
                                            const RuleVersion &, const amber::Origin &);
bool catch_up_valid(const react::BackgroundItem &, const RuleVersions &, const ValueRead &, Step,
                    double hz);
void cancel_catch_up(std::deque<BackgroundItem> &, const RuleVersion &);

void write_instances(snap::Output &, const InstanceScripts &, IdentityRead, ObjectSelection = {});
void read_instances(snap::Input &, InstanceScripts &, std::size_t slots);

// Direct reactive installation and cleanup. No lifecycle-owned references are retained.
void install_reactions(const amber::catalog::State &catalog_state, amber::react::State &react_state,
                       const ValueRead &, std::span<const Handle>, Step, double hz,
                       const amber::Origin &, DeferredEffects &);
void restore_reactions(const amber::catalog::State &catalog_state, amber::react::State &react_state,
                       const ValueRead &, std::span<const Handle>, Step, double hz,
                       const amber::Origin &, DeferredEffects &);
void initialize_reactions(const amber::catalog::State &catalog_state,
                          amber::react::State &react_state, const ValueRead &values,
                          std::span<const Handle> initial, Step now, double hz, bool restored,
                          DeferredEffects &effects);

void rebind_reactions(const amber::catalog::State &catalog_state, amber::react::State &react_state,
                      const ValueRead &, std::span<const Handle>, Step, double hz,
                      const amber::Origin &, DeferredEffects &);
void retire_reactions(const catalog::Models &, const ValueRead &, amber::react::State &react_state,
                      std::span<const Handle>, Step, double hz, const amber::Origin &,
                      DeferredEffects &);

// True means cached rule state changed and the caller must persist its registry pin.
bool update_rule(catalog::Scripts &, amber::react::State &react_state, std::deque<BackgroundItem> &,
                 const ValueRead &, ScriptId, RuleMode, Step now, double hz, const amber::Origin &,
                 DeferredEffects &);
void reconcile_allocations(const ValueRead &, amber::react::State &react_state, DeferredEffects &,
                           Step, double hz);
void reconcile_priority(const ValueRead &, amber::react::State &react_state, SettingKey,
                        DeferredEffects &, Step, double hz);
void reconcile_object_priorities(const ValueRead &, amber::react::State &react_state, Handle,
                                 DeferredEffects &, Step, double hz);

using SnapshotSelection = snap::ObjectSelection;
void write_properties(snap::Output &, const Properties &, const Processes &,
                      const SnapshotSelection & = {});
void read_properties(snap::Input &, Properties &, Processes &, std::size_t slots);
void write_relations(snap::Output &, const AuthoredRelations &, const SnapshotSelection & = {});
void read_relations(snap::Input &, AuthoredRelations &, std::size_t slots);
void write_shared(snap::Output &, const SharedColumns &, const GivesTo &, const AllocationRows &,
                  const MeterRows &, const SnapshotSelection & = {});
void read_shared(snap::Input &, SharedColumns &, GivesTo &, AllocationRows &, MeterRows &,
                 std::size_t slots);
void write_script_states(snap::Output &, const ScriptStates &, const Subscriptions &,
                         const SnapshotSelection & = {});
void read_script_states(snap::Input &, ScriptStates &, Subscriptions &);
void write_random(snap::Output &, const RandomStates &, const SnapshotSelection & = {});
void read_random(snap::Input &, RandomStates &);
void write_settings(snap::Output &, const InstanceSettings &, const ModelSettings &,
                    const PlotSettings &, const WorldSettings &, const SnapshotSelection & = {},
                    std::optional<std::span<const ModelId>> models = {},
                    std::optional<std::span<const PlotId>> plots = {});
void read_settings(snap::Input &, InstanceSettings &, ModelSettings &, PlotSettings &,
                   WorldSettings &, std::size_t slots);

Resolution resolve(const NativeActions &, const react::Terms &, const catalog::Models &,
                   const catalog::Scripts &, const InstalledScripts &, IdentityRead,
                   const ActionRequest &, Handle authenticated_body);
bool collides(const NativeActions &, const react::ScriptDeclaration &);

std::vector<ArgumentError> validate_arguments(const react::Terms &, const react::Term &,
                                              const EventPayload &);
std::string format_unit(const std::array<std::int8_t, 8> &dimensions);
std::string action_signature(const react::Terms &, const NativeActions &, TermId action);

struct State {
  explicit State(VmSettings settings) : vm(settings) {}
  Terms terms;
  Materials materials;
  Properties properties;
  Processes processes;
  InstanceSettings instance_settings;
  ModelSettings model_settings;
  PlotSettings plot_settings;
  WorldSettings world_settings;
  SharedColumns shared;
  GivesTo gives_to;
  AllocationRows allocations;
  MeterRows meters;
  AuthoredRelations authored_relations;
  RelationsRead relations;
  RuntimeMeasurements runtime_measurements;
  ScriptStates states;
  RandomStates random;
  Triggers triggers;
  RuleVersions rules;
  Subscriptions subscriptions;
  InstanceScripts installations;
  NativeActions native_declarations;
  std::vector<TermId> inspection_terms;
  Vm vm;
};
} // namespace amber::react
