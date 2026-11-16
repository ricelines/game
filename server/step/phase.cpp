#include "step.hpp"

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace amber::step {
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
std::optional<std::variant<ForegroundItem, react::BackgroundItem, elaborate::FixtureResult,
                           step::authoring::Completion>>
next_work(WorkQueues &queues, Frame &frame, std::deque<elaborate::FixtureResult> &fixtures,
          std::deque<step::authoring::Completion> &authored, Monotonic::time_point now) {
  if (!frame.foreground || admits(frame, now)) {
    if (auto item = pop_foreground(queues)) {
      ++frame.foreground;
      return std::move(*item);
    }
    if (!fixtures.empty()) {
      auto item = std::move(fixtures.front());
      fixtures.pop_front();
      ++frame.foreground;
      return item;
    }
    if (!authored.empty()) {
      auto item = std::move(authored.front());
      authored.pop_front();
      ++frame.foreground;
      return item;
    }
  }
  if (!frame.background || admits(frame, now)) {
    if (auto item = pop_background(queues)) {
      ++frame.background;
      return std::move(*item);
    }
  }
  return {};
}
void write_state_report(const std::filesystem::path &path, const react::ValueRead &values,
                        std::span<const TermId> inspection_terms, TransformRead poses,
                        const amber::step::Clock &clock, std::size_t script_errors,
                        const TimerWheel &pending_timers,
                        const react::AllocationRows &allocations) {
  const auto now = clock.step;
  const auto hz = clock.hz;

  using Json = nlohmann::json;
  Json rows = Json::array();
  for (std::uint32_t slot = 1; slot < values.identity.model.size(); ++slot) {
    const auto h = values.identity.at(slot);
    if (!values.identity.valid(h))
      continue;
    Json quantities = Json::object();
    for (const auto term : inspection_terms) {
      const auto answer = react::read(values, h, term, now, hz);
      if (answer.present && answer.value.is_numeric())
        quantities[values.terms.rows[term].name] = answer.value.numeric();
    }
    rows.push_back(
        {{"handle", to_string(h)},
         {"model", values.identity.model[slot]},
         {"position", {poses.position[slot].x, poses.position[slot].y, poses.position[slot].z}},
         {"values", std::move(quantities)}});
  }
  std::ofstream output(path);
  output << Json{{"step", now},
                 {"objects", std::move(rows)},
                 {"script_errors", script_errors},
                 {"timers", step::timers(pending_timers).size()},
                 {"allocations", allocations.values.size()}}
                .dump(2)
         << '\n';
  if (!output)
    throw std::runtime_error("cannot write state report");
}
void record_frame(record::Journal &journal, record::Writer &writer, net::RecordFeed &feed,
                  IdentityRead ids, TransformRead poses, std::size_t &script_errors) {
  auto records = journal.take();
  for (const auto &event : records)
    if (event.value("status", std::string{}) == "error" ||
        event.value("status", std::string{}) == "cutoff")
      ++script_errors;
  net::append_records(feed, records, ids, poses);
  writer.submit(std::move(records));
}
} // namespace amber::step
