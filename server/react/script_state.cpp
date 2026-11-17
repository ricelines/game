#include "react.hpp"

namespace amber::react {
namespace {
std::size_t atom_bytes(const StateAtom &atom) {
  return sizeof(StateAtom) +
         (std::holds_alternative<std::string>(atom) ? std::get<std::string>(atom).size() : 0);
}
} // namespace

std::size_t state_key_bytes(const StateKey &key) {
  return sizeof(StateKey) +
         (std::holds_alternative<std::string>(key) ? std::get<std::string>(key).size() : 0);
}
std::size_t state_entry_bytes(const StateKey &key, const StateAtom &value) {
  return state_key_bytes(key) + atom_bytes(value) + sizeof(void *) * 2;
}
std::pair<ScriptState::Entries::iterator, bool> ScriptState::insert(StateKey key, StateAtom value) {
  auto result = values.try_emplace(std::move(key), std::move(value));
  if (result.second)
    bytes += state_entry_bytes(result.first->first, result.first->second);
  return result;
}
void ScriptState::replace(Entries::iterator entry, StateAtom value) {
  bytes -= atom_bytes(entry->second);
  entry->second = std::move(value);
  bytes += atom_bytes(entry->second);
}
void ScriptState::erase(Entries::iterator entry) {
  bytes -= state_entry_bytes(entry->first, entry->second);
  values.erase(entry);
}
void ScriptState::restore(Entries entries) {
  values = std::move(entries);
  bytes = 0;
  for (const auto &[key, value] : values)
    bytes += state_entry_bytes(key, value);
}

std::size_t StateKeyHash::operator()(const StateKey &key) const {
  if (const auto *text = std::get_if<std::string>(&key))
    return (*this)(StateStringKey{*text});
  return std::hash<std::int64_t>{}(std::get<std::int64_t>(key)) ^ 0x9e3779b9;
}
std::size_t StateKeyHash::operator()(StateStringKey key) const {
  return std::hash<std::string_view>{}(key.text);
}
bool StateKeyEqual::operator()(const StateKey &a, const StateKey &b) const {
  return a == b;
}
bool StateKeyEqual::operator()(const StateKey &a, StateStringKey b) const {
  const auto *text = std::get_if<std::string>(&a);
  return text && *text == b.text;
}
bool StateKeyEqual::operator()(StateStringKey a, const StateKey &b) const {
  return (*this)(b, a);
}
} // namespace amber::react
