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
struct ResolvedWatch {
  FactKey source;
  std::vector<FactKey> dependencies;
};
template <class T> void unique(std::vector<T> &values, const T &value) {
  if (std::find(values.begin(), values.end(), value) == values.end())
    values.push_back(value);
}
Handle first_out(const react::ValueRead &values, Handle object, TermId relation) {
  if (!values.identity.valid(object))
    return {};
  auto column = values.relations.columns.find(relation);
  if (column == values.relations.columns.end())
    return {};
  const auto *edges = column->second->out.get(object.slot());
  if (!edges || edges->empty() || !values.identity.valid(edges->front()))
    return {};
  return edges->front();
}
ResolvedWatch resolve(const react::ValueRead &values, ScriptKey consumer,
                      const react::WatchDeclaration &declaration) {
  ResolvedWatch link;
  Handle subject = consumer.object;
  link.source = {subject, declaration.term};
  const auto &term = values.terms.rows.at(declaration.term);
  if (!subject.bits || term.combine == react::Combine::None || !term.along)
    return link;
  unique(link.dependencies, FactKey{subject, declaration.term});
  if (term.id < values.shared.size() && values.shared[term.id].get(subject.slot()))
    return link;
  unique(link.dependencies, FactKey{subject, term.along});
  const auto fallback = first_out(values, subject, term.along);
  if (fallback.bits)
    link.source.subject = fallback;
  return link;
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
std::optional<std::pair<FactKey, react::WatchBinding>>
existing(const react::Subscriptions &subscriptions, WatchKey key) {
  for (const auto &[source, bindings] : subscriptions.rows)
    for (const auto &binding : bindings)
      if (binding.consumer == key.consumer && binding.index == key.index)
        return std::pair{source, binding};
  return {};
}
void arm(WatchChanges &result, const react::Subscriptions &subscriptions,
         const react::ValueRead &values, FactKey source, Step now, double hz,
         const record::Origin &origin) {
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
                          ScriptKey consumer, const react::ScriptDeclaration &declaration,
                          const amber::step::Clock &clock, const record::Origin &origin,
                          SyncMode mode, std::optional<std::span<const TermId>> allowed_terms) {
  const auto now = clock.step;
  const auto hz = clock.hz;

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
    auto next = resolve(values, consumer, declaration.watches[index]);
    auto old = existing(subscriptions, key);
    const auto source = next.source;
    if (old && mode != SyncMode::Initial && old->first == source) {
      auto &bindings = subscriptions.rows.at(source);
      auto &binding = *std::find_if(bindings.begin(), bindings.end(), [&](const auto &row) {
        return row.consumer == consumer && row.index == index;
      });
      binding.declaration = declaration.watches[index];
      binding.dependencies = std::move(next.dependencies);
      continue;
    }
    react::WatchBinding binding{consumer, index,
                                answer(values, allocations, source, consumer.object, now, hz),
                                declaration.watches[index], std::move(next.dependencies)};
    if (!old && mode == SyncMode::Change)
      binding.last = values.terms.rows[source.term].kind == react::Kind::Relation
                         ? react::WatchAnswer{react::RelationAnswer{}}
                         : react::WatchAnswer{react::Answer{}};
    if (old) {
      if (mode != SyncMode::Initial)
        binding.last = old->second.last;
      react::unbind_watch(subscriptions, consumer, index);
    }
    react::bind_watch(subscriptions, source, std::move(binding));
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
  auto incoming = [&](Handle subject, TermId relation) {
    std::vector<Handle> result;
    if (values.identity.valid(subject))
      if (auto found = values.relations.columns.find(relation);
          found != values.relations.columns.end())
        if (const auto *edges = found->second->in.get(subject.slot()))
          for (auto h : *edges)
            if (values.identity.valid(h) && first_out(values, h, relation) == subject)
              result.push_back(h);
    return result;
  };
  std::vector<Handle> subjects{source.subject};
  const auto &term = values.terms.rows.at(watch.term);
  if (term.combine != react::Combine::None && term.along) {
    const auto fallback = incoming(source.subject, term.along);
    for (auto h : fallback)
      if (term.id >= values.shared.size() || !values.shared[term.id].get(h.slot()))
        unique(subjects, h);
  }
  std::vector<Handle> consumers;
  for (auto h : subjects)
    if (values.identity.valid(h))
      consumers.push_back(h);
  std::erase_if(consumers,
                [&](Handle h) { return resolve(values, {h, 0}, watch).source != source; });
  return consumers;
}
WatchChanges sync_rule_watches(amber::react::State &react_state, const react::ValueRead &values,
                               const catalog::Scripts &scripts,
                               std::span<const react::Effect> outputs, Step now, double hz,
                               const record::Origin &origin) {
  auto &subscriptions = react_state.subscriptions;

  auto &rules = react_state.rules;

  std::unordered_set<FactKey, FactKeyHash> changed;
  for (const auto &effect : outputs)
    if (const auto *work = std::get_if<step::WatchWork>(&effect);
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
                     amber::step::Clock{.step = now, .hz = hz}, origin, SyncMode::Change,
                     std::span<const TermId>(allowed));
    result.dirty.insert(result.dirty.end(), std::make_move_iterator(changes.dirty.begin()),
                        std::make_move_iterator(changes.dirty.end()));
    result.timers.insert(result.timers.end(), std::make_move_iterator(changes.timers.begin()),
                         std::make_move_iterator(changes.timers.end()));
  }
  return result;
}
WatchChanges refresh_dependencies(react::Subscriptions &subscriptions,
                                  const react::ValueRead &values, FactKey changed, Step now,
                                  double hz, const record::Origin &origin) {
  WatchChanges result;
  std::vector<WatchKey> keys;
  for (const auto &[source, bindings] : subscriptions.rows) {
    (void)source;
    for (const auto &binding : bindings)
      if (std::find(binding.dependencies.begin(), binding.dependencies.end(), changed) !=
          binding.dependencies.end())
        keys.push_back({binding.consumer, binding.index});
  }
  std::vector<FactKey> armed;
  for (auto key : keys) {
    auto old = existing(subscriptions, key);
    auto next = resolve(values, key.consumer, old->second.declaration);
    if (next.source == old->first && next.dependencies == old->second.dependencies)
      continue;
    const auto source = next.source;
    old->second.dependencies = std::move(next.dependencies);
    react::unbind_watch(subscriptions, key.consumer, key.index);
    react::bind_watch(subscriptions, source, std::move(old->second));
    if (source != old->first) {
      result.dirty.push_back({source, origin});
      unique(armed, source);
    }
  }
  for (auto source : armed)
    arm(result, subscriptions, values, source, now, hz, origin);
  return result;
}
WatchResult check_changed(react::Subscriptions &subscriptions, const react::ValueRead &values,
                          const react::AllocationRows &allocations, FactKey source, Step now,
                          double hz, const record::Origin &origin, ScriptId recipient) {
  react::CurrentAnswers current;
  current.values.emplace(source, answer(values, allocations, source, {}, now, hz));
  const auto combine = values.terms.rows.at(source.term).combine;
  if (combine == react::Combine::ShareEqual || combine == react::Combine::SharePriority)
    if (const auto found = subscriptions.rows.find(source); found != subscriptions.rows.end())
      for (const auto &binding : found->second)
        current.by_consumer.emplace(binding.consumer, answer(values, allocations, source,
                                                             binding.consumer.object, now, hz));
  WatchResult result{source, react::check_watch(subscriptions, source, current, recipient), {}};
  if (values.identity.valid(source.subject))
    result.timers =
        react::threshold_timers(subscriptions, values.processes, source, now, hz, origin);
  if (recipient)
    std::erase_if(result.timers, [&](const auto &timer) {
      return !timer.threshold || timer.threshold->script != recipient;
    });
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
      InvocationPlan plan{handler, delivery.consumer.object, result.source.subject,
                          trigger, std::move(payload),       {}};
      plan.watch = WatchRun{result.source, delivery.index, delivery.value};
      if (result.source.subject.bits)
        unique(plan.acquired, result.source.subject);
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
  const auto row = subscriptions.rows.find(source);
  if (row == subscriptions.rows.end())
    return false;
  for (const auto &binding : row->second) {
    const auto &declaration = scripts.rows.at(binding.consumer.script).declaration;
    if (find_handler(triggers, values, binding.consumer, declaration.watches.at(binding.index).term,
                     now, hz))
      return true;
  }
  return false;
}
} // namespace amber::react
