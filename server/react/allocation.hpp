#pragma once
#include "react.hpp"

namespace amber::react {
struct SettingKey {
  enum class Scope { Instance, Model, Plot, World } scope = Scope::Instance;
  std::uint64_t subject = 0;
  TermId term = 0;
  bool operator==(const SettingKey &) const = default;
};
struct SettingKeyHash {
  std::size_t operator()(const SettingKey &) const;
};
struct PriorityLinks {
  std::unordered_map<TermId, std::vector<TermId>> demands_by_input;
  std::unordered_map<FactKey, std::vector<SettingKey>, FactKeyHash> by_pair;
  std::unordered_map<SettingKey, std::vector<FactKey>, SettingKeyHash> by_setting;
  std::unordered_map<std::uint64_t, std::vector<FactKey>> by_subject;
};
void rebuild_priorities(PriorityLinks &, const ValueRead &);
std::vector<FactKey> allocation_inputs(const PriorityLinks &, FactKey);
std::vector<FactKey> priority_inputs(const PriorityLinks &, SettingKey);
bool reconcile_allocation(const ValueRead &, AllocationRows &, MeterRows &, PriorityLinks &,
                          FactKey demand, Step, double hz);
void retire_priorities(PriorityLinks &, Handle);
} // namespace amber::react
