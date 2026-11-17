#include "snap/snap.hpp"
#include "step.hpp"
#include <algorithm>

namespace amber::react {
template <class S> void serialize(S &s, react::EventWork &v) {
  s.object(v.receiver);
  s.object(v.sender);
  s.value4b(v.event);
  snap::field(s, v.payload);
  s.object(v.origin);
  s.value4b(v.recipient_script);
}
template <class S> void serialize(S &s, react::WatchWork &v) {
  s.object(v.source);
  s.object(v.origin);
}
template <class S> void serialize(S &s, react::BackgroundItem &v) {
  s.object(v.receiver);
  s.value4b(v.trigger);
  s.value4b(v.script);
  s.object(v.origin);
  s.value4b(v.version);
}
template <class S> void serialize(S &s, react::Timer &v) {
  s.value8b(v.due);
  s.object(v.event);
  snap::field(s, v.threshold);
}
} // namespace amber::react

namespace amber::step {

template <class S> void serialize(S &s, ForegroundItem &v) {
  std::uint8_t kind = std::holds_alternative<react::EventWork>(v.work) ? 0 : 1;
  s.value1b(kind);
  switch (kind) {
  case 0:
    if constexpr (std::is_same_v<S, snap::Input>)
      v.work.template emplace<react::EventWork>();
    s.object(std::get<react::EventWork>(v.work));
    break;
  case 1:
    if constexpr (std::is_same_v<S, snap::Input>)
      v.work.template emplace<react::WatchWork>();
    s.object(std::get<react::WatchWork>(v.work));
    break;
  default:
    throw std::runtime_error("invalid snapshot foreground work kind");
  }
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
    std::erase_if(rows, [&](const react::Timer &timer) {
      return !selected.contains(timer.event.receiver) ||
             (timer.threshold && !selected.contains(timer.threshold->subject));
    });
  out.container(rows, snap::max_collection());
}
void read_timers(snap::Input &in, TimerWheel &wheel) {
  wheel = TimerWheel{};
  in.value8b(wheel.now);
  std::vector<react::Timer> saved;
  snap::field(in, saved);
  for (auto &timer : saved)
    schedule(wheel, std::move(timer));
}
void write_queues(snap::Output &out, const WorkQueues &q, const Intents &intents,
                  std::uint64_t next_record, const snap::ObjectSelection &selected) {
  out.value8b(next_record);
  auto write_lane = [&](const auto &lane) {
    std::vector<const ForegroundItem *> kept;
    kept.reserve(lane.size());
    for (const auto &item : lane) {
      const bool include = std::visit(
          [&](const auto &work) {
            using T = std::decay_t<decltype(work)>;
            if constexpr (std::is_same_v<T, react::EventWork>)
              return selected.contains(work.receiver);
            else if constexpr (std::is_same_v<T, react::WatchWork>)
              return selected.contains(work.source.subject);
            else
              return false;
          },
          item.work);
      if (include && (!item.threshold || selected.contains(item.threshold->subject)))
        kept.push_back(&item);
    }
    out.container(kept, snap::max_collection(),
                  [](snap::Output &s, const ForegroundItem *item) { s.object(*item); });
  };
  write_lane(q.causal);
  write_lane(q.turns);
  auto background = selected.all ? std::deque<react::BackgroundItem>{} : q.background;
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
