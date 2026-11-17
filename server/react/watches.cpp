#include "catalog/catalog.hpp"
#include "react.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace amber::react {
std::vector<HandlerRef> dispatch(const Triggers &triggers, const ValueRead &values, Handle receiver,
                                 TermId trigger, Step now, double hz) {
  std::vector<HandlerRef> out;
  if (receiver && !values.identity.valid(receiver))
    return out;
  const auto found = triggers.find(trigger);
  if (found == triggers.end())
    return out;
  const auto &row = found->second;
  if (!receiver) {
    for (const auto &handler : row.by_world)
      if (handler.enabled)
        out.push_back(handler);
    return out;
  }
  if (const auto material = row.by_material.find(values.identity.material[receiver.slot()]);
      material != row.by_material.end() && material->second.enabled)
    out.push_back(material->second);
  for (const auto &[flag, handler] : row.by_flag) {
    if (!handler.enabled)
      continue;
    const auto value = read(values, receiver, flag, now, hz);
    if (value.present && (value.value.kind == Value::Kind::Boolean
                              ? value.value.boolean
                              : value.value.is_numeric() && value.value.numeric() != 0))
      out.push_back(handler);
  }
  ScriptId model_script = 0;
  if (const auto model = row.by_model.find(values.identity.model[receiver.slot()]);
      model != row.by_model.end() && model->second.enabled) {
    out.push_back(model->second);
    model_script = model->second.script;
  }
  // Rule scripts cannot be installed; the only possible duplicate is the
  // model's ordinary script also installed directly on this object.
  if (const auto instances = row.by_instance.find(receiver.bits);
      instances != row.by_instance.end())
    for (const auto &handler : instances->second)
      if (handler.enabled && handler.script != model_script)
        out.push_back(handler);
  return out;
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
std::optional<HandlerRef> find_handler(const Triggers &triggers, const ValueRead &values,
                                       ScriptKey consumer, TermId trigger, Step now, double hz) {
  const auto found = triggers.find(trigger);
  if (found == triggers.end())
    return {};
  const auto &row = found->second;
  const auto matches = [&](const HandlerRef &handler) {
    return handler.enabled && handler.script == consumer.script;
  };
  if (!consumer.object) {
    for (const auto &handler : row.by_world)
      if (matches(handler))
        return handler;
    return {};
  }
  if (!values.identity.valid(consumer.object) ||
      ((values.identity.flags[consumer.object.slot()] & Held) &&
       !values.plots[consumer.object.slot()]))
    return {};
  if (const auto material = row.by_material.find(values.identity.material[consumer.object.slot()]);
      material != row.by_material.end() && matches(material->second))
    return material->second;
  for (const auto &[flag, handler] : row.by_flag) {
    if (matches(handler)) {
      const auto value = read(values, consumer.object, flag, now, hz);
      if (value.present && (value.value.kind == Value::Kind::Boolean
                                ? value.value.boolean
                                : value.value.is_numeric() && value.value.numeric() != 0))
        return handler;
    }
  }
  if (const auto model = row.by_model.find(values.identity.model[consumer.object.slot()]);
      model != row.by_model.end() && matches(model->second))
    return model->second;
  if (const auto instances = row.by_instance.find(consumer.object.bits);
      instances != row.by_instance.end())
    for (const auto &handler : instances->second) {
      if (matches(handler))
        return handler;
    }
  return {};
}

namespace {
struct WatchHandler {
  HandlerRef handler;
  const ScriptDeclaration *declaration;
};
std::vector<WatchHandler> watchers(const State &state, const ValueRead &values,
                                   const catalog::Scripts &scripts, FactKey fact, Step now,
                                   double hz) {
  std::vector<WatchHandler> result;
  if (!values.identity.valid(fact.subject) ||
      ((values.identity.flags[fact.subject.slot()] & Held) && !values.plots[fact.subject.slot()]))
    return result;
  for (const auto &handler : dispatch(state.triggers, values, fact.subject, fact.term, now, hz)) {
    const auto &declaration = scripts.rows.at(handler.script).declaration;
    if (declaration.active &&
        std::any_of(declaration.watches.begin(), declaration.watches.end(),
                    [&](const auto &watch) { return watch.term == fact.term; }))
      result.push_back({handler, &declaration});
  }
  return result;
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
WatchAnswer answer(const ValueRead &values, const AllocationRows &allocations, FactKey fact,
                   Handle source, Step now, double hz) {
  const auto &term = values.terms.rows.at(fact.term);
  if (term.kind == Kind::Relation) {
    const auto column = values.relations.columns.find(fact.term);
    return column != values.relations.columns.end() ? read_relation(*column->second, fact.subject)
                                                    : RelationAnswer{};
  }
  if (term.combine != Combine::None) {
    const auto *shared = values.identity.valid(source) && term.id < values.shared.size()
                             ? values.shared[term.id].get(source.slot())
                             : nullptr;
    const auto aggregate = shared ? Answer{true, Value(shared->total)} : Answer{};
    if (term.combine == Combine::ShareEqual || term.combine == Combine::SharePriority)
      return SharedWatchAnswer{aggregate, get_share(allocations, source, term.id, fact.subject)};
    return aggregate;
  }
  return read(values, fact.subject, fact.term, now, hz);
}
const Answer *scalar(const WatchAnswer &value) {
  if (const auto *shared = std::get_if<SharedWatchAnswer>(&value))
    return &shared->aggregate;
  return std::get_if<Answer>(&value);
}
bool changed(const WatchDeclaration &watch, const WatchAnswer &before, const WatchAnswer &after) {
  const auto *old = scalar(before);
  const auto *next = scalar(after);
  if (watch.thresholds.empty() || !old || !next || !old->present || !next->present ||
      !old->value.is_numeric() || !next->value.is_numeric())
    return true;
  const auto *old_share = std::get_if<SharedWatchAnswer>(&before);
  const auto *new_share = std::get_if<SharedWatchAnswer>(&after);
  if (old_share && new_share && old_share->own_allocation != new_share->own_allocation)
    return true;
  return std::any_of(watch.thresholds.begin(), watch.thresholds.end(), [&](const auto &threshold) {
    return threshold.test(old->value.numeric()) != threshold.test(next->value.numeric());
  });
}
std::vector<RelationDelta> relation_changes(const WatchAnswer &before, const WatchAnswer &after) {
  std::vector<RelationDelta> result;
  const auto *next = std::get_if<RelationAnswer>(&after);
  if (!next)
    return result;
  const auto *old = std::get_if<RelationAnswer>(&before);
  const std::vector<Handle> empty;
  auto difference = [&](const auto &previous, const auto &current, bool incoming) {
    std::unordered_set<std::uint64_t> remaining;
    remaining.reserve(current.size());
    for (auto handle : current)
      remaining.insert(handle.bits);
    for (auto handle : previous)
      if (!remaining.erase(handle.bits))
        result.push_back({handle, false, incoming});
    for (auto handle : current)
      if (remaining.contains(handle.bits))
        result.push_back({handle, true, incoming});
  };
  difference(old ? old->out : empty, next->out, false);
  difference(old ? old->in : empty, next->in, true);
  return result;
}
bool state_fact(const Term &term) {
  return term.source == Source::State && term.combine == Combine::None &&
         (term.kind == Kind::Quantity || term.kind == Kind::Flag);
}
std::optional<Timer> crossing_timer(const ValueRead &values, FactKey fact,
                                    std::span<const WatchHandler> handlers, Step now, double hz,
                                    const amber::Origin &origin) {
  const auto *process = fact.term < values.processes.size()
                            ? values.processes[fact.term].get(fact.subject.slot())
                            : nullptr;
  if (!process)
    return {};
  std::optional<Step> first;
  for (const auto &handler : handlers)
    for (const auto &watch : handler.declaration->watches)
      if (watch.term == fact.term)
        for (const auto &threshold : watch.thresholds)
          if (const auto due = next_crossing(*process, threshold, now, hz);
              due && (!first || *due < *first))
            first = due;
  if (!first)
    return {};
  return Timer{*first, {fact.subject, fact.subject, fact.term, {}, origin}, fact};
}
PlannedInvocation invocation(const HandlerRef &handler, FactKey fact, Handle source,
                             const WatchAnswer &current, Step now, double hz,
                             const amber::Origin &origin) {
  InvocationInput input;
  input.receiver = fact.subject;
  input.sender = source;
  input.trigger = fact.term;
  input.step = now;
  input.hz = hz;
  input.origin = origin;
  if (source)
    input.acquired.push_back(source);
  if (const auto *value = scalar(current))
    input.payload = value->present ? value->value : Value{};
  return {handler, std::move(input)};
}
} // namespace

WatchResult evaluate_watch(const State &state, const ValueRead &values,
                           const catalog::Scripts &scripts, const WatchWork &work, Step now,
                           double hz) {
  WatchResult result;
  const bool shared_change = values.terms.rows.at(work.source.term).combine != Combine::None;
  for (const auto fact : wake_targets(values, work.source)) {
    const auto handlers = watchers(state, values, scripts, fact, now, hz);
    if (state_fact(values.terms.rows.at(fact.term)))
      result.thresholds.push_back(
          {fact, crossing_timer(values, fact, handlers, now, hz, work.origin)});
    if (handlers.empty())
      continue;
    const auto source = effective_subject(values, fact);
    result.modeled |= !shared_change || source == work.source.subject;
    const auto current = answer(values, state.allocations, fact, source, now, hz);
    const auto found = state.watch_answers.find(fact);
    const WatchAnswer absent = Answer{};
    const auto &before = found == state.watch_answers.end() ? absent : found->second;
    if (before == current)
      continue;
    const auto deltas = relation_changes(before, current);
    std::shared_ptr<WatchRun> pending;
    for (const auto &handler : handlers) {
      if (!std::any_of(handler.declaration->watches.begin(), handler.declaration->watches.end(),
                       [&](const auto &watch) {
                         return watch.term == fact.term && changed(watch, before, current);
                       }))
        continue;
      if (std::holds_alternative<RelationAnswer>(current) && deltas.empty())
        continue;
      if (!pending)
        pending = std::make_shared<WatchRun>(WatchRun{fact, current});
      auto plan = invocation(handler.handler, fact, source, current, now, hz, work.origin);
      plan.input.watch = pending;
      if (!std::holds_alternative<RelationAnswer>(current)) {
        result.invocations.push_back(std::move(plan));
        continue;
      }
      for (const auto &delta : deltas) {
        auto edge = plan;
        edge.input.payload = std::vector<EventField>{{"other", Value(delta.other)},
                                                     {"added", Value(delta.added)},
                                                     {"incoming", Value(delta.incoming)}};
        if (delta.other != source)
          edge.input.acquired.push_back(delta.other);
        result.invocations.push_back(std::move(edge));
      }
    }
  }
  return result;
}

WatchResult evaluate_catch_up(const State &state, const ValueRead &values,
                              const catalog::Scripts &scripts, const BackgroundItem &item, Step now,
                              double hz) {
  WatchResult result;
  if (!catch_up_valid(item, state.triggers, scripts, values))
    return result;
  for (const auto fact : wake_targets(values, {item.receiver, item.trigger})) {
    const auto source = effective_subject(values, fact);
    if (source != item.receiver)
      continue;
    const auto handlers = watchers(state, values, scripts, fact, now, hz);
    const auto selected = std::find_if(handlers.begin(), handlers.end(), [&](const auto &handler) {
      return handler.handler.script == item.script && handler.handler.version == item.version;
    });
    if (selected == handlers.end())
      continue;
    result.modeled = true;
    const auto current = answer(values, state.allocations, fact, source, now, hz);
    result.invocations.push_back(
        invocation(selected->handler, fact, source, current, now, hz, item.origin));
    if (state_fact(values.terms.rows.at(fact.term)))
      result.thresholds.push_back(
          {fact, crossing_timer(values, fact, handlers, now, hz, item.origin)});
  }
  return result;
}

bool has_watch(const State &state, const ValueRead &values, const catalog::Scripts &scripts,
               FactKey fact, Step now, double hz) {
  return !watchers(state, values, scripts, fact, now, hz).empty();
}
std::optional<Timer> threshold_timer(const State &state, const ValueRead &values,
                                     const catalog::Scripts &scripts, FactKey fact, Step now,
                                     double hz, const amber::Origin &origin) {
  return crossing_timer(values, fact, watchers(state, values, scripts, fact, now, hz), now, hz,
                        origin);
}
void baseline_watches(State &state, const ValueRead &values, const catalog::Scripts &scripts,
                      Handle object, Step now, double hz, const amber::Origin &origin,
                      DeferredEffects &effects) {
  for (const auto &[term, trigger] : state.triggers) {
    (void)trigger;
    const FactKey fact{object, term};
    if (state.watch_answers.contains(fact))
      continue;
    const auto handlers = watchers(state, values, scripts, fact, now, hz);
    if (handlers.empty())
      continue;
    state.watch_answers.emplace(
        fact, answer(values, state.allocations, fact, effective_subject(values, fact), now, hz));
    if (auto timer = crossing_timer(values, fact, handlers, now, hz, origin))
      effects.ordered.emplace_back(std::move(*timer));
  }
}
} // namespace amber::react
