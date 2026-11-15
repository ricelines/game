#pragma once
#include "snap/snap.hpp"
#include "step.hpp"

namespace amber::step {
void write_timers(snap::Output &, const TimerWheel &, const snap::ObjectSelection & = {});
void read_timers(snap::Input &, TimerWheel &);
void write_queues(snap::Output &, const WorkQueues &, const Intents &, std::uint64_t next_record,
                  const snap::ObjectSelection & = {},
                  std::span<const std::uint64_t> selected_inputs = {});
std::uint64_t read_queues(snap::Input &, WorkQueues &, Intents &);
} // namespace amber::step
