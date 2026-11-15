#include "exchange.hpp"

#include <unordered_set>

namespace amber::react {
std::string check_exchange(const Properties &properties, const Processes &processes,
                           const ScriptState &state, const Terms &terms,
                           const ResourceExchange &exchange, Step now, double hz,
                           std::size_t capacity) {
  std::unordered_set<TermId> visited_terms;
  for (const auto &entry : exchange.terms) {
    if (!entry.term || entry.term >= terms.rows.size())
      return "unknown exchange term";
    const auto &term = terms.rows[entry.term];
    if ((term.kind != Kind::Quantity && term.kind != Kind::Flag) || term.source != Source::State ||
        term.combine != Combine::None)
      return "exchange term is not writable state";
    if (!visited_terms.insert(entry.term).second)
      return "exchange term is repeated";
    if (!entry.after.present() ||
        (entry.after.kind == Value::Kind::Number && !std::isfinite(entry.after.number)))
      return "exchange value must be present and finite";
    const auto before =
        read_state(properties, processes, exchange.holder.object, entry.term, now, hz);
    if ((before.present ? before.value : Value{}) != entry.before)
      return "exchange prior term answer changed";
    if (entry.term < processes.size())
      if (const auto *p = processes[entry.term].get(exchange.holder.object.slot())) {
        if ((entry.after.kind != Value::Kind::Number && entry.after.kind != Value::Kind::Integer) ||
            entry.after.numeric() < p->lo || entry.after.numeric() > p->hi)
          return "exchange would exceed current process bounds";
      }
  }
  auto bytes = state_bytes(state);
  std::unordered_set<StateKey, StateKeyHash> visited_keys;
  for (const auto &entry : exchange.state) {
    if (!visited_keys.insert(entry.key).second)
      return "exchange flat-state key is repeated";
    if (const auto *number = std::get_if<double>(&entry.after); number && !std::isfinite(*number))
      return "exchange flat-state number must be finite";
    const auto old = state.find(entry.key);
    if ((old == state.end() ? std::optional<StateAtom>{} : old->second) != entry.before)
      return "exchange prior flat-state answer changed";
    if (old != state.end())
      bytes -= state_entry_bytes(old->first, old->second);
    bytes += state_entry_bytes(entry.key, entry.after);
  }
  return bytes > capacity ? "exchange exceeds script state memory limit" : "";
}
bool apply_exchange(Properties &properties, Processes &processes, ScriptState &state,
                    const Terms &terms, const ResourceExchange &exchange, Step now, double hz,
                    std::size_t capacity, DeferredEffects &effects, std::string &error) {
  error = check_exchange(properties, processes, state, terms, exchange, now, hz, capacity);
  if (!error.empty())
    return false;
  bool changed = false;
  for (const auto &entry : exchange.terms) {
    if (entry.before == entry.after)
      continue;
    if (properties.size() <= entry.term)
      properties.resize(entry.term + 1);
    if (entry.term < processes.size())
      if (auto *p = processes[entry.term].get(exchange.holder.object.slot())) {
        p->v0 = entry.after.numeric();
        p->step0 = now;
        ++p->revision;
      }
    properties[entry.term].set(exchange.holder.object.slot(), entry.after);
    effects.ordered.push_back(step::WatchWork{{exchange.holder.object, entry.term}, {}});
    changed = true;
  }
  for (const auto &entry : exchange.state)
    if (!entry.before || *entry.before != entry.after) {
      state[entry.key] = entry.after;
      changed = true;
    }
  effects.changed |= changed;
  return true;
}
} // namespace amber::react
