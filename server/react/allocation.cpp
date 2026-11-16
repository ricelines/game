#include "react.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace amber::react {
namespace {
bool sharing(Combine combine) {
  return combine == Combine::ShareEqual || combine == Combine::SharePriority;
}
} // namespace
void validate_shared_terms(const Terms &terms) {
  std::vector<TermId> paired;
  for (const auto &term : terms.rows) {
    if (!sharing(term.combine))
      continue;
    if (!term.paired || term.paired >= terms.rows.size())
      throw std::invalid_argument("shared demand requires a paired supply term");
    const auto &supply = terms.rows[term.paired];
    if (term.kind != Kind::Quantity || supply.kind != Kind::Quantity ||
        supply.combine == Combine::None || sharing(supply.combine) ||
        supply.dimensions != term.dimensions)
      throw std::invalid_argument("allocation requires compatible combined supply and demand");
    if (std::find(paired.begin(), paired.end(), term.paired) != paired.end())
      throw std::invalid_argument("one supply term pairs with one demand term");
    paired.push_back(term.paired);
    if (term.priority && (term.priority >= terms.rows.size() ||
                          terms.rows[term.priority].source != Source::Setting ||
                          terms.rows[term.priority].kind != Kind::Quantity))
      throw std::invalid_argument("allocation priority requires a quantity setting");
  }
}
std::vector<FactKey> allocation_inputs(const Terms &terms, FactKey source) {
  std::vector<FactKey> pairs;
  for (const auto &term : terms.rows)
    if (sharing(term.combine) && (term.id == source.term || term.paired == source.term))
      pairs.push_back({source.subject, term.id});
  return pairs;
}
std::vector<FactKey> priority_inputs(const ValueRead &values, SettingKey key) {
  std::vector<FactKey> pairs;
  for (const auto &term : values.terms.rows) {
    if (term.combine != Combine::SharePriority || term.priority != key.term ||
        term.id >= values.shared.size())
      continue;
    const auto &column = values.shared[term.id];
    for (std::size_t row = 0; row < column.slot_of.size(); ++row) {
      const auto &demand = column.value[row];
      const bool affected =
          std::any_of(demand.parts.begin(), demand.parts.end(), [&](const Contribution &part) {
            if (!values.identity.valid(part.from))
              return false;
            switch (key.scope) {
            case SettingKey::Scope::Instance:
              return part.from.bits == key.subject;
            case SettingKey::Scope::Model:
              return values.identity.model[part.from.slot()] == key.subject;
            case SettingKey::Scope::Plot:
              return values.plots[part.from.slot()] == key.subject;
            case SettingKey::Scope::World:
              return true;
            }
            return false;
          });
      if (affected) {
        const auto slot = column.slot_of[row];
        const auto subject = values.identity.at(slot);
        if (values.identity.valid(subject))
          pairs.push_back({subject, term.id});
      }
    }
  }
  return pairs;
}
bool reconcile_allocation(const ValueRead &values, amber::react::State &react_state, FactKey pair,
                          Step now, double hz) {

  if (!values.identity.valid(pair.subject))
    return false;
  const auto &term = values.terms.rows.at(pair.term);
  const Shared empty;
  auto row = [&](TermId id) -> const Shared & {
    const auto *found =
        id < values.shared.size() ? values.shared[id].get(pair.subject.slot()) : nullptr;
    return found ? *found : empty;
  };
  const auto &demand = row(term.id);
  PriorityValues priority;
  if (term.priority && term.combine == Combine::SharePriority)
    for (const auto &part : demand.parts) {
      const auto value = read(values, part.from, term.priority, now, hz);
      if (value.present && value.value.is_numeric())
        priority.values.emplace(part.from.bits, value.value.numeric());
    }
  const auto changed =
      allocate(row(term.paired), demand, term.combine, priority, pair.subject, term.paired, term.id,
               react_state, amber::step::Clock{.step = now, .hz = hz});
  return std::any_of(changed.begin(), changed.end(), [](const auto &key) { return !key.supplier; });
}
} // namespace amber::react

namespace amber::react {
namespace {
using Pairs = std::unordered_set<FactKey, FactKeyHash>;
void add(Pairs &pairs, std::vector<FactKey> inputs) {
  pairs.insert(inputs.begin(), inputs.end());
}
void reconcile(const ValueRead &values, amber::react::State &react_state, const Pairs &pairs,
               react::DeferredEffects &effects, Step now, double hz) {

  for (const auto pair : pairs)
    if (react::reconcile_allocation(values, react_state, pair, now, hz))
      effects.ordered.emplace_back(step::WatchWork{pair, {}});
}
} // namespace
void reconcile_allocations(const ValueRead &values, amber::react::State &react_state,
                           react::DeferredEffects &effects, Step now, double hz) {

  Pairs pairs;
  for (const auto &effect : effects.ordered)
    if (const auto *changed = std::get_if<react::ContributionChange>(&effect))
      add(pairs, react::allocation_inputs(values.terms, changed->source));
    else if (const auto *dirty = std::get_if<step::WatchWork>(&effect))
      add(pairs, react::priority_inputs(values, {react::SettingKey::Scope::Instance,
                                                 dirty->source.subject.bits, dirty->source.term}));
  std::erase_if(effects.ordered, [](const auto &effect) {
    return std::holds_alternative<react::ContributionChange>(effect);
  });
  reconcile(values, react_state, pairs, effects, now, hz);
}
void reconcile_priority(const ValueRead &values, amber::react::State &react_state,
                        react::SettingKey scope, react::DeferredEffects &effects, Step now,
                        double hz) {

  Pairs pairs;
  add(pairs, react::priority_inputs(values, scope));
  reconcile(values, react_state, pairs, effects, now, hz);
}
void reconcile_object_priorities(const ValueRead &values, amber::react::State &react_state,
                                 Handle object, react::DeferredEffects &effects, Step now,
                                 double hz) {

  Pairs pairs;
  for (const auto &term : values.terms.rows)
    if (term.combine == react::Combine::SharePriority && term.priority)
      add(pairs, react::priority_inputs(
                     values, {react::SettingKey::Scope::Instance, object.bits, term.priority}));
  reconcile(values, react_state, pairs, effects, now, hz);
}
} // namespace amber::react
