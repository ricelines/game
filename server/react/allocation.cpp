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
std::vector<FactKey> priority_inputs(const ValueRead &values, const GivesTo &gives,
                                     SettingKey key) {
  std::vector<FactKey> pairs;
  if (values.terms.rows[key.term].source != Source::Setting)
    return pairs;
  if (key.scope == SettingKey::Scope::Instance) {
    const Handle contributor{key.subject};
    if (!values.identity.valid(contributor))
      return pairs;
    if (const auto *contributions = gives.get(contributor.slot()))
      for (const auto &source : *contributions) {
        const auto &term = values.terms.rows[source.term];
        if (term.combine == Combine::SharePriority && term.priority == key.term &&
            values.identity.valid(source.subject))
          pairs.push_back({source.subject, source.term});
      }
    return pairs;
  }
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
            return key.scope == SettingKey::Scope::Model
                       ? values.identity.model[part.from.slot()] == key.subject
                   : key.scope == SettingKey::Scope::Plot
                       ? values.plots[part.from.slot()] == key.subject
                       : true;
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
  return allocate(row(term.paired), demand, term.combine, priority, pair, react_state.allocations);
}
double read_share(const ValueRead &values, State &state, DeferredEffects &effects, FactKey pair,
                  Handle contributor, Step now, double hz) {
  if (!values.identity.valid(pair.subject))
    return 0;
  const auto supply = values.terms.rows[pair.term].paired;
  const auto pending = std::erase_if(effects.ordered, [&](const Effect &effect) {
    const auto *change = std::get_if<ContributionChange>(&effect);
    return change && change->source.subject == pair.subject &&
           (change->source.term == pair.term || change->source.term == supply);
  });
  if (pending && reconcile_allocation(values, state, pair, now, hz))
    effects.ordered.emplace_back(WatchWork{pair, {}});
  return get_share(state.allocations, pair.subject, pair.term, contributor);
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
      effects.ordered.emplace_back(react::WatchWork{pair, {}});
}
} // namespace
void reconcile_allocations(const ValueRead &values, amber::react::State &react_state,
                           react::DeferredEffects &effects, Step now, double hz) {

  Pairs pairs;
  for (const auto &effect : effects.ordered)
    if (const auto *changed = std::get_if<react::ContributionChange>(&effect))
      add(pairs, react::allocation_inputs(values.terms, changed->source));
  std::erase_if(effects.ordered, [](const auto &effect) {
    return std::holds_alternative<react::ContributionChange>(effect);
  });
  reconcile(values, react_state, pairs, effects, now, hz);
}
void reconcile_priority(const ValueRead &values, amber::react::State &react_state,
                        react::SettingKey scope, react::DeferredEffects &effects, Step now,
                        double hz) {

  Pairs pairs;
  add(pairs, react::priority_inputs(values, react_state.gives_to, scope));
  reconcile(values, react_state, pairs, effects, now, hz);
}
void reconcile_object_priorities(const ValueRead &values, amber::react::State &react_state,
                                 Handle object, react::DeferredEffects &effects, Step now,
                                 double hz) {
  const auto *contributions = react_state.gives_to.get(object.slot());
  if (!contributions)
    return;
  for (const auto &source : *contributions) {
    const auto &term = values.terms.rows[source.term];
    const FactKey pair{source.subject, source.term};
    if (term.combine == Combine::SharePriority && term.priority &&
        reconcile_allocation(values, react_state, pair, now, hz))
      effects.ordered.emplace_back(WatchWork{pair, {}});
  }
}
} // namespace amber::react
