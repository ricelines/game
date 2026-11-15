#pragma once

#include "bindings/execution/execution.hpp"
#include "command/command.hpp"
#include "install/install.hpp"
#include "perm/perm.hpp"

namespace amber::drive {
using Monotonic = std::chrono::steady_clock;
struct Settings {
  double step_hz = 0, margin_ms = 0;
  std::size_t average_steps = 0;
};
struct Clock {
  Step step = 0;
  Monotonic::time_point next_start;
  std::uint64_t overruns = 0;
};
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
struct Frame {
  Step step = 0;
  Monotonic::time_point started, handler_deadline, boundary;
  std::size_t foreground = 0, background = 0;
};
Frame begin_frame(Clock &, const Settings &, const PhaseCosts &, step::WorkQueues &,
                  step::TimerWheel &, std::vector<step::Work> arrived, Monotonic::time_point now);
bool admits(const Frame &, Monotonic::time_point now);
Monotonic::time_point finish_frame(Clock &, PhaseCosts &, const Frame &, double other_phase_ms,
                                   Monotonic::time_point now);
struct EventTurn {
  step::EventWork event;
  std::optional<step::ThresholdToken> threshold;
};
template <class T> struct Consumer {
  void *context = nullptr;
  void (*run)(void *, T) = nullptr;
};
using EventConsumer = Consumer<EventTurn>;
using WatchConsumer = Consumer<step::WatchWork>;
using InputConsumer = Consumer<step::InputWork>;
using BackgroundConsumer = Consumer<step::BackgroundItem>;
void drain_foreground(step::WorkQueues &, Frame &, EventConsumer, WatchConsumer, InputConsumer);
void drain_background(step::WorkQueues &, Frame &, BackgroundConsumer, EventConsumer, WatchConsumer,
                      InputConsumer);

struct Input {
  std::uint64_t id = 0;
  std::string operation;
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
struct InputOperation {
  InputReply (*run)(void *, const Input &) = nullptr;
  void *context = nullptr;
};
struct InputOperations {
  std::unordered_map<std::string, InputOperation> rows;
};
struct InputCompletion {
  std::uint64_t id = 0, session = 0, sequence = 0, request = 0;
  nlohmann::json fields;
};
step::InputWork enqueue_input(Inputs &, Input);
void register_input(InputOperations &, std::string name, InputOperation);
std::optional<InputCompletion> invoke_input(Inputs &, const InputOperations &, step::InputWork);
InputCompletion complete_input(Inputs &, std::uint64_t, nlohmann::json reply);
void write_inputs(snap::Output &, const Inputs &,
                  std::optional<std::span<const std::uint64_t>> selected_ids = {});
void read_inputs(snap::Input &, Inputs &);

struct Gate {
  bool allowed = false;
  std::string reason;
};
std::string_view required_right(std::string_view right, Caller, const perm::Target &);
Gate check_right(std::string_view right, Caller, const perm::Target &, const perm::Players &,
                 const perm::Grants &, Step);
struct Bounds {
  Vec3 minimum, maximum;
};
bool in_reach(Vec3 actor_point, Bounds target, float reach_m);
struct PlannedInvocation {
  react::HandlerRef handler;
  bindings::InvocationInput input;
};
bool valid_event(const EventTurn &, IdentityRead, const react::Processes &,
                 const react::Subscriptions &);
EventPayload normalize_arguments(const react::Terms &, TermId action, EventPayload);
std::vector<PlannedInvocation> plan_event(const react::Triggers &, const react::ValueRead &,
                                          const step::EventWork &, Step, double hz);
std::vector<PlannedInvocation> plan_action(const react::Triggers &, const react::ValueRead &,
                                           const command::Resolution &, const ActionRequest &,
                                           Handle authenticated_body, const record::Origin &, Step,
                                           double hz);
struct WatchPlan {
  bool modeled = false;
  std::optional<FactKey> shared_gap;
  std::vector<PlannedInvocation> invocations;
  install::WatchChanges changes;
};
WatchPlan plan_watch(react::Subscriptions &, install::WatchLinks &, const react::ValueRead &,
                     const react::AllocationRows &, const react::Triggers &,
                     const catalog::Scripts &, const step::WatchWork &, Step, double hz);
WatchPlan plan_background(react::Subscriptions &, install::WatchLinks &, const react::ValueRead &,
                          const react::AllocationRows &, const react::Triggers &,
                          const catalog::Scripts &, const react::RuleVersions &,
                          const step::BackgroundItem &, Step, double hz);

struct InputOrder {
  std::uint64_t input = 0, session = 0, sequence = 0, request = 0;
};
struct NativeCall {
  ActionRequest request;
  nlohmann::json arguments;
  Handle body;
  Step step = 0;
  double hz = 0;
  record::Origin origin;
  std::optional<InputOrder> order{};
};
struct NativeCompletion {
  record::Result result = record::Result::NoEffect;
  bool changed = false;
  nlohmann::json reply = nlohmann::json::object();
  react::DeferredEffects effects;
  std::vector<record::Event> records;
  bool deferred = false;
};
struct NativeOperation {
  std::string (*validate)(const nlohmann::json &) = nullptr;
  NativeCompletion (*run)(void *, const NativeCall &) = nullptr;
  void *context = nullptr;
};
struct NativeOperations {
  std::unordered_map<TermId, NativeOperation> rows;
};
void register_native(NativeOperations &, TermId, NativeOperation);
NativeCompletion invoke_native(const NativeOperations &, NativeCall);
} // namespace amber::drive
