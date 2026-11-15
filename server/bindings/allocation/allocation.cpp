#include "allocation.hpp"

#include <unordered_set>

namespace amber::bindings {
namespace {
using Pairs = std::unordered_set<FactKey, FactKeyHash>;
void add(Pairs &pairs, std::vector<FactKey> inputs) {
  pairs.insert(inputs.begin(), inputs.end());
}
void reconcile(const react::ValueRead &values, AllocationReconciliation &tables, const Pairs &pairs,
               react::DeferredEffects &effects, Step now, double hz) {
  for (const auto pair : pairs)
    if (react::reconcile_allocation(values, tables.allocations, tables.meters, tables.priorities,
                                    pair, now, hz))
      effects.ordered.emplace_back(step::WatchWork{pair, {}});
}
} // namespace
void reconcile_allocations(const react::ValueRead &values, AllocationReconciliation &tables,
                           react::DeferredEffects &effects, Step now, double hz) {
  Pairs pairs;
  for (const auto &effect : effects.ordered)
    if (const auto *changed = std::get_if<react::ContributionChange>(&effect))
      add(pairs, react::allocation_inputs(tables.priorities, changed->source));
    else if (const auto *dirty = std::get_if<step::WatchWork>(&effect))
      add(pairs, react::priority_inputs(tables.priorities,
                                        {react::SettingKey::Scope::Instance,
                                         dirty->source.subject.bits, dirty->source.term}));
  std::erase_if(effects.ordered, [](const auto &effect) {
    return std::holds_alternative<react::ContributionChange>(effect);
  });
  reconcile(values, tables, pairs, effects, now, hz);
}
void reconcile_priority(const react::ValueRead &values, AllocationReconciliation &tables,
                        react::SettingKey scope, react::DeferredEffects &effects, Step now,
                        double hz) {
  Pairs pairs;
  add(pairs, react::priority_inputs(tables.priorities, scope));
  reconcile(values, tables, pairs, effects, now, hz);
}
void reconcile_object_priorities(const react::ValueRead &values, AllocationReconciliation &tables,
                                 Handle object, react::DeferredEffects &effects, Step now,
                                 double hz) {
  Pairs pairs;
  for (const auto &term : values.terms.rows)
    if (term.combine == react::Combine::SharePriority && term.priority)
      add(pairs, react::priority_inputs(tables.priorities, {react::SettingKey::Scope::Instance,
                                                            object.bits, term.priority}));
  reconcile(values, tables, pairs, effects, now, hz);
}
} // namespace amber::bindings
