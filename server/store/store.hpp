#pragma once

#include <array>
#include <cassert>
#include <chrono>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace amber {

using Step = std::uint64_t;
using PlayerId = std::uint32_t;
using ModelId = std::uint32_t;
using TermId = std::uint32_t;
using ScriptId = std::uint32_t;
using GroupId = std::uint32_t;
using PlotId = std::uint32_t;
using GrantId = std::uint32_t;

struct Handle {
  std::uint64_t bits = 0;

  static constexpr Handle make(std::uint32_t slot, std::uint32_t generation) {
    return {std::uint64_t{generation} << 32 | slot};
  }
  constexpr std::uint32_t slot() const { return static_cast<std::uint32_t>(bits); }
  constexpr std::uint32_t generation() const { return static_cast<std::uint32_t>(bits >> 32); }
  constexpr explicit operator bool() const { return bits != 0; }
  auto operator<=>(const Handle &) const = default;
};

std::string to_string(Handle handle);
std::optional<Handle> parse_handle(std::string_view text);

struct Vec3 {
  float x = 0;
  float y = 0;
  float z = 0;

  auto operator<=>(const Vec3 &) const = default;
  Vec3 operator+(Vec3 other) const { return {x + other.x, y + other.y, z + other.z}; }
  Vec3 operator-(Vec3 other) const { return {x - other.x, y - other.y, z - other.z}; }
  Vec3 operator*(float scale) const { return {x * scale, y * scale, z * scale}; }
  float length_squared() const { return x * x + y * y + z * z; }
  float length() const { return std::sqrt(length_squared()); }
};

struct Quat {
  float x = 0;
  float y = 0;
  float z = 0;
  float w = 1;
  auto operator<=>(const Quat &) const = default;
};
// Rigid transforms use unit quaternions, normalized once at input boundaries.
Quat compose(Quat, Quat);
Vec3 rotate(Quat, Vec3);

inline constexpr std::uint32_t no_position = std::numeric_limits<std::uint32_t>::max();

// Object-keyed columns have one dense iteration order and one slot lookup.
template <class T> struct SparseColumn {
  std::vector<std::uint32_t> slot_of;
  std::vector<T> value;
  std::vector<std::uint32_t> pos_of;

  const T *get(std::uint32_t slot) const {
    if (slot >= pos_of.size() || pos_of[slot] == no_position)
      return nullptr;
    return &value[pos_of[slot]];
  }

  T *get(std::uint32_t slot) { return const_cast<T *>(std::as_const(*this).get(slot)); }

  template <class U> T &set(std::uint32_t slot, U &&entry) {
    if (slot >= pos_of.size())
      pos_of.resize(static_cast<std::size_t>(slot) + 1, no_position);
    auto &position = pos_of[slot];
    if (position == no_position) {
      position = static_cast<std::uint32_t>(value.size());
      slot_of.push_back(slot);
      value.emplace_back(std::forward<U>(entry));
    } else {
      value[position] = std::forward<U>(entry);
    }
    return value[position];
  }

  bool erase(std::uint32_t slot) {
    if (slot >= pos_of.size() || pos_of[slot] == no_position)
      return false;
    const auto position = pos_of[slot];
    if (position + 1 != value.size()) {
      value[position] = std::move(value.back());
      slot_of[position] = slot_of.back();
      pos_of[slot_of[position]] = position;
    }
    value.pop_back();
    slot_of.pop_back();
    pos_of[slot] = no_position;
    return true;
  }

  void clear() {
    slot_of.clear();
    value.clear();
    pos_of.clear();
  }

  // Snapshot payloads contain dense pairs; reconstruct the derived lookup.
  void rebuild_positions(std::size_t slots) {
    pos_of.assign(slots, no_position);
    for (std::uint32_t position = 0; position < slot_of.size(); ++position) {
      assert(slot_of[position] < slots && pos_of[slot_of[position]] == no_position);
      pos_of[slot_of[position]] = position;
    }
  }
};

// Canonical SI scalar values. Text and transient event records live separately.
struct Value {
  enum class Kind : std::uint8_t { None, Number, Integer, Boolean, Object };
  Kind kind = Kind::None;
  union {
    double number;
    std::int64_t integer;
    bool boolean;
    Handle object;
  };
  constexpr Value() : integer(0) {}
  constexpr Value(double value) : kind(Kind::Number), number(value) {}
  constexpr Value(std::int64_t value) : kind(Kind::Integer), integer(value) {}
  constexpr Value(bool value) : kind(Kind::Boolean), boolean(value) {}
  constexpr Value(Handle value) : kind(Kind::Object), object(value) {}
  constexpr bool present() const { return kind != Kind::None; }
  constexpr bool is_numeric() const {
    return kind == Kind::Number || kind == Kind::Integer || kind == Kind::Boolean;
  }
  double numeric() const {
    switch (kind) {
    case Kind::Number:
      return number;
    case Kind::Integer:
      return static_cast<double>(integer);
    case Kind::Boolean:
      return boolean ? 1.0 : 0.0;
    default:
      assert(false && "value is not numeric");
      return 0.0;
    }
  }
  bool operator==(const Value &other) const {
    if (kind != other.kind)
      return false;
    switch (kind) {
    case Kind::None:
      return true;
    case Kind::Number:
      return number == other.number;
    case Kind::Integer:
      return integer == other.integer;
    case Kind::Boolean:
      return boolean == other.boolean;
    case Kind::Object:
      return object == other.object;
    }
    return false;
  }
};
static_assert(sizeof(Value) == 16);

struct TermRef {
  TermId id = 0;
  bool operator==(const TermRef &) const = default;
};
using EventAtom = std::variant<Value, Vec3, std::string, TermRef>;
struct EventField {
  std::string name;
  EventAtom value;
  bool operator==(const EventField &) const = default;
};
using EventPayload = std::variant<Value, Vec3, std::string, std::vector<EventField>>;

struct FactKey {
  Handle subject;
  TermId term = 0;
  bool operator==(const FactKey &) const = default;
};
struct FactKeyHash {
  std::size_t operator()(const FactKey &key) const {
    return std::hash<std::uint64_t>{}(key.subject.bits ^
                                      (std::uint64_t{key.term} * 0x9e3779b97f4a7c15ULL));
  }
};
struct ScriptKey {
  Handle object;
  ScriptId script = 0;
  bool operator==(const ScriptKey &) const = default;
};
struct ScriptKeyHash {
  std::size_t operator()(const ScriptKey &key) const {
    return FactKeyHash{}({key.object, key.script});
  }
};
struct ScriptHolder {
  ScriptId script = 0;
  Handle object;
  bool operator==(const ScriptHolder &) const = default;
};
using Caller = std::variant<PlayerId, ScriptHolder>;
struct ActionRequest {
  Caller caller = PlayerId{0};
  TermId action = 0;
  Handle target;
  EventPayload args;
};

struct Relation {
  SparseColumn<std::vector<Handle>> out, in;
};

enum ObjectFlag : std::uint32_t {
  Static = 1,
  Dynamic = 2,
  Body = 4,
  ScriptedMotion = 8,
  Held = 16,
  Proxy = 32,
  NoCopy = 64,
  Escrow = 128,
};

struct ObjectInit {
  Vec3 position;
  Quat rotation;
  ModelId model = 0;
  TermId material = 0;
  PlayerId owner = 0;
  PlayerId maker = 0;
  Step created_step = 0;
  GroupId group = 0;
  std::uint32_t preset = 0;
  PlotId plot = 0;
  Handle parent;
  std::uint32_t flags = Static;
  std::uint32_t params = 0;
};

// Only lifecycle operations and the object snapshot section accept this table.
struct Objects {
  std::vector<std::uint32_t> generation{0};
  std::vector<Vec3> position{{}};
  std::vector<Quat> rotation{{}};
  std::vector<ModelId> model{0};
  std::vector<TermId> material{0};
  std::vector<PlayerId> owner{0};
  std::vector<PlayerId> maker{0};
  std::vector<Step> created_step{0};
  std::vector<GroupId> group{0};
  std::vector<std::uint32_t> preset{0};
  std::vector<PlotId> plot{0};
  std::vector<Handle> parent{{}};
  std::vector<std::uint32_t> flags{0};
  std::vector<std::uint32_t> params{0};
  std::vector<std::uint32_t> free_slots;
};

struct IdentityRead {
  const std::vector<std::uint32_t> &generation;
  const std::vector<ModelId> &model;
  const std::vector<TermId> &material;
  const std::vector<std::uint32_t> &flags;

  bool valid(Handle handle) const {
    return handle.slot() < generation.size() && model[handle.slot()] != 0 &&
           generation[handle.slot()] == handle.generation();
  }
  Handle at(std::uint32_t slot) const { return Handle::make(slot, generation[slot]); }
};

struct TransformRead {
  const std::vector<Vec3> &position;
  const std::vector<Quat> &rotation;
};

struct TransformWrite {
  std::vector<Vec3> &position;
  std::vector<Quat> &rotation;
  operator TransformRead() const { return {position, rotation}; }
};

struct AuthorityRead {
  const std::vector<std::uint32_t> &generation;
  const std::vector<PlayerId> &owner;
  const std::vector<GroupId> &group;
  const std::vector<std::uint32_t> &preset;
  const std::vector<PlotId> &plot;
};

struct AuthorityWrite {
  const std::vector<std::uint32_t> &generation;
  std::vector<PlayerId> &owner;
  std::vector<GroupId> &group;
  std::vector<std::uint32_t> &preset;
  const std::vector<PlotId> &plot;
  operator AuthorityRead() const { return {generation, owner, group, preset, plot}; }
};

struct PlotWrite {
  std::vector<PlotId> &plot;
};

struct AssetBindingWrite {
  std::vector<ModelId> &model;
  std::vector<TermId> &material;
  std::vector<std::uint32_t> &params;
};

// Runtime writers cannot change another module's flag bits or create a body.
template <std::uint32_t Mask> class FlagWrite {
public:
  explicit FlagWrite(std::vector<std::uint32_t> &flags) : flags_(flags) {}
  void set(std::uint32_t slot, std::uint32_t bits) const {
    assert((bits & ~Mask) == 0);
    flags_[slot] = (flags_[slot] & ~Mask) | (bits & Mask);
  }
  std::uint32_t get(std::uint32_t slot) const { return flags_[slot] & Mask; }

private:
  std::vector<std::uint32_t> &flags_;
};

using SpaceFlags = FlagWrite<Static | Dynamic | ScriptedMotion | Held>;
using PermissionFlags = FlagWrite<NoCopy | Escrow>;

struct AttributionRead {
  const std::vector<PlayerId> &maker;
  const std::vector<Step> &created_step;
};

struct ParentRead {
  const std::vector<Handle> &parent;
};

struct ParentWrite {
  std::vector<Handle> &parent;
  operator ParentRead() const { return {parent}; }
};

IdentityRead identity(const Objects &objects);
TransformRead transforms(const Objects &objects);
TransformWrite transforms(Objects &objects);
AuthorityRead authority(const Objects &objects);
AuthorityWrite authority(Objects &objects);
AttributionRead attribution(const Objects &objects);
ParentRead parents(const Objects &objects);
ParentWrite parents(Objects &objects);
PlotWrite plot_membership(Objects &objects);
AssetBindingWrite asset_bindings(Objects &objects);
SpaceFlags space_flags(Objects &objects);
PermissionFlags permission_flags(Objects &objects);

Handle create(Objects &objects, const ObjectInit &init);
void destroy(Objects &objects, Handle handle);

} // namespace amber

// Event provenance and work rows are stored by value.
namespace amber::record {
struct Origin {
  std::uint64_t cause = 0;
  std::uint64_t root = 0;
  std::vector<PlayerId> authors;
  std::optional<double> oldest;
  bool more = false;
  bool operator==(const Origin &) const = default;
};
struct Credit {
  PlayerId author = 0;
  std::optional<double> created_s;
  bool seed = false;
};
enum class Kind {
  Action,
  Handler,
  Gap,
  Denial,
  Speech,
  Observation,
  Create,
  Destroy,
  Registry,
  System
};
enum class Result { None, Applied, NoEffect, NoHandler, NotModeled, Denied };
struct Actor {
  enum class Kind { System, Player, Script } kind = Kind::System;
  std::uint32_t id = 0;
  static Actor player(PlayerId id) { return {Kind::Player, id}; }
  static Actor script(ScriptId id) { return {Kind::Script, id}; }
};
struct Event {
  Step step = 0;
  Kind kind = Kind::System;
  Actor actor;
  std::string name;
  Result result = Result::None;
  bool changed = false;
  std::vector<Handle> things;
  std::optional<PlayerId> script_author;
  nlohmann::json detail = nlohmann::json::object();
  std::vector<Credit> credits{};
};
} // namespace amber::record

namespace amber::step {
struct EventWork {
  Handle receiver, sender;
  TermId event = 0;
  EventPayload payload;
  record::Origin origin;
  ScriptId recipient_script = 0; // internal targeted delivery; zero broadcasts normally
};
struct WatchWork {
  FactKey source;
  record::Origin origin;
};
struct InputWork {
  std::uint64_t id = 0;
};
using Work = std::variant<EventWork, WatchWork, InputWork>;
struct BackgroundItem {
  Handle receiver;
  TermId trigger = 0;
  ScriptId script = 0;
  record::Origin origin;
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
  EventWork event;
  std::optional<ThresholdToken> threshold;
};
struct ForegroundItem {
  Work work;
  std::optional<ThresholdToken> threshold;
};
struct WorkQueues {
  std::deque<ForegroundItem> causal, turns;
  std::deque<BackgroundItem> background;
  std::unordered_set<FactKey, FactKeyHash> watch_queued;
  std::size_t emitted_events = 0;
};
struct ThresholdKey {
  FactKey source;
  Handle consumer;
  ScriptId script = 0;
  std::uint32_t watch_index = 0;
  bool operator==(const ThresholdKey &) const = default;
};
struct ThresholdKeyHash {
  std::size_t operator()(const ThresholdKey &key) const {
    return FactKeyHash{}(key.source) ^ (ScriptKeyHash{}({key.consumer, key.script}) << 1) ^
           (static_cast<std::size_t>(key.watch_index) << 2);
  }
};
struct TimerLocation {
  unsigned level = 0, bucket = 0;
  std::size_t index = 0;
};
struct TimerWheel {
  Step now = 0;
  std::array<std::array<std::vector<Timer>, 256>, 4> levels;
  // Derived from pending threshold rows, never serialized. Ordinary timers
  // retain independent entries, including multiple events on the same target.
  std::unordered_map<ThresholdKey, TimerLocation, ThresholdKeyHash> thresholds;
};
enum class PushResult { Queued, Coalesced, Overflow };
using IntentId = std::uint64_t;
using MoveGoal = std::variant<Vec3, Handle>;
enum class IntentEnd { Arrived, Blocked, Cancelled, Replaced };
struct Intent {
  IntentId id = 0;
  MoveGoal goal;
  record::Origin origin;
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
} // namespace amber::step

// Ordered passive reactive tables/DTOs. Requires base store + record + step passive declarations.
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

using Effect = std::variant<step::EventWork, step::WatchWork, step::Timer, Diagnostic,
                            record::Event, ContributionChange>;

struct DeferredEffects {
  std::vector<Effect> ordered;
  bool changed = false;
  std::vector<Handle> consulted_handles{};
  std::vector<step::BackgroundItem> background{};
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
  record::Origin origin;
  ScriptState *state = nullptr;
  std::uint64_t *random = nullptr;
  DeferredEffects *effects = nullptr;
  std::chrono::steady_clock::time_point deadline;
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
  record::Origin origin;
  std::vector<Handle> acquired;
  std::optional<Caller> action_caller;
  Handle action_body, action_target;
  std::chrono::steady_clock::time_point deadline{};
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
  record::Origin origin{};
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
  std::vector<step::WatchWork> dirty;
  std::vector<step::Timer> timers;
};

struct WatchResult {
  FactKey source;
  std::vector<WatchDelivery> deliveries;
  std::vector<step::Timer> timers;
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

} // namespace amber::react

namespace amber::catalog {
using Hash = std::string;
struct RuntimePins {
  std::string bake, luau = "0.747", host_abi = "ambergris-host-1", jolt = "5.6.0";
};
struct Text {
  std::string name, description, writing;
  bool operator==(const Text &) const = default;
};
struct ModelTexts {
  std::unordered_map<ModelId, Text> rows;
};
struct InstanceTexts {
  std::unordered_map<std::uint64_t, Text> rows;
};
using TextId = std::uint32_t;
enum class AuthoredTextKind { Detail, Refusal };
struct AuthoredText {
  TextId id = 0;
  Handle target;
  PlayerId author = 0;
  double created_s = 0;
  AuthoredTextKind kind = AuthoredTextKind::Detail;
  std::string text;
};
struct AuthoredTexts {
  std::vector<AuthoredText> rows{AuthoredText{}};
};
TextId append_authored_text(AuthoredTexts &, Handle, PlayerId, double created_s, AuthoredTextKind,
                            std::string);
struct Asset {
  Hash hash, source_hash;
  std::string canonical_params, part_name;
  Hash parent_asset;
  Vec3 minimum, maximum;
  double volume = 0, capacity = 0;
  std::vector<TermId> replicated;
  nlohmann::json manifest;
  std::string source;
};
struct Assets {
  std::unordered_map<Hash, Asset> rows;
};
struct Model {
  ModelId id = 0;
  Hash asset, source_hash;
  std::string part_name, canonical_params;
  TermId material = 0;
  PlayerId author = 0;
  double created_s = 0;
  ScriptId script = 0;
  std::map<std::string, ModelId> parts;
  GroupId group = 0;
};
struct Models {
  std::vector<Model> rows{Model{}};
};
struct ParameterBlock {
  std::uint32_t id = 0;
  ModelId model = 0;
  Hash asset;
  std::string canonical_params;
  std::map<std::string, ModelId> parts;
};
struct ParameterBlocks {
  std::vector<ParameterBlock> rows{ParameterBlock{}};
};
struct RegistryDocument {
  Hash hash;
  nlohmann::json data;
};
struct MechanicalBindings {
  nlohmann::json values = nlohmann::json::object();
};
struct MeasurementBindings {
  TermId volume = 0, capacity = 0;
};
struct ModelRegistration {
  ModelId root = 0;
  std::map<std::string, ModelId> parts;
};
struct EffectiveAsset {
  Hash hash;
  std::uint32_t params = 0;
};
struct DuplicateChoice {
  TermId same_as = 0;
  std::string none_fits;
};
Hash sha256(std::string_view);
bool valid_hash(std::string_view);
Asset read_asset(const std::filesystem::path &root, Hash expected, std::string canonical_params,
                 const RuntimePins &);
void import_asset(Assets &, Asset);
const std::map<std::string, ModelId> &part_models(const Models &, const ParameterBlocks &, ModelId,
                                                  std::uint32_t params);
EffectiveAsset resolve_asset(const Models &, const ParameterBlocks &, ModelId, std::uint32_t);
struct RevisedParameter {
  std::uint32_t before = 0, after = 0;
};
struct ModelUpdate {
  ModelId model = 0;
  bool changed = false;
  std::vector<ModelId> models;
  std::vector<RevisedParameter> parameters;
  std::vector<std::pair<ModelId, ModelId>> inherited_settings;
};
// Resolve every registered parameter set against the proposal's baked sets.
// The default adopts the new defaults; explicit values survive on retained keys.
std::vector<RevisedParameter> definition_parameters(const Models &, const ParameterBlocks &,
                                                    const Assets &, ModelId model,
                                                    ModelId proposal);
// A rare definition edit on the single writer. Parameter block identities stay
// usable and receive the new geometry; no old definition can be reconfigured in.
ModelUpdate update_definition(Models &, ParameterBlocks &, ModelTexts &, const Assets &,
                              ModelId model, ModelId proposal);

} // namespace amber::catalog
namespace amber::lifecycle {
struct NamedPart {
  std::string name;
  Handle object;
  bool operator==(const NamedPart &) const = default;
};
using Parts = SparseColumn<std::vector<NamedPart>>;
struct PartTemplate {
  std::string name;
  ModelId model = 0;
  TermId material = 0;
  Vec3 position;
  Quat rotation;
};
struct AssemblyTemplate {
  ModelId model = 0;
  std::uint32_t params = 0;
  TermId material = 0;
  std::vector<PartTemplate> parts;
};
AssemblyTemplate expand(const catalog::Models &, const catalog::ParameterBlocks &,
                        const catalog::Assets &, ModelId, std::uint32_t params = 0);
struct Created {
  Handle root;
  std::vector<Handle> objects;
};
Created instantiate(Objects &, Parts &, Relation &declared_parts, const AssemblyTemplate &,
                    ObjectInit root);
std::vector<Handle> collect(const Parts &, IdentityRead, std::span<const Handle> roots);
std::vector<Handle> collect(const Parts &, IdentityRead, Handle root);
// Step coordinates owner cleanup before row release.
// The input is the already collected full closure, not an independent root list.
void release_rows(Objects &, Parts &, Relation &declared_parts, std::span<const Handle>);
struct Binding {
  ModelId model = 0;
  TermId material = 0;
  std::uint32_t params = 0;
  bool operator==(const Binding &) const = default;
};
struct Rebinding {
  Handle object;
  Binding before, after;
  Vec3 position;
  Quat rotation;
};
struct Replacement {
  Handle root;
  Binding original;
  std::vector<NamedPart> original_parts;
  std::vector<Rebinding> retained;
  std::vector<PartTemplate> added;
  std::vector<Handle> removed;
};
// Binding/name/material decisions only; retained poses are filled by plan_replace.
Replacement plan_bindings(const catalog::Models &, const Parts &, IdentityRead,
                          const std::vector<std::uint32_t> &params, Handle,
                          const AssemblyTemplate &, std::optional<TermId> material = {});
Replacement plan_replace(const catalog::Models &, const Parts &, IdentityRead, TransformRead,
                         const std::vector<std::uint32_t> &params, Handle, const AssemblyTemplate &,
                         std::optional<TermId> material = {});
// Step applies returned row changes and retirement before resuming the caller.
Created apply_replacement(Objects &, Parts &, Relation &declared_parts, const Replacement &,
                          PlayerId edit_maker, Step now);
void rebuild_declared_parts(const Parts &, IdentityRead, Relation &);

} // namespace amber::lifecycle

namespace amber {
struct ObjectState {
  Objects objects;
  lifecycle::Parts parts;
  Relation declared_parts;
};
} // namespace amber

namespace amber::catalog {
struct PreparedScript {
  Hash hash, source_hash, bytecode_hash, registry_pin;
  RuntimePins pins;
  std::string source;
  react::CompiledScript compiled;
  react::ScriptDeclaration declaration;
  nlohmann::json metadata;
};
struct Script {
  ScriptId id = 0;
  Hash artifact, source_hash, bytecode_hash, registry_pin;
  RuntimePins pins;
  PlayerId author = 0;
  double created_s = 0;
  react::ScriptDeclaration declaration;
  nlohmann::json tests = nlohmann::json::array();
  std::string source;
};
struct Scripts {
  std::vector<Script> rows{Script{}};
};
struct ModelSubmission {
  Hash asset;
  TermId material = 0;
  PlayerId author = 0;
  double created_s = 0;
  ScriptId script = 0;
  Text text;
  react::Values settings;
};
struct TermProposal {
  react::Term term;
};

} // namespace amber::catalog

namespace amber::snap {
struct ObjectSelection {
  bool all = true;
  std::unordered_set<std::uint64_t> objects;
  std::unordered_set<std::uint32_t> slots;
  ObjectSelection() = default;
  explicit ObjectSelection(std::span<const Handle> selected) : all(false) {
    for (auto object : selected) {
      objects.insert(object.bits);
      slots.insert(object.slot());
    }
  }
  bool contains(Handle object) const { return all || objects.contains(object.bits); }
};
} // namespace amber::snap
namespace amber::perm {
enum class Verb : std::uint8_t { Use, Take, Move, Edit, Copy, Transfer, Destroy };
using Rights = std::uint32_t;
constexpr Rights bit(Verb verb) {
  return Rights{1} << static_cast<unsigned>(verb);
}
inline constexpr Rights all_rights = (Rights{1} << 7) - 1;
inline constexpr Rights inherited_rights = bit(Verb::Edit) | bit(Verb::Copy) | bit(Verb::Transfer);
enum class HolderKind : std::uint8_t { Player, Script };
struct Holder {
  HolderKind kind = HolderKind::Player;
  std::uint32_t id = 0;
  Handle installed_on;
  static Holder player(PlayerId id) { return {HolderKind::Player, id, {}}; }
  static Holder script(ScriptId id, Handle object) { return {HolderKind::Script, id, object}; }
  bool operator==(const Holder &) const = default;
};
enum class AccountKind : std::uint8_t { Player, Object, Group };
struct Account {
  AccountKind kind = AccountKind::Player;
  std::uint64_t subject = 0;
  static Account player(PlayerId id) { return {AccountKind::Player, id}; }
  static Account object(Handle object) { return {AccountKind::Object, object.bits}; }
  static Account group(GroupId id) { return {AccountKind::Group, id}; }
  auto operator<=>(const Account &) const = default;
};
struct AccountTarget {
  Account account;
  PlayerId controller = 0;
  static AccountTarget player(PlayerId id) { return {Account::player(id), id}; }
};
enum class ScopeKind : std::uint8_t { Object, OwnedOnPlot, Account, Avatar, Plot, Owned, AllLand };
struct Scope {
  ScopeKind kind = ScopeKind::Object;
  std::uint64_t subject = 0;
  PlayerId owner = 0;
  AccountKind account_kind = AccountKind::Player;
  static Scope object(Handle handle) { return {ScopeKind::Object, handle.bits, 0}; }
  static Scope owned_on_plot(PlayerId owner, PlotId plot) {
    return {ScopeKind::OwnedOnPlot, plot, owner};
  }
  static Scope account(Account account) {
    return {ScopeKind::Account, account.subject, 0, account.kind};
  }
  Account account() const { return {account_kind, subject}; }
  static Scope avatar(Handle body) { return {ScopeKind::Avatar, body.bits, 0}; }
  static Scope plot(PlotId id) { return {ScopeKind::Plot, id, 0}; }
  static Scope owned(PlayerId owner) { return {ScopeKind::Owned, 0, owner}; }
  static Scope all_land() { return {ScopeKind::AllLand, 0, 0}; }
  bool operator==(const Scope &) const = default;
};
struct GrantKey {
  Holder holder;
  Scope scope;
  bool operator==(const GrantKey &) const = default;
};
struct GrantKeyHash {
  std::size_t operator()(const GrantKey &) const;
};
struct Player {
  std::vector<GroupId> groups;
  std::vector<GrantId> grants_held, grants_given;
  std::vector<Handle> inventory, mailbox;
  std::vector<nlohmann::json> notices;
  std::uint32_t notices_dropped = 0;
};
struct Players {
  std::size_t mailbox_max = 0;
  std::vector<Player> rows{Player{}};
};
struct Group {
  GroupId id = 0;
  PlayerId controller = 0;
  std::string name;
};
struct Groups {
  std::vector<Group> rows{Group{}};
};
struct Grant {
  GrantId id = 0;
  PlayerId issuer = 0;
  GrantId parent = 0;
  Holder holder;
  Scope scope;
  Rights verbs = 0;
  std::int64_t cap = 0, spent = 0;
  Step period_steps = 0, period_start = 0, expires_step = 0;
  bool live = false;
};
struct Grants {
  std::vector<Grant> rows{Grant{}};
  std::unordered_map<GrantKey, GrantId, GrantKeyHash> by_key;
  // Each live deadline has one ordered (step,id) entry, erased on removal.
  std::set<std::pair<Step, GrantId>> expiring, reminders;
};
struct Ledger {
  std::map<Account, std::int64_t> balance;
};
using OwnerLimits = SparseColumn<Rights>;
struct Target {
  Handle object;
  PlayerId owner = 0;
  GroupId group = 0;
  Rights preset = 0;
  PlotId plot = 0;
  std::uint32_t flags = 0;
  Rights owner_limit = inherited_rights;
  bool on_lot = false;
};
enum class Reason {
  Allowed,
  UnknownHolder,
  MissingRight,
  InheritedLimit,
  NoCopy,
  Expired,
  Allowance,
  InsufficientBalance,
  BadAmount,
  BalanceOverflow,
  Duplicate,
  InvalidDelegation,
  ExpiryRequired,
  InvalidScope,
  NotController,
  Banned,
  NotGoods,
  NotPlaced,
  InEscrow,
  OfferGone,
  WrongBuyer,
  MailboxFull,
  StaleGoods,
  NotParticipant,
  NotPresent
};
enum class LandVerb : std::uint8_t { Build, Enter, RunScripts, Transfer, Return };
enum class PlotKind : std::uint8_t { Street, Alley, Lot };
struct Plot {
  PlotId id = 0;
  PlayerId owner = 0;
  GroupId group = 0;
  Rights preset = 0;
  std::array<float, 4> bounds{}; // min x, min y, max x, max y
  std::vector<PlayerId> banned;  // sorted
  PlotKind kind = PlotKind::Lot;
};
struct Plots {
  std::vector<Plot> rows{Plot{}};
};
struct Decision {
  Reason reason = Reason::MissingRight;
  GrantId grant = 0;
  explicit operator bool() const { return reason == Reason::Allowed; }
};
struct GrantRequest {
  PlayerId issuer = 0;
  Holder holder;
  Scope scope;
  Rights verbs = 0;
  std::int64_t cap = 0;
  Step period_steps = 0, expires_step = 0;
};
struct ScopeControl {
  Scope scope;
  PlayerId controller = 0;
  Rights available = 0;
  PlotId plot = 0;
  bool on_lot = false;
};
struct HolderControl {
  Holder holder;
  PlayerId controller = 0;
  bool installed = false;
};
struct GrantResult {
  Reason reason = Reason::InvalidScope;
  GrantId grant = 0;
  explicit operator bool() const { return reason == Reason::Allowed; }
};
struct GrantDeadlines {
  std::vector<GrantId> reminders, expired;
};
struct TransferResult {
  Reason reason = Reason::BadAmount;
  std::int64_t amount = 0;
  explicit operator bool() const { return reason == Reason::Allowed; }
};
struct AccountRefund {
  Handle object;
  PlayerId owner = 0;
  std::int64_t amount = 0;
};
using OfferId = std::uint32_t;
struct Offer {
  OfferId id = 0;
  PlayerId seller = 0, buyer = 0;
  Handle goods;
  std::int64_t price = 0;
  Step expires_step = 0;
  std::uint32_t trade = 0;
  bool live = false;
  std::vector<Handle> parts;
};
struct Offers {
  std::vector<Offer> rows{Offer{}};
  std::set<std::pair<Step, OfferId>> expiring;
};
struct CustodyChange {
  Handle goods, parent;
  bool in_world = false;
};
struct OfferResult {
  Reason reason = Reason::OfferGone;
  OfferId offer = 0;
  std::vector<CustodyChange> custody;
  explicit operator bool() const { return reason == Reason::Allowed; }
};
struct Buyer {
  Holder actor;
  PlayerId beneficiary = 0;
  AccountTarget payer;
  Handle custodian;
  bool present = false;
};
struct SwapResult {
  Reason reason = Reason::OfferGone;
  std::vector<CustodyChange> custody;
  std::vector<GrantId> revoked;
  explicit operator bool() const { return reason == Reason::Allowed; }
};
using TradeId = std::uint32_t;
struct TradeSide {
  OfferId offer = 0;
  std::int64_t money = 0;
  bool confirmed = false;
};
struct Trade {
  TradeId id = 0;
  std::array<PlayerId, 2> players{};
  std::array<TradeSide, 2> sides{};
  bool live = true;
};
struct Trades {
  std::vector<Trade> rows{Trade{}};
};
struct TradeResult {
  Reason reason = Reason::OfferGone;
  TradeId trade = 0;
  bool completed = false;
  std::vector<CustodyChange> custody;
  std::vector<GrantId> revoked;
  explicit operator bool() const { return reason == Reason::Allowed; }
};
struct SnapshotSelection {
  snap::ObjectSelection objects{std::span<const Handle>{}};
  std::unordered_set<PlayerId> players;
  std::unordered_set<GroupId> groups;
  std::unordered_set<PlotId> plots;
  std::unordered_set<GrantId> grants;
  std::unordered_set<OfferId> offers;
  std::unordered_set<TradeId> trades;
};
} // namespace amber::perm

namespace amber::net {
enum class ControlKind : std::uint8_t {
  Look,
  Inspect,
  Actions,
  Do,
  Cancel,
  RegisterModel,
  RegisterVariant,
  RegisterScript,
  AuthoringFailed,
  Events,
  Debug,
  View
};
}

namespace amber::react {
enum class MechanicAction { Grasp, Release, Attach, Impulse, Transfer };
enum class ConstructionAction { Create, Destroy, Reconfigure, ReviseModel, PlaceExisting };
enum class ExchangeAction {
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
} // namespace amber::react

namespace amber::step {
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
struct EventTurn {
  step::EventWork event;
  std::optional<step::ThresholdToken> threshold;
};
struct Input {
  std::uint64_t id = 0;
  net::ControlKind kind;
  Caller caller = PlayerId{0};
  Handle body;
  std::uint64_t session = 0, sequence = 0, request = 0;
  nlohmann::json fields = nlohmann::json::object();
  record::Origin origin;
  bool awaiting = false;
};
struct Inputs {
  std::uint64_t next_id = 1;
  std::map<std::uint64_t, Input> rows;
};
struct InputReply {
  bool deferred = false;
  nlohmann::json fields = nlohmann::json::object();
  std::optional<record::Origin> origin{};
};
struct InputCompletion {
  std::uint64_t id = 0, session = 0, sequence = 0, request = 0;
  nlohmann::json fields;
};
struct Bounds {
  Vec3 minimum, maximum;
};
struct PlannedInvocation {
  react::HandlerRef handler;
  react::InvocationInput input;
};
struct WatchPlan {
  bool modeled = false;
  std::optional<FactKey> shared_gap;
  std::vector<PlannedInvocation> invocations;
  react::WatchChanges changes;
};
struct InputOrder {
  std::uint64_t input = 0, session = 0, sequence = 0, request = 0;
};
struct GapDraft {
  Handle target;
  TermId term, action;
  PlayerId requester;
  record::Origin origin;
};
struct NativeCall {
  ActionRequest request;
  nlohmann::json arguments;
  Handle body;
  Step step = 0;
  double hz = 0;
  record::Origin origin;
  std::optional<InputOrder> order{};
  Monotonic::time_point deadline{};
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
  std::vector<ModelId> changed_models;
  std::vector<ScriptId> changed_scripts;
  std::vector<TermId> changed_terms, changed_materials;
  bool permissions_changed = false;
  std::vector<perm::CustodyChange> custody;
  std::vector<Handle> created_objects, removed_objects, visible_objects, hidden_objects;
  std::vector<lifecycle::Rebinding> rebound_objects;
};
} // namespace amber::step

namespace amber::step {
struct DeliveryTargets {
  std::unordered_map<PlayerId, Handle> present_bodies;
};
} // namespace amber::step

namespace amber::step {
struct ActionOutcome {
  nlohmann::json reply;
  record::Origin origin;
  bool deferred = false;
};
} // namespace amber::step
