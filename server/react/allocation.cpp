#include "allocation.hpp"

#include <algorithm>
#include <stdexcept>

namespace amber::react {
namespace {
bool sharing(Combine combine) {
  return combine == Combine::ShareEqual || combine == Combine::SharePriority;
}
template <class T> void unique(std::vector<T> &rows, const T &value) {
  if (std::find(rows.begin(), rows.end(), value) == rows.end())
    rows.push_back(value);
}
void unlink(PriorityLinks &links, FactKey pair) {
  auto old = links.by_pair.find(pair);
  if (old == links.by_pair.end())
    return;
  for (const auto &key : old->second) {
    auto &pairs = links.by_setting.at(key);
    std::erase(pairs, pair);
    if (pairs.empty())
      links.by_setting.erase(key);
  }
  links.by_pair.erase(old);
  auto &subjects = links.by_subject.at(pair.subject.bits);
  std::erase(subjects, pair);
  if (subjects.empty())
    links.by_subject.erase(pair.subject.bits);
}
void link(PriorityLinks &links, const ValueRead &values, FactKey pair) {
  const auto &term = values.terms.rows.at(pair.term);
  if (!term.priority || term.combine != Combine::SharePriority) {
    unlink(links, pair);
    return;
  }
  std::vector<SettingKey> keys;
  if (pair.term < values.shared.size())
    if (const auto *demand = values.shared[pair.term].get(pair.subject.slot()))
      for (const auto &part : demand->parts)
        if (values.identity.valid(part.from)) {
          unique(keys, {SettingKey::Scope::Instance, part.from.bits, term.priority});
          unique(keys, {SettingKey::Scope::Model, values.identity.model[part.from.slot()],
                        term.priority});
          unique(keys, {SettingKey::Scope::Plot, values.plots[part.from.slot()], term.priority});
          unique(keys, {SettingKey::Scope::World, 0, term.priority});
        }
  if (const auto old = links.by_pair.find(pair); old != links.by_pair.end() && old->second == keys)
    return;
  unlink(links, pair);
  for (auto key : keys)
    links.by_setting[key].push_back(pair);
  links.by_pair.emplace(pair, std::move(keys));
  links.by_subject[pair.subject.bits].push_back(pair);
}
} // namespace
std::size_t SettingKeyHash::operator()(const SettingKey &key) const {
  return FactKeyHash{}({Handle{key.subject}, key.term}) ^
         (static_cast<std::size_t>(key.scope) * 0x9e3779b9U);
}
void rebuild_priorities(PriorityLinks &links, const ValueRead &values) {
  links = {};
  for (const auto &term : values.terms.rows) {
    if (!sharing(term.combine))
      continue;
    if (!term.paired || term.paired >= values.terms.rows.size())
      throw std::invalid_argument("shared demand requires a paired supply term");
    const auto &supply = values.terms.rows[term.paired];
    if (term.kind != Kind::Quantity || supply.kind != Kind::Quantity ||
        supply.combine == Combine::None || sharing(supply.combine) ||
        supply.dimensions != term.dimensions)
      throw std::invalid_argument("allocation requires compatible combined supply and demand");
    if (links.demands_by_input.contains(term.paired))
      throw std::invalid_argument("one supply term pairs with one demand term");
    if (term.priority && (term.priority >= values.terms.rows.size() ||
                          values.terms.rows[term.priority].source != Source::Setting ||
                          values.terms.rows[term.priority].kind != Kind::Quantity))
      throw std::invalid_argument("allocation priority requires a quantity setting");
    links.demands_by_input[term.id].push_back(term.id);
    links.demands_by_input[term.paired].push_back(term.id);
    if (term.id < values.shared.size())
      for (const auto slot : values.shared[term.id].slot_of) {
        const auto subject = Handle::make(slot, values.identity.generation.at(slot));
        if (values.identity.valid(subject))
          link(links, values, {subject, term.id});
      }
  }
}
std::vector<FactKey> allocation_inputs(const PriorityLinks &links, FactKey source) {
  std::vector<FactKey> pairs;
  if (auto found = links.demands_by_input.find(source.term); found != links.demands_by_input.end())
    for (auto demand : found->second)
      pairs.push_back({source.subject, demand});
  return pairs;
}
std::vector<FactKey> priority_inputs(const PriorityLinks &links, SettingKey key) {
  auto found = links.by_setting.find(key);
  return found == links.by_setting.end() ? std::vector<FactKey>{} : found->second;
}
bool reconcile_allocation(const ValueRead &values, AllocationRows &allocations, MeterRows &meters,
                          PriorityLinks &links, FactKey pair, Step now, double hz) {
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
  const auto changed = allocate(row(term.paired), demand, term.combine, priority, pair.subject,
                                term.paired, term.id, allocations, meters, now, hz);
  link(links, values, pair);
  return std::any_of(changed.begin(), changed.end(), [](const auto &key) { return !key.supplier; });
}
void retire_priorities(PriorityLinks &links, Handle subject) {
  const auto found = links.by_subject.find(subject.bits);
  if (found == links.by_subject.end())
    return;
  const auto pairs = found->second;
  for (const auto pair : pairs)
    unlink(links, pair);
}
} // namespace amber::react
