#include "step.hpp"
#include <stdexcept>
namespace amber::step {
namespace {
std::string_view status(react::RunOutcome::Status value) {
  switch (value) {
  case react::RunOutcome::Status::Ok:
    return "ok";
  case react::RunOutcome::Status::Error:
    return "error";
  case react::RunOutcome::Status::Cutoff:
    return "cutoff";
  case react::RunOutcome::Status::Missing:
    return "missing";
  case react::RunOutcome::Status::Action:
    return "action";
  }
  std::terminate();
}
} // namespace
void publish_value_changes(net::ReplicationIndex &replication, net::SessionViews &views,
                           const react::Processes &processes, std::span<const FactKey> facts) {
  std::vector<Handle> changed;
  changed.reserve(facts.size());
  for (const auto fact : facts) {
    net::value_changed(replication, fact.subject, fact.term, processes);
    changed.push_back(fact.subject);
  }
  net::invalidate(views, changed);
}
void record_send_overflow(record::Journal &journal, const net::SendOverflow &notice, Step now) {
  record::Event event;
  event.step = now;
  event.kind = record::Kind::System;
  event.name = "overflow";
  event.detail = {{"queue", "send"},
                  {"session", notice.session},
                  {"buffered_bytes", notice.buffered_bytes},
                  {"action", notice.closed ? "closed" : "stale"}};
  journal.append(std::move(event));
}
void publish_effects(react::DeferredEffects &&effects, const amber::Origin &origin, Step now,
                     step::WorkQueues &queues, step::TimerWheel &wheel, record::Journal &journal,
                     std::size_t event_limit) {
  auto overflow = [&]() {
    record::Event event;
    event.step = now;
    event.kind = record::Kind::System;
    event.name = "overflow";
    journal.append(std::move(event), origin);
  };
  for (auto &item : effects.background) {
    item.origin = origin;
    step::enqueue_background(queues, std::move(item));
  }
  for (auto &effect : effects.ordered)
    std::visit(
        [&](auto &item) {
          using T = std::decay_t<decltype(item)>;
          if constexpr (std::is_same_v<T, react::EventWork>) {
            item.origin = origin;
            if (step::enqueue_event(queues, std::move(item), event_limit) ==
                step::PushResult::Overflow)
              overflow();
          } else if constexpr (std::is_same_v<T, react::WatchWork>) {
            item.origin = origin;
            step::enqueue_watch(queues, std::move(item));
          } else if constexpr (std::is_same_v<T, react::Timer>) {
            item.event.origin = origin;
            step::schedule(wheel, std::move(item));
          } else if constexpr (std::is_same_v<T, react::Diagnostic>) {
            record::Event event;
            event.step = now;
            event.kind = record::Kind::System;
            event.name = item.name;
            event.detail["message"] = item.message;
            journal.append(std::move(event), origin);
          } else if constexpr (std::is_same_v<T, record::Event>) {
            item.step = now;
            journal.append(std::move(item), origin);
          } else if constexpr (std::is_same_v<T, react::ContributionChange>) {
            throw std::logic_error("reconcile contribution changes before publishing effects");
          }
        },
        effect);
}
amber::Origin publish_invocation(record::Journal &journal, step::WorkQueues &queues,
                                 step::TimerWheel &timers, react::CompletedInvocation &&invocation,
                                 const amber::Origin &parent, std::size_t event_limit) {
  if (!invocation.origin.cause)
    return parent;
  auto origin = invocation.origin;
  auto things = nlohmann::json::array();
  for (auto object : invocation.things)
    things.push_back(to_string(object));
  auto detail = nlohmann::json{{"status", status(invocation.outcome.status)},
                               {"memory_bytes", invocation.outcome.memory_bytes},
                               {"things", std::move(things)}};
  if (!invocation.outcome.error.empty())
    detail["error"] = invocation.outcome.error;
  invocation.object_credits.insert(invocation.object_credits.end(),
                                   invocation.script_credits.begin(),
                                   invocation.script_credits.end());
  origin = journal.finish(
      origin, invocation.effects.changed ? record::Result::Applied : record::Result::NoEffect,
      invocation.effects.changed, std::move(detail), invocation.object_credits);
  publish_effects(std::move(invocation.effects), origin, invocation.step, queues, timers, journal,
                  event_limit);
  return origin;
}
} // namespace amber::step
