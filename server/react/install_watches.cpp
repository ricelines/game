#include "catalog/catalog.hpp"
#include "react.hpp"
#include "snap/snap.hpp"

#include <algorithm>
#include <stdexcept>

namespace amber::react {
namespace {
struct WatchKey {
  ScriptKey consumer;
  std::uint32_t index;
  bool operator==(const WatchKey &) const = default;
};
template <class T> void unique(std::vector<T> &values, const T &value) {
  if (std::find(values.begin(), values.end(), value) == values.end())
    values.push_back(value);
}
std::vector<FactKey> wake_targets(const ValueRead &values, FactKey source) {
  std::vector<FactKey> targets{source};
  const auto &changed = values.terms.rows.at(source.term);
  if (changed.kind == Kind::Relation) {
    for (const auto &term : values.terms.rows)
      if (term.combine != Combine::None && term.along == source.term)
        targets.push_back({source.subject, term.id});
  } else if (changed.combine != Combine::None && changed.along) {
    if (const auto relation = values.relations.columns.find(changed.along);
        relation != values.relations.columns.end())
      if (const auto *incoming = relation->second->in.get(source.subject.slot()))
        for (auto consumer : *incoming)
          if (consumer != source.subject && values.identity.valid(consumer))
            targets.push_back({consumer, source.term});
  }
  return targets;
}
Handle effective_subject(const ValueRead &values, FactKey consumer) {
  const auto &term = values.terms.rows.at(consumer.term);
  return term.combine != Combine::None
             ? shared_subject(values.shared, values.relations, consumer.subject, term)
             : consumer.subject;
}
react::WatchAnswer answer(const react::ValueRead &values, const react::AllocationRows &allocations,
                          FactKey source, Handle consumer, Step now, double hz) {
  const auto &term = values.terms.rows.at(source.term);
  if (term.kind == react::Kind::Relation) {
    auto column = values.relations.columns.find(source.term);
    return values.identity.valid(source.subject) && column != values.relations.columns.end()
               ? react::WatchAnswer{react::read_relation(*column->second, source.subject)}
               : react::WatchAnswer{react::RelationAnswer{}};
  }
  if (term.combine != react::Combine::None) {
    const auto *shared = values.identity.valid(source.subject) && term.id < values.shared.size()
                             ? values.shared[term.id].get(source.subject.slot())
                             : nullptr;
    const auto aggregate = shared ? react::Answer{true, Value(shared->total)} : react::Answer{};
    if (term.combine == react::Combine::ShareEqual || term.combine == react::Combine::SharePriority)
      return react::SharedWatchAnswer{
          aggregate, react::get_share(allocations, source.subject, term.id, consumer)};
    return aggregate;
  }
  if (!values.identity.valid(source.subject))
    return react::Answer{};
  return react::read(values, source.subject, source.term, now, hz);
}
react::WatchBinding *existing(react::Subscriptions &subscriptions, FactKey source, WatchKey key) {
  const auto found = subscriptions.rows.find(source);
  if (found != subscriptions.rows.end())
    for (auto &binding : found->second)
      if (binding.consumer == key.consumer && binding.index == key.index)
        return &binding;
  return nullptr;
}
void arm(WatchChanges &result, const react::Subscriptions &subscriptions,
         const react::ValueRead &values, FactKey source, Step now, double hz,
         const amber::Origin &origin) {
  if (!values.identity.valid(source.subject))
    return;
  auto timers = react::threshold_timers(subscriptions, values.processes, source, now, hz, origin);
  result.timers.insert(result.timers.end(), std::make_move_iterator(timers.begin()),
                       std::make_move_iterator(timers.end()));
}
std::vector<WatchKey> consumer_keys(const react::Subscriptions &subscriptions, ScriptKey consumer) {
  std::vector<WatchKey> keys;
  for (const auto &[source, bindings] : subscriptions.rows) {
    (void)source;
    for (const auto &binding : bindings)
      if (binding.consumer == consumer)
        unique(keys, WatchKey{consumer, binding.index});
  }
  return keys;
}
} // namespace
void remove_watches(Subscriptions &subscriptions, ScriptKey consumer) {
  for (auto row = subscriptions.rows.begin(); row != subscriptions.rows.end();) {
    std::erase_if(row->second,
                  [&](const WatchBinding &binding) { return binding.consumer == consumer; });
    if (row->second.empty())
      row = subscriptions.rows.erase(row);
    else
      ++row;
  }
}
WatchChanges sync_watches(amber::react::State &react_state, const react::ValueRead &values,
                          ScriptKey consumer, const react::ScriptDeclaration &declaration, Step now,
                          double hz, const amber::Origin &origin, SyncMode mode,
                          std::optional<std::span<const TermId>> allowed_terms) {

  auto &subscriptions = react_state.subscriptions;
  auto &allocations = react_state.allocations;

  WatchChanges changes;
  if (!declaration.active) {
    remove_watches(subscriptions, consumer);
    return changes;
  }
  auto allowed = [&](std::uint32_t index) {
    return !allowed_terms || std::find(allowed_terms->begin(), allowed_terms->end(),
                                       declaration.watches[index].term) != allowed_terms->end();
  };
  for (auto key : consumer_keys(subscriptions, consumer))
    if (key.index >= declaration.watches.size() || !allowed(key.index)) {
      react::unbind_watch(subscriptions, consumer, key.index);
    }
  std::vector<FactKey> armed;
  for (std::uint32_t index = 0; index < declaration.watches.size(); ++index) {
    if (!allowed(index))
      continue;
    const WatchKey key{consumer, index};
    const FactKey source{consumer.object, declaration.watches[index].term};
    auto *old = existing(subscriptions, source, key);
    if (old && mode != SyncMode::Initial) {
      old->declaration = declaration.watches[index];
      continue;
    }
    const FactKey value_source{effective_subject(values, source), source.term};
    react::WatchBinding binding{consumer, index,
                                answer(values, allocations, value_source, consumer.object, now, hz),
                                declaration.watches[index]};
    if (!old && mode == SyncMode::Change)
      binding.last = values.terms.rows[source.term].kind == react::Kind::Relation
                         ? react::WatchAnswer{react::RelationAnswer{}}
                         : react::WatchAnswer{react::Answer{}};
    react::bind_watch(subscriptions, std::move(binding));
    if (mode == SyncMode::Change)
      changes.dirty.push_back({source, origin});
    unique(armed, source);
  }
  for (auto source : armed)
    arm(changes, subscriptions, values, source, now, hz, origin);
  return changes;
}
std::vector<Handle> source_consumers(const react::ValueRead &values, FactKey source,
                                     const react::WatchDeclaration &watch) {
  std::vector<Handle> consumers;
  for (const auto target : wake_targets(values, source))
    if (target.term == watch.term && values.identity.valid(target.subject) &&
        effective_subject(values, target) == source.subject)
      consumers.push_back(target.subject);
  return consumers;
}
WatchChanges sync_rule_watches(amber::react::State &react_state, const react::ValueRead &values,
                               const catalog::Scripts &scripts,
                               std::span<const react::Effect> outputs, Step now, double hz,
                               const amber::Origin &origin) {
  auto &subscriptions = react_state.subscriptions;

  auto &rules = react_state.rules;

  std::unordered_set<FactKey, FactKeyHash> changed;
  for (const auto &effect : outputs)
    if (const auto *work = std::get_if<react::WatchWork>(&effect);
        work && values.identity.valid(work->source.subject))
      changed.insert(work->source);
  std::unordered_set<ScriptKey, ScriptKeyHash> consumers;
  const auto current = latest_rules(rules);
  for (const auto &fact : changed) {
    for (const auto &rule : current) {
      if (!rule.handler.enabled)
        continue;
      if (rule.flag_selector && rule.selector == fact.term)
        consumers.insert({fact.subject, rule.handler.script});
      const auto &declaration = scripts.rows.at(rule.handler.script).declaration;
      for (const auto &watch : declaration.watches) {
        if (watch.term == fact.term)
          for (auto h : source_consumers(values, fact, watch))
            consumers.insert({h, rule.handler.script});
        const auto &term = values.terms.rows.at(watch.term);
        if (term.combine != react::Combine::None && term.along == fact.term)
          consumers.insert({fact.subject, rule.handler.script});
      }
    }
    for (const auto &rule : rules.history)
      if (rule.flag_selector && rule.selector == fact.term &&
          !react::watch_sources(subscriptions, ScriptKey{fact.subject, rule.handler.script})
               .empty())
        consumers.insert({fact.subject, rule.handler.script});
  }
  WatchChanges result;
  for (const auto &consumer : consumers) {
    const auto allowed = rule_watch_terms(rules, values, consumer.object, consumer.script, now, hz);
    if (allowed.empty()) {
      remove_watches(subscriptions, consumer);
      continue;
    }
    auto changes =
        sync_watches(react_state, values, consumer, scripts.rows.at(consumer.script).declaration,
                     now, hz, origin, SyncMode::Change, std::span<const TermId>(allowed));
    result.dirty.insert(result.dirty.end(), std::make_move_iterator(changes.dirty.begin()),
                        std::make_move_iterator(changes.dirty.end()));
    result.timers.insert(result.timers.end(), std::make_move_iterator(changes.timers.begin()),
                         std::make_move_iterator(changes.timers.end()));
  }
  return result;
}
WatchResult check_changed(react::Subscriptions &subscriptions, const react::ValueRead &values,
                          const react::AllocationRows &allocations, FactKey source, Step now,
                          double hz, const amber::Origin &origin, ScriptId recipient) {
  WatchResult result;
  for (const auto target : wake_targets(values, source)) {
    if (!subscriptions.rows.contains(target))
      continue;
    const FactKey value_source{effective_subject(values, target), target.term};
    const auto current = answer(values, allocations, value_source, target.subject, now, hz);
    auto deliveries = react::check_watch(subscriptions, target, current, recipient);
    for (auto &delivery : deliveries) {
      delivery.source = value_source.subject;
      result.deliveries.push_back(std::move(delivery));
    }
    if (values.identity.valid(target.subject)) {
      auto timers =
          react::threshold_timers(subscriptions, values.processes, target, now, hz, origin);
      for (auto &timer : timers)
        if (!recipient || timer.threshold->script == recipient)
          result.timers.push_back(std::move(timer));
    }
  }
  return result;
}
std::vector<InvocationPlan> plan_deliveries(const WatchResult &result,
                                            const react::Triggers &triggers,
                                            const catalog::Scripts &scripts,
                                            const react::ValueRead &values, Step now, double hz) {
  struct Ranked {
    std::size_t rank;
    std::uint32_t index;
    InvocationPlan plan;
  };
  std::vector<Ranked> ranked;
  for (const auto &delivery : result.deliveries) {
    const auto &declaration = scripts.rows.at(delivery.consumer.script).declaration;
    if (delivery.index >= declaration.watches.size())
      continue;
    const auto trigger = declaration.watches[delivery.index].term;
    auto found = find_handler(triggers, values, delivery.consumer, trigger, now, hz);
    if (!found)
      continue;
    const auto &[handler, rank] = *found;
    auto add = [&](EventPayload payload, Handle other = {}) {
      InvocationPlan plan{handler, delivery.consumer.object, delivery.source,
                          trigger, std::move(payload),       {}};
      plan.watch = WatchRun{{delivery.consumer.object, trigger}, delivery.index, delivery.value};
      if (delivery.source)
        unique(plan.acquired, delivery.source);
      if (other.bits)
        unique(plan.acquired, other);
      ranked.push_back({rank, delivery.index, std::move(plan)});
    };
    if (const auto *value = std::get_if<react::Answer>(&delivery.value))
      add(value->present ? value->value : Value{});
    else if (const auto *shared = std::get_if<react::SharedWatchAnswer>(&delivery.value))
      add(shared->aggregate.present ? shared->aggregate.value : Value{});
    else
      for (const auto &delta : delivery.relations)
        add(std::vector<EventField>{{"other", Value(delta.other)},
                                    {"added", Value(delta.added)},
                                    {"incoming", Value(delta.incoming)}},
            delta.other);
  }
  std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked &a, const Ranked &b) {
    return std::tie(a.plan.receiver.bits, a.rank, a.index) <
           std::tie(b.plan.receiver.bits, b.rank, b.index);
  });
  std::vector<InvocationPlan> plans;
  plans.reserve(ranked.size());
  for (auto &row : ranked)
    plans.push_back(std::move(row.plan));
  return plans;
}
bool has_applicable_watch(const react::Subscriptions &subscriptions,
                          const react::Triggers &triggers, const catalog::Scripts &scripts,
                          const react::ValueRead &values, FactKey source, Step now, double hz) {
  for (const auto target : wake_targets(values, source)) {
    const auto row = subscriptions.rows.find(target);
    if (row == subscriptions.rows.end())
      continue;
    if (values.terms.rows.at(source.term).combine != Combine::None &&
        effective_subject(values, target) != source.subject)
      continue;
    for (const auto &binding : row->second) {
      const auto &declaration = scripts.rows.at(binding.consumer.script).declaration;
      if (find_handler(triggers, values, binding.consumer,
                       declaration.watches.at(binding.index).term, now, hz))
        return true;
    }
  }
  return false;
}
} // namespace amber::react
