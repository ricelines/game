#pragma once

#include "step/step.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <unordered_map>

struct lua_State;

namespace amber::react {

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
  std::uint64_t revision = 0;
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
  std::uint64_t provider_revision = 0;
  record::Origin origin;
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
  TermId density = 0;
};
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
std::vector<HandlerRef> dispatch(const Triggers &, IdentityRead, std::span<const TermId> flags,
                                 Handle receiver, TermId trigger);
std::vector<HandlerRef> dispatch(const Triggers &, const ValueRead &, Handle receiver,
                                 TermId trigger, Step, double hz);

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
RelationAnswer read_relation(const Relation &, Handle);
struct WatchBinding {
  ScriptKey consumer;
  std::uint32_t index = 0;
  std::uint64_t revision = 1;
  WatchAnswer last = Answer{};
  std::vector<Threshold> thresholds;
};
struct Subscriptions {
  std::unordered_map<FactKey, std::vector<WatchBinding>, FactKeyHash> rows;
  std::uint64_t next_revision = 1;
  // Derived incident indexes; rebuild after restoring authoritative rows.
  std::unordered_map<ScriptKey, std::vector<FactKey>, ScriptKeyHash> by_consumer;
  std::unordered_map<std::uint64_t, std::vector<FactKey>> by_subject;
  std::unordered_map<std::uint64_t, std::vector<ScriptKey>> consumers_on;
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
std::vector<WatchDelivery> check_watch(Subscriptions &, FactKey, const CurrentAnswers &,
                                       ScriptId recipient = 0);
std::optional<Step> next_crossing(const Process &, const Threshold &, Step now, double hz);
bool valid_threshold(const step::ThresholdToken &, IdentityRead, const Processes &,
                     const Subscriptions &);
void bind_watch(Subscriptions &, FactKey, WatchBinding);
void unbind_watch(Subscriptions &, ScriptKey, std::uint32_t index);
void retire_subscriptions(Subscriptions &, Handle);
void rebuild_subscription_index(Subscriptions &);
std::vector<step::Timer> threshold_timers(const Subscriptions &, const Processes &, FactKey,
                                          Step now, double hz, const record::Origin &);

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
  std::unordered_map<FactKey, std::vector<AllocationKey>, FactKeyHash> by_subject_term;
  std::unordered_map<std::uint64_t, std::vector<AllocationKey>> by_object;
};
using MeterRows = std::unordered_map<AllocationKey, Meter, AllocationKeyHash>;
struct PriorityValues {
  std::unordered_map<std::uint64_t, double> values;
};
double metered(const Meter &, Step now, double hz);
std::vector<AllocationKey> allocate(const Shared &supply, const Shared &demand, Combine,
                                    const PriorityValues &, Handle subject, TermId supply_term,
                                    TermId demand_term, AllocationRows &, MeterRows &, Step now,
                                    double hz);
double get_share(const AllocationRows &, Handle subject, TermId demand_term, Handle contributor);
double live_share(const ValueRead &, Handle subject, TermId demand_term, Handle contributor,
                  Step now, double hz);
void rebuild_allocation_index(AllocationRows &);
void retire_allocations(AllocationRows &, MeterRows &, Handle);

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
using Effect = std::variant<step::EventWork, step::WatchWork, step::Timer, Diagnostic,
                            record::Event, ContributionChange>;
struct DeferredEffects {
  std::vector<Effect> ordered;
  bool changed = false;
  std::vector<Handle> consulted_handles{};
  std::vector<step::BackgroundItem> background{};
};
bool set_state(Properties &, Processes &, const Term &, Handle, Value, Step, double hz,
               DeferredEffects &);
bool set_rate(Properties &, Processes &, const Term &, Handle, double rate, double lo, double hi,
              Step, double hz, DeferredEffects &);
bool transfer_state(Properties &, Processes &, const Term &, Handle from, Handle to, double amount,
                    Step, double hz, DeferredEffects &);
bool set_contribution(SharedColumns &, GivesTo &, const Term &, Handle from, Handle subject,
                      double amount, DeferredEffects &);
void withdraw_contributor(SharedColumns &, GivesTo &, const Terms &, Handle, DeferredEffects &);
bool set_relation(Relation &, TermId, Handle from, Handle to, bool present, DeferredEffects &);
bool apply_derived_fact(RuntimeMeasurements &, const DerivedFactInput &, DeferredEffects &);
void retire_object(Properties &, Processes &, ScriptStates &, RandomStates &, Handle,
                   std::span<const ScriptId> installed_scripts);
void publish_effects(DeferredEffects &&, const record::Origin &, Step now, step::WorkQueues &,
                     step::TimerWheel &, record::Journal &, std::size_t event_limit);

struct WatchDeclaration {
  TermId term = 0, along = 0;
  std::vector<Threshold> thresholds;
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
ScriptValidation validate_script(std::string_view, const Terms &, const ScriptDeclaration &,
                                 const RightsDeclaration &);
CompiledScript compile_script(std::string_view);
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
  record::Origin origin;
  ScriptState *state = nullptr;
  std::uint64_t *random = nullptr;
  DeferredEffects *effects = nullptr;
  std::chrono::steady_clock::time_point deadline;
  const ScriptDeclaration *declaration = nullptr;
};
void consult_object(const Invocation &, Handle);
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
class Vm {
public:
  explicit Vm(VmSettings);
  ~Vm();
  Vm(const Vm &) = delete;
  Vm &operator=(const Vm &) = delete;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  friend void register_binding(Vm &, std::string_view, int (*)(lua_State *), void *);
  friend RunOutcome load_script(Vm &, const CompiledScript &, const ScriptDeclaration &);
  friend RunOutcome run_handler(Vm &, const HandlerRef &, Invocation &);
  friend RunOutcome resume_handler(Vm &, Invocation &, const nlohmann::json &);
  friend bool set_activation(Vm &, ScriptId, bool, bool);
  friend std::size_t state_capacity(const Vm &, ScriptId);
  friend ScriptAnalysis analyze_script(std::string_view, const Terms &, const RightsDeclaration &,
                                       VmSettings);
};
void register_binding(Vm &, std::string_view, int (*)(lua_State *), void *binding_local);
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
StateKey check_state_key(lua_State *, int index);
StateAtom check_state_atom(lua_State *, int index);
std::size_t state_entry_bytes(const StateKey &, const StateAtom &);
std::size_t state_bytes(const ScriptState &);
std::size_t state_capacity(const Vm &, ScriptId);
std::size_t state_capacity(lua_State *);
void refresh_state_memory(lua_State *);
// Adapter contexts are separately owned by composition, never a shared host aggregate.
struct ReadBinding {
  const ValueRead &values;
};
struct StateBinding {
  Properties &properties;
  Processes &processes;
  const Terms &terms;
};
struct RelationBinding {
  const RelationsRead &relations;
  const Terms &terms;
};
struct ContributionBinding {
  SharedColumns &shared;
  GivesTo &gives_to;
  const Terms &terms;
  const RelationsRead &relations;
};
struct ShareBinding {
  const ValueRead &values;
};
int get_binding(lua_State *);
int read_binding(lua_State *);
int setting_binding(lua_State *);
int set_binding(lua_State *);
int rate_binding(lua_State *);
int related_binding(lua_State *);
int contribute_binding(lua_State *);
int share_binding(lua_State *);

} // namespace amber::react
