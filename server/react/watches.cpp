#include "react.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace amber::react {
std::vector<HandlerRef> dispatch(const Triggers &triggers, IdentityRead identity,
                                 std::span<const TermId> flags, Handle receiver, TermId trigger) {
  std::vector<HandlerRef> out;
  if (!identity.valid(receiver))
    return out;
  auto it = triggers.find(trigger);
  if (it == triggers.end())
    return out;
  const auto &row = it->second;
  auto add = [&](const HandlerRef &handler) {
    if (handler.enabled && std::none_of(out.begin(), out.end(), [&](const auto &existing) {
          return existing.script == handler.script;
        }))
      out.push_back(handler);
  };
  if (auto found = row.by_material.find(identity.material[receiver.slot()]);
      found != row.by_material.end())
    add(found->second);
  for (const auto &[flag, handler] : row.by_flag)
    if (std::find(flags.begin(), flags.end(), flag) != flags.end())
      add(handler);
  if (auto found = row.by_model.find(identity.model[receiver.slot()]); found != row.by_model.end())
    add(found->second);
  if (auto found = row.by_instance.find(receiver.bits); found != row.by_instance.end())
    for (const auto &handler : found->second)
      add(handler);
  return out;
}
std::vector<HandlerRef> dispatch(const Triggers &triggers, const ValueRead &values, Handle receiver,
                                 TermId trigger, Step now, double hz) {
  std::vector<TermId> flags;
  if (auto row = triggers.find(trigger); row != triggers.end())
    for (const auto &[term, handler] : row->second.by_flag) {
      (void)handler;
      const auto value = read(values, receiver, term, now, hz);
      if (value.present && (value.value.kind == Value::Kind::Boolean
                                ? value.value.boolean
                                : value.value.is_numeric() && value.value.numeric() != 0))
        flags.push_back(term);
    }
  return dispatch(triggers, values.identity, flags, receiver, trigger);
}
bool Threshold::test(double current) const {
  switch (predicate) {
  case Predicate::Above:
    return current > value;
  case Predicate::AtLeast:
    return current >= value;
  case Predicate::Below:
    return current < value;
  case Predicate::AtMost:
    return current <= value;
  }
  return false;
}
std::optional<Step> next_crossing(const Process &process, const Threshold &threshold, Step now,
                                  double hz) {
  const double current = process_value(process, now, hz);
  const double end = process.rate > 0 ? process.hi : process.lo;
  const bool before = threshold.test(current);
  if (process.rate == 0 || before == threshold.test(end))
    return {};
  const double crossing =
      static_cast<double>(process.step0) + (threshold.value - process.v0) * hz / process.rate;
  if (!std::isfinite(crossing) ||
      crossing >= static_cast<double>(std::numeric_limits<Step>::max() - 2))
    return {};
  // Predicate strictness differs by direction; adjust against the actual sampled value.
  Step due = std::max(now + 1, static_cast<Step>(std::max(0.0, std::floor(crossing))));
  while (due > now + 1 && threshold.test(process_value(process, due - 1, hz)) != before)
    --due;
  while (threshold.test(process_value(process, due, hz)) == before)
    ++due;
  return due;
}
RelationAnswer read_relation(const Relation &relation, Handle object) {
  RelationAnswer answer;
  if (const auto *out = relation.out.get(object.slot()))
    answer.out = *out;
  if (const auto *in = relation.in.get(object.slot()))
    answer.in = *in;
  return answer;
}
std::vector<WatchDelivery> check_watch(Subscriptions &subscriptions, FactKey source,
                                       const CurrentAnswers &answers, ScriptId recipient) {
  std::vector<WatchDelivery> out;
  auto found = subscriptions.rows.find(source);
  if (found == subscriptions.rows.end())
    return out;
  auto current = answers.values.find(source);
  const WatchAnswer common =
      current == answers.values.end() ? WatchAnswer{Answer{}} : current->second;
  for (auto &binding : found->second) {
    if (recipient && binding.consumer.script != recipient)
      continue;
    const auto own = answers.by_consumer.find(binding.consumer);
    const WatchAnswer &answer = own == answers.by_consumer.end() ? common : own->second;
    bool changed = binding.last != answer;
    if (changed)
      binding.revision = subscriptions.next_revision++;
    auto scalar = [](const WatchAnswer &value) -> const Answer * {
      if (const auto *shared = std::get_if<SharedWatchAnswer>(&value))
        return &shared->aggregate;
      return std::get_if<Answer>(&value);
    };
    const auto *before = scalar(binding.last);
    const auto *after = scalar(answer);
    const auto *old_share = std::get_if<SharedWatchAnswer>(&binding.last);
    const auto *new_share = std::get_if<SharedWatchAnswer>(&answer);
    const bool share_changed =
        old_share && new_share && old_share->own_allocation != new_share->own_allocation;
    if (!binding.thresholds.empty() && before && after && before->present && after->present &&
        before->value.is_numeric() && after->value.is_numeric()) {
      changed = share_changed || std::any_of(binding.thresholds.begin(), binding.thresholds.end(),
                                             [&](const Threshold &t) {
                                               return t.test(before->value.numeric()) !=
                                                      t.test(after->value.numeric());
                                             });
    }
    std::vector<RelationDelta> deltas;
    if (changed)
      if (const auto *relation = std::get_if<RelationAnswer>(&answer)) {
        const auto *old = std::get_if<RelationAnswer>(&binding.last);
        auto difference = [&](const std::vector<Handle> &previous, const std::vector<Handle> &next,
                              bool incoming) {
          for (auto handle : previous)
            if (std::find(next.begin(), next.end(), handle) == next.end())
              deltas.push_back({handle, false, incoming});
          for (auto handle : next)
            if (std::find(previous.begin(), previous.end(), handle) == previous.end())
              deltas.push_back({handle, true, incoming});
        };
        const std::vector<Handle> empty;
        difference(old ? old->out : empty, relation->out, false);
        difference(old ? old->in : empty, relation->in, true);
      }
    binding.last = answer;
    if (changed)
      out.push_back({binding.consumer, binding.index, answer, std::move(deltas)});
  }
  return out;
}
bool valid_threshold(const step::ThresholdToken &token, IdentityRead identity,
                     const Processes &processes, const Subscriptions &subscriptions) {
  if (!identity.valid(token.source.subject) || !identity.valid(token.consumer) ||
      token.source.term >= processes.size())
    return false;
  const auto *process = processes[token.source.term].get(token.source.subject.slot());
  if (!process || process->revision != token.process_revision)
    return false;
  auto found = subscriptions.rows.find(token.source);
  if (found == subscriptions.rows.end())
    return false;
  return std::any_of(found->second.begin(), found->second.end(), [&](const auto &binding) {
    return binding.consumer.object == token.consumer && binding.consumer.script == token.script &&
           binding.index == token.watch_index && binding.revision == token.binding_revision;
  });
}
namespace {
template <class T> void unique_push(std::vector<T> &values, const T &value) {
  if (std::find(values.begin(), values.end(), value) == values.end())
    values.push_back(value);
}
void index_binding(Subscriptions &subscriptions, FactKey source, ScriptKey consumer) {
  unique_push(subscriptions.by_subject[source.subject.bits], source);
  unique_push(subscriptions.by_consumer[consumer], source);
  unique_push(subscriptions.consumers_on[consumer.object.bits], consumer);
}
void unindex_consumer(Subscriptions &subscriptions, FactKey source, ScriptKey consumer) {
  auto found = subscriptions.by_consumer.find(consumer);
  if (found == subscriptions.by_consumer.end())
    return;
  std::erase(found->second, source);
  if (!found->second.empty())
    return;
  subscriptions.by_consumer.erase(found);
  auto object = subscriptions.consumers_on.find(consumer.object.bits);
  std::erase(object->second, consumer);
  if (object->second.empty())
    subscriptions.consumers_on.erase(object);
}
void unindex_source(Subscriptions &subscriptions, FactKey source) {
  auto found = subscriptions.by_subject.find(source.subject.bits);
  std::erase(found->second, source);
  if (found->second.empty())
    subscriptions.by_subject.erase(found);
}
} // namespace
void bind_watch(Subscriptions &subscriptions, FactKey source, WatchBinding binding) {
  auto &rows = subscriptions.rows[source];
  auto found = std::find_if(rows.begin(), rows.end(), [&](const auto &row) {
    return row.consumer == binding.consumer && row.index == binding.index;
  });
  binding.revision = std::max(subscriptions.next_revision, binding.revision);
  subscriptions.next_revision = binding.revision + 1;
  index_binding(subscriptions, source, binding.consumer);
  if (found == rows.end())
    rows.push_back(std::move(binding));
  else
    *found = std::move(binding);
}
void unbind_watch(Subscriptions &subscriptions, ScriptKey consumer, std::uint32_t index) {
  auto found = subscriptions.by_consumer.find(consumer);
  if (found == subscriptions.by_consumer.end())
    return;
  const auto sources = found->second;
  for (auto source : sources) {
    auto row = subscriptions.rows.find(source);
    std::erase_if(row->second, [&](const auto &binding) {
      return binding.consumer == consumer && binding.index == index;
    });
    if (std::none_of(row->second.begin(), row->second.end(),
                     [&](const auto &binding) { return binding.consumer == consumer; }))
      unindex_consumer(subscriptions, source, consumer);
    if (row->second.empty()) {
      subscriptions.rows.erase(row);
      unindex_source(subscriptions, source);
    }
  }
}
void retire_subscriptions(Subscriptions &subscriptions, Handle object) {
  if (auto found = subscriptions.by_subject.find(object.bits);
      found != subscriptions.by_subject.end()) {
    const auto sources = found->second;
    for (auto source : sources) {
      auto row = subscriptions.rows.find(source);
      for (const auto &binding : row->second)
        unindex_consumer(subscriptions, source, binding.consumer);
      subscriptions.rows.erase(row);
    }
    subscriptions.by_subject.erase(object.bits);
  }
  if (auto found = subscriptions.consumers_on.find(object.bits);
      found != subscriptions.consumers_on.end()) {
    const auto consumers = found->second;
    for (auto consumer : consumers) {
      const auto sources = subscriptions.by_consumer.at(consumer);
      for (auto source : sources) {
        auto row = subscriptions.rows.find(source);
        std::erase_if(row->second,
                      [&](const auto &binding) { return binding.consumer == consumer; });
        if (row->second.empty()) {
          subscriptions.rows.erase(row);
          unindex_source(subscriptions, source);
        }
      }
      subscriptions.by_consumer.erase(consumer);
    }
    subscriptions.consumers_on.erase(object.bits);
  }
}
void rebuild_subscription_index(Subscriptions &subscriptions) {
  subscriptions.by_subject.clear();
  subscriptions.by_consumer.clear();
  subscriptions.consumers_on.clear();
  for (const auto &[source, rows] : subscriptions.rows)
    for (const auto &binding : rows) {
      index_binding(subscriptions, source, binding.consumer);
      subscriptions.next_revision = std::max(subscriptions.next_revision, binding.revision + 1);
    }
}
std::vector<step::Timer> threshold_timers(const Subscriptions &subscriptions,
                                          const Processes &processes, FactKey source, Step now,
                                          double hz, const record::Origin &origin) {
  std::vector<step::Timer> timers;
  if (source.term >= processes.size())
    return timers;
  const auto *process = processes[source.term].get(source.subject.slot());
  if (!process)
    return timers;
  auto found = subscriptions.rows.find(source);
  if (found == subscriptions.rows.end())
    return timers;
  for (const auto &binding : found->second) {
    std::optional<Step> first;
    for (const auto &threshold : binding.thresholds) {
      auto due = next_crossing(*process, threshold, now, hz);
      if (due && (!first || *due < *first))
        first = due;
    }
    if (first)
      timers.push_back(
          {*first,
           {binding.consumer.object, source.subject, source.term, Value{}, origin},
           step::ThresholdToken{source, binding.consumer.object, binding.consumer.script,
                                binding.index, process->revision, binding.revision}});
  }
  return timers;
}
void publish_effects(DeferredEffects &&effects, const record::Origin &origin, Step now,
                     step::WorkQueues &queues, step::TimerWheel &wheel, record::Journal &journal,
                     std::size_t event_limit) {
  if (!effects.structural.empty())
    throw std::logic_error("apply structural outputs before publishing effects");
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
          if constexpr (std::is_same_v<T, step::EventWork>) {
            item.origin = origin;
            if (step::enqueue_event(queues, std::move(item), event_limit) ==
                step::PushResult::Overflow)
              overflow();
          } else if constexpr (std::is_same_v<T, step::ActionWork>) {
            item.origin = origin;
            if (step::enqueue_action(queues, std::move(item), event_limit) ==
                step::PushResult::Overflow)
              overflow();
          } else if constexpr (std::is_same_v<T, step::WatchWork>) {
            item.origin = origin;
            step::enqueue_watch(queues, std::move(item));
          } else if constexpr (std::is_same_v<T, step::Timer>) {
            item.event.origin = origin;
            step::schedule(wheel, std::move(item));
          } else if constexpr (std::is_same_v<T, Diagnostic>) {
            record::Event event;
            event.step = now;
            event.kind = record::Kind::System;
            event.name = item.name;
            event.detail["message"] = item.message;
            journal.append(std::move(event), origin);
          } else if constexpr (std::is_same_v<T, record::Event>) {
            item.step = now;
            journal.append(std::move(item), origin);
          } else if constexpr (std::is_same_v<T, ContributionChange>) {
            throw std::logic_error("reconcile contribution changes before publishing effects");
          } else if constexpr (std::is_same_v<T, CausalReceipt>) {
            item.finalize(item.owner, item.receipt, origin);
          }
        },
        effect);
}
} // namespace amber::react
