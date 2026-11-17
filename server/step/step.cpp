#include "step.hpp"

#include <algorithm>

namespace amber::step {
namespace {
ThresholdKey key(const react::ThresholdToken &token) {
  return {token.source, token.consumer, token.script, token.watch_index};
}
void put_timer(TimerWheel &wheel, react::Timer timer) {
  const Step delta = timer.due - wheel.now;
  const unsigned level = delta < (Step{1} << 8)    ? 0
                         : delta < (Step{1} << 16) ? 1
                         : delta < (Step{1} << 24) ? 2
                                                   : 3;
  const auto index = static_cast<unsigned>((timer.due >> (8 * level)) & 255);
  auto &bucket = wheel.levels[level][index];
  if (timer.threshold)
    wheel.thresholds.emplace(key(*timer.threshold), TimerLocation{level, index, bucket.size()});
  bucket.push_back(std::move(timer));
}
void remove_timer(TimerWheel &wheel, TimerLocation location) {
  auto &bucket = wheel.levels[location.level][location.bucket];
  if (bucket[location.index].threshold)
    wheel.thresholds.erase(key(*bucket[location.index].threshold));
  if (location.index != bucket.size() - 1) {
    bucket[location.index] = std::move(bucket.back());
    if (bucket[location.index].threshold)
      wheel.thresholds.at(key(*bucket[location.index].threshold)) = location;
  }
  bucket.pop_back();
}
std::vector<react::Timer> take_bucket(TimerWheel &wheel, unsigned level, unsigned index) {
  auto rows = std::move(wheel.levels[level][index]);
  wheel.levels[level][index].clear();
  for (const auto &timer : rows)
    if (timer.threshold)
      wheel.thresholds.erase(key(*timer.threshold));
  return rows;
}
void cascade(TimerWheel &wheel, unsigned level) {
  const auto index = static_cast<unsigned>((wheel.now >> (8 * level)) & 255);
  for (auto &timer : take_bucket(wheel, level, index))
    put_timer(wheel, std::move(timer));
}
} // namespace
void append_inputs(WorkQueues &queues, std::vector<Work> inputs) {
  for (auto &input : inputs)
    queues.turns.push_back({std::move(input), {}});
}
void schedule(TimerWheel &wheel, react::Timer timer) {
  if (timer.threshold) {
    const auto found = wheel.thresholds.find(key(*timer.threshold));
    if (found != wheel.thresholds.end()) {
      const auto location = found->second;
      remove_timer(wheel, location);
    }
  }
  timer.due = std::max(timer.due, wheel.now + 1);
  put_timer(wheel, std::move(timer));
}
void discard_script_work(WorkQueues &queues, TimerWheel &wheel, std::span<const Handle> objects) {
  if (objects.empty())
    return;
  std::vector<Handle> selected(objects.begin(), objects.end());
  std::sort(selected.begin(), selected.end());
  const auto includes = [&](Handle object) {
    return std::binary_search(selected.begin(), selected.end(), object);
  };
  const auto event = [&](const ForegroundItem &item) {
    const auto *work = std::get_if<react::EventWork>(&item.work);
    return work && includes(work->receiver);
  };
  queues.emitted_events -= std::erase_if(queues.causal, event);
  std::erase_if(queues.turns, event);
  std::erase_if(queues.background,
                [&](const react::BackgroundItem &item) { return includes(item.receiver); });
  for (unsigned level = 0; level < wheel.levels.size(); ++level)
    for (unsigned bucket = 0; bucket < wheel.levels[level].size(); ++bucket) {
      auto &rows = wheel.levels[level][bucket];
      for (std::size_t index = rows.size(); index-- > 0;)
        if (includes(rows[index].event.receiver))
          remove_timer(wheel, {level, bucket, index});
    }
}
void cancel_threshold(TimerWheel &wheel, const ThresholdKey &key) {
  if (const auto found = wheel.thresholds.find(key); found != wheel.thresholds.end())
    remove_timer(wheel, found->second);
}
void cancel_source_thresholds(TimerWheel &wheel, const react::Subscriptions &subscriptions,
                              FactKey source) {
  if (const auto found = subscriptions.rows.find(source); found != subscriptions.rows.end())
    for (const auto &binding : found->second)
      cancel_threshold(wheel,
                       {source, binding.consumer.object, binding.consumer.script, binding.index});
}
void promote_due(TimerWheel &wheel, Step now, WorkQueues &queues) {
  while (wheel.now < now) {
    ++wheel.now;
    if ((wheel.now & 0xffffff) == 0)
      cascade(wheel, 3);
    if ((wheel.now & 0xffff) == 0)
      cascade(wheel, 2);
    if ((wheel.now & 0xff) == 0)
      cascade(wheel, 1);
    for (auto &timer : take_bucket(wheel, 0, static_cast<unsigned>(wheel.now & 255))) {
      if (timer.due <= wheel.now)
        queues.turns.push_back({std::move(timer.event), std::move(timer.threshold)});
      else
        put_timer(wheel, std::move(timer));
    }
  }
}
std::optional<ForegroundItem> pop_foreground(WorkQueues &queues) {
  auto &source = queues.causal.empty() ? queues.turns : queues.causal;
  if (source.empty())
    return {};
  auto work = std::move(source.front());
  source.pop_front();
  if (const auto *watch = std::get_if<react::WatchWork>(&work.work))
    queues.watch_queued.erase(watch->source);
  else if (&source == &queues.causal)
    --queues.emitted_events;
  return work;
}
std::optional<react::BackgroundItem> pop_background(WorkQueues &queues) {
  if (queues.background.empty())
    return {};
  auto item = std::move(queues.background.front());
  queues.background.pop_front();
  return item;
}
PushResult enqueue_event(WorkQueues &q, react::EventWork w, std::size_t limit) {
  if (q.emitted_events >= limit)
    return PushResult::Overflow;
  q.causal.push_back({std::move(w), {}});
  ++q.emitted_events;
  return PushResult::Queued;
}
PushResult enqueue_watch(WorkQueues &q, react::WatchWork w) {
  if (!q.watch_queued.insert(w.source).second)
    return PushResult::Coalesced;
  q.causal.push_back({std::move(w), {}});
  return PushResult::Queued;
}
void enqueue_background(WorkQueues &q, react::BackgroundItem w) {
  q.background.push_back(std::move(w));
}
void rebuild_queue_index(WorkQueues &q) {
  q.watch_queued.clear();
  q.emitted_events = 0;
  for (const auto &item : q.causal) {
    if (const auto *watch = std::get_if<react::WatchWork>(&item.work))
      q.watch_queued.insert(watch->source);
    else
      ++q.emitted_events;
  }
  for (const auto &item : q.turns)
    if (const auto *watch = std::get_if<react::WatchWork>(&item.work))
      q.watch_queued.insert(watch->source);
}
std::vector<react::Timer> timers(const TimerWheel &wheel) {
  std::vector<react::Timer> result;
  for (const auto &level : wheel.levels)
    for (const auto &bucket : level)
      result.insert(result.end(), bucket.begin(), bucket.end());
  std::stable_sort(result.begin(), result.end(),
                   [](const react::Timer &a, const react::Timer &b) { return a.due < b.due; });
  return result;
}
IntentStart start_move(Intents &intents, Handle body, MoveGoal goal, amber::Origin origin,
                       std::optional<float> speed_m_s) {
  auto replaced = finish_move(intents, body, IntentEnd::Replaced);
  const auto id = intents.next_id++;
  intents.moving.emplace(body.bits, Intent{id, std::move(goal), std::move(origin), speed_m_s});
  return {id, std::move(replaced)};
}
std::optional<IntentEnding> finish_move(Intents &intents, Handle body, IntentEnd reason) {
  auto it = intents.moving.find(body.bits);
  if (it == intents.moving.end())
    return {};
  IntentEnding result{body, std::move(it->second), reason};
  intents.moving.erase(it);
  return result;
}
std::optional<IntentEnding> cancel_move(Intents &intents, Handle body) {
  return finish_move(intents, body, IntentEnd::Cancelled);
}
} // namespace amber::step
