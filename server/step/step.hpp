#pragma once

#include "record/record.hpp"

#include <array>
#include <deque>
#include <unordered_map>
#include <unordered_set>

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
  std::uint64_t process_revision = 0, binding_revision = 0;
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
void append_inputs(WorkQueues &, std::vector<Work>);
void promote_due(TimerWheel &, Step now, WorkQueues &);
std::optional<ForegroundItem> pop_foreground(WorkQueues &);
std::optional<BackgroundItem> pop_background(WorkQueues &);
PushResult enqueue_event(WorkQueues &, EventWork, std::size_t limit);
PushResult enqueue_watch(WorkQueues &, WatchWork);
void enqueue_background(WorkQueues &, BackgroundItem);
void schedule(TimerWheel &, Timer);
void rebuild_queue_index(WorkQueues &);
std::vector<Timer> timers(const TimerWheel &);

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
IntentStart start_move(Intents &, Handle, MoveGoal, record::Origin,
                       std::optional<float> speed_m_s = {});
std::optional<IntentEnding> cancel_move(Intents &, Handle);
std::optional<IntentEnding> finish_move(Intents &, Handle, IntentEnd);

} // namespace amber::step
