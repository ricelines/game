#include "snapshot.hpp"
#include "record/snapshot.hpp"
#include <algorithm>

namespace amber::step {
template <class S> void serialize(S &s, EventWork &v) {
  s.object(v.receiver);
  s.object(v.sender);
  s.value4b(v.event);
  snap::field(s, v.payload);
  s.object(v.origin);
  s.value4b(v.recipient_script);
}
template <class S> void serialize(S &s, WatchWork &v) {
  s.object(v.source);
  s.object(v.origin);
}
template <class S> void serialize(S &s, ActionWork &v) {
  s.object(v.request);
  s.object(v.origin);
}
template <class S> void serialize(S &s, InputWork &v) {
  s.value8b(v.id);
}
template <class S> void serialize(S &s, BackgroundItem &v) {
  s.object(v.receiver);
  s.value4b(v.trigger);
  s.value4b(v.script);
  s.object(v.origin);
  s.value4b(v.version);
}
template <class S> void serialize(S &s, ThresholdToken &v) {
  s.object(v.source);
  s.object(v.consumer);
  s.value4b(v.script);
  s.value4b(v.watch_index);
  s.value8b(v.process_revision);
  s.value8b(v.binding_revision);
}
template <class S> void serialize(S &s, Timer &v) {
  s.value8b(v.due);
  s.object(v.event);
  snap::field(s, v.threshold);
}
template <class S> void serialize(S &s, ForegroundItem &v) {
  snap::field(s, v.work);
  snap::field(s, v.threshold);
}
template <class S> void serialize(S &s, Intent &v) {
  s.value8b(v.id);
  snap::field(s, v.goal);
  s.object(v.origin);
  snap::field(s, v.speed_m_s);
}
void write_timers(snap::Output &out, const TimerWheel &wheel,
                  const snap::ObjectSelection &selected) {
  out.value8b(wheel.now);
  auto rows = timers(wheel);
  if (!selected.all)
    std::erase_if(rows, [&](const Timer &timer) {
      return !selected.contains(timer.event.receiver) ||
             (timer.threshold && !selected.contains(timer.threshold->consumer));
    });
  out.container(rows, snap::max_collection());
}
void read_timers(snap::Input &in, TimerWheel &wheel) {
  wheel = TimerWheel{};
  in.value8b(wheel.now);
  std::vector<Timer> saved;
  snap::field(in, saved);
  for (auto &timer : saved)
    schedule(wheel, std::move(timer));
}
void write_queues(snap::Output &out, const WorkQueues &q, const Intents &intents,
                  std::uint64_t next_record, const snap::ObjectSelection &selected,
                  std::span<const std::uint64_t> selected_inputs) {
  out.value8b(next_record);
  auto write_lane = [&](const auto &lane) {
    if (selected.all) {
      out.container(lane, snap::max_collection());
      return;
    }
    std::deque<ForegroundItem> kept;
    for (const auto &item : lane) {
      const bool include = std::visit(
          [&](const auto &work) {
            using T = std::decay_t<decltype(work)>;
            if constexpr (std::is_same_v<T, EventWork>)
              return selected.contains(work.receiver);
            else if constexpr (std::is_same_v<T, WatchWork>)
              return selected.contains(work.source.subject);
            else if constexpr (std::is_same_v<T, ActionWork>)
              return selected.contains(work.request.target);
            else
              return std::find(selected_inputs.begin(), selected_inputs.end(), work.id) !=
                     selected_inputs.end();
          },
          item.work);
      if (include && (!item.threshold || selected.contains(item.threshold->consumer)))
        kept.push_back(item);
    }
    out.container(kept, snap::max_collection());
  };
  write_lane(q.causal);
  write_lane(q.turns);
  auto background = selected.all ? std::deque<BackgroundItem>{} : q.background;
  if (!selected.all)
    std::erase_if(background, [&](const auto &item) { return !selected.contains(item.receiver); });
  out.container(selected.all ? q.background : background, snap::max_collection());
  out.value8b(intents.next_id);
  std::vector<std::pair<std::uint64_t, Intent>> moving(intents.moving.begin(),
                                                       intents.moving.end());
  if (!selected.all)
    std::erase_if(moving, [&](const auto &item) { return !selected.contains(Handle{item.first}); });
  out.container(moving, snap::max_collection(), [](snap::Output &s, auto &entry) {
    s.value8b(entry.first);
    s.object(entry.second);
  });
}
std::uint64_t read_queues(snap::Input &in, WorkQueues &q, Intents &intents) {
  std::uint64_t next_record = 0;
  in.value8b(next_record);
  snap::field(in, q.causal);
  snap::field(in, q.turns);
  snap::field(in, q.background);
  in.value8b(intents.next_id);
  std::vector<std::pair<std::uint64_t, Intent>> moving;
  in.container(moving, snap::max_collection(), [](snap::Input &s, auto &entry) {
    s.value8b(entry.first);
    s.object(entry.second);
  });
  intents.moving.clear();
  for (auto &entry : moving)
    intents.moving.emplace(entry.first, std::move(entry.second));
  rebuild_queue_index(q);
  return next_record;
}
} // namespace amber::step
