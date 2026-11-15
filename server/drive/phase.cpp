#include "drive.hpp"

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace amber::drive {
namespace {
void consume(step::ForegroundItem item, EventConsumer event, WatchConsumer watch,
             ActionConsumer action, InputConsumer input) {
  std::visit(
      [&](auto &work) {
        using T = std::decay_t<decltype(work)>;
        if constexpr (std::is_same_v<T, step::EventWork>)
          event.run(event.context, {std::move(work), std::move(item.threshold)});
        else if constexpr (std::is_same_v<T, step::WatchWork>)
          watch.run(watch.context, std::move(work));
        else if constexpr (std::is_same_v<T, step::ActionWork>)
          action.run(action.context, std::move(work));
        else
          input.run(input.context, std::move(work));
      },
      item.work);
}
void drain_causal(step::WorkQueues &queues, Frame &frame, EventConsumer event, WatchConsumer watch,
                  ActionConsumer action, InputConsumer input) {
  while (!queues.causal.empty() && admits(frame, Monotonic::now())) {
    consume(std::move(*step::pop_foreground(queues)), event, watch, action, input);
    ++frame.foreground;
  }
}
} // namespace
PhaseCosts::PhaseCosts(std::size_t window) : samples_(window) {
  if (!window)
    throw std::invalid_argument("phase average window must be positive");
}
void PhaseCosts::observe(double milliseconds) {
  total_ -= samples_[next_];
  samples_[next_] = milliseconds;
  total_ += milliseconds;
  next_ = (next_ + 1) % samples_.size();
  count_ = std::min(count_ + 1, samples_.size());
}
double PhaseCosts::mean_ms() const {
  return count_ ? total_ / static_cast<double>(count_) : 0;
}
Frame begin_frame(Clock &clock, const Settings &settings, const PhaseCosts &costs,
                  step::WorkQueues &queues, step::TimerWheel &timers,
                  std::vector<step::Work> arrived, Monotonic::time_point now) {
  ZoneScopedN("due work and inputs");
  if (settings.step_hz <= 0 || !std::isfinite(settings.step_hz) || settings.margin_ms < 0 ||
      !std::isfinite(settings.margin_ms) || !settings.average_steps)
    throw std::invalid_argument("invalid scheduler settings");
  if (clock.next_start == Monotonic::time_point{})
    clock.next_start = now;
  const double period_ms = 1000.0 / settings.step_hz;
  const auto duration = [](double milliseconds) {
    return std::chrono::duration_cast<Monotonic::duration>(
        std::chrono::duration<double, std::milli>(milliseconds));
  };
  Frame frame{++clock.step,
              now,
              now + duration(std::max(0.0, period_ms - costs.mean_ms() - settings.margin_ms)),
              clock.next_start + duration(period_ms),
              0,
              0};
  step::promote_due(timers, clock.step, queues);
  step::append_inputs(queues, std::move(arrived));
  return frame;
}
bool admits(const Frame &frame, Monotonic::time_point now) {
  return now < frame.handler_deadline;
}
Monotonic::time_point finish_frame(Clock &clock, PhaseCosts &costs, const Frame &frame,
                                   double other_phase_ms, Monotonic::time_point now) {
  costs.observe(other_phase_ms);
  if (now > frame.boundary) {
    ++clock.overruns;
    clock.next_start = now;
  } else
    clock.next_start = frame.boundary;
  return clock.next_start;
}
void drain_foreground(step::WorkQueues &queues, Frame &frame, EventConsumer event,
                      WatchConsumer watch, ActionConsumer action, InputConsumer input) {
  ZoneScopedN("foreground work");
  while (admits(frame, Monotonic::now())) {
    auto item = step::pop_foreground(queues);
    if (!item)
      return;
    consume(std::move(*item), event, watch, action, input);
    ++frame.foreground;
  }
}
void drain_background(step::WorkQueues &queues, Frame &frame, BackgroundConsumer background,
                      EventConsumer event, WatchConsumer watch, ActionConsumer action,
                      InputConsumer input) {
  ZoneScopedN("background work");
  while (frame.background == 0 || admits(frame, Monotonic::now())) {
    auto item = step::pop_background(queues);
    if (!item)
      return;
    background.run(background.context, std::move(*item));
    ++frame.background;
    drain_causal(queues, frame, event, watch, action, input);
  }
}
} // namespace amber::drive
