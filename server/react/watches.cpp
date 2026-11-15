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
    if (!binding.declaration.thresholds.empty() && before && after && before->present &&
        after->present && before->value.is_numeric() && after->value.is_numeric()) {
      changed =
          share_changed ||
          std::any_of(binding.declaration.thresholds.begin(), binding.declaration.thresholds.end(),
                      [&](const Threshold &t) {
                        return t.test(before->value.numeric()) != t.test(after->value.numeric());
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
                     const Processes &processes, const Subscriptions &subscriptions, Step now,
                     double hz) {
  if (!identity.valid(token.source.subject) || !identity.valid(token.consumer) ||
      token.source.term >= processes.size())
    return false;
  const auto *process = processes[token.source.term].get(token.source.subject.slot());
  if (!process)
    return false;
  const auto found = subscriptions.rows.find(token.source);
  if (found == subscriptions.rows.end())
    return false;
  for (const auto &binding : found->second) {
    if (binding.consumer != ScriptKey{token.consumer, token.script} ||
        binding.index != token.watch_index)
      continue;
    const auto *before = std::get_if<Answer>(&binding.last);
    if (!before || !before->present || !before->value.is_numeric())
      return false;
    const auto current = process_value(*process, now, hz);
    return std::any_of(binding.declaration.thresholds.begin(), binding.declaration.thresholds.end(),
                       [&](const Threshold &threshold) {
                         return threshold.test(before->value.numeric()) != threshold.test(current);
                       });
  }
  return false;
}
std::vector<FactKey> watch_sources(const Subscriptions &subscriptions, ScriptKey consumer) {
  std::vector<FactKey> result;
  for (const auto &[source, bindings] : subscriptions.rows)
    if (std::any_of(bindings.begin(), bindings.end(),
                    [&](const auto &binding) { return binding.consumer == consumer; }))
      result.push_back(source);
  return result;
}
std::vector<FactKey> watch_sources(const Subscriptions &subscriptions, Handle subject) {
  std::vector<FactKey> result;
  for (const auto &[source, bindings] : subscriptions.rows) {
    (void)bindings;
    if (source.subject == subject)
      result.push_back(source);
  }
  return result;
}
std::vector<ScriptKey> watch_consumers(const Subscriptions &subscriptions, Handle object) {
  std::vector<ScriptKey> result;
  for (const auto &[source, bindings] : subscriptions.rows) {
    (void)source;
    for (const auto &binding : bindings)
      if (binding.consumer.object == object &&
          std::find(result.begin(), result.end(), binding.consumer) == result.end())
        result.push_back(binding.consumer);
  }
  return result;
}
void bind_watch(Subscriptions &subscriptions, FactKey source, WatchBinding binding) {
  auto &rows = subscriptions.rows[source];
  auto found = std::find_if(rows.begin(), rows.end(), [&](const auto &row) {
    return row.consumer == binding.consumer && row.index == binding.index;
  });
  if (found == rows.end())
    rows.push_back(std::move(binding));
  else
    *found = std::move(binding);
}
void unbind_watch(Subscriptions &subscriptions, ScriptKey consumer, std::uint32_t index) {
  for (auto row = subscriptions.rows.begin(); row != subscriptions.rows.end();) {
    std::erase_if(row->second, [&](const WatchBinding &binding) {
      return binding.consumer == consumer && binding.index == index;
    });
    if (row->second.empty())
      row = subscriptions.rows.erase(row);
    else
      ++row;
  }
}
void retire_subscriptions(Subscriptions &subscriptions, Handle object) {
  for (auto row = subscriptions.rows.begin(); row != subscriptions.rows.end();) {
    std::erase_if(row->second,
                  [&](const WatchBinding &binding) { return binding.consumer.object == object; });
    if (row->first.subject == object || row->second.empty())
      row = subscriptions.rows.erase(row);
    else
      ++row;
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
    for (const auto &threshold : binding.declaration.thresholds) {
      auto due = next_crossing(*process, threshold, now, hz);
      if (due && (!first || *due < *first))
        first = due;
    }
    if (first)
      timers.push_back({*first,
                        {binding.consumer.object, source.subject, source.term, Value{}, origin},
                        step::ThresholdToken{source, binding.consumer.object,
                                             binding.consumer.script, binding.index}});
  }
  return timers;
}
} // namespace amber::react
