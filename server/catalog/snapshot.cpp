#include "catalog.hpp"
#include "snap/snap.hpp"
#include <algorithm>
namespace amber::catalog {
namespace {
template <class S> void pins(S &s, RuntimePins &value) {
  snap::field(s, value.bake);
  snap::field(s, value.luau);
  snap::field(s, value.host_abi);
  snap::field(s, value.jolt);
}
template <class S> void script(S &s, Script &value) {
  snap::field(s, value.id);
  snap::field(s, value.artifact);
  snap::field(s, value.source_hash);
  snap::field(s, value.bytecode_hash);
  snap::field(s, value.registry_pin);
  pins(s, value.pins);
  snap::field(s, value.author);
  snap::field(s, value.created_s);
  std::string declaration;
  if constexpr (std::is_same_v<S, snap::Output>)
    declaration = declaration_json(value.declaration).dump();
  snap::field(s, declaration);
  if constexpr (std::is_same_v<S, snap::Input>)
    value.declaration = declaration_from_json(nlohmann::json::parse(declaration));
  std::string tests;
  if constexpr (std::is_same_v<S, snap::Output>)
    tests = value.tests.dump();
  snap::field(s, tests);
  if constexpr (std::is_same_v<S, snap::Input>)
    value.tests = nlohmann::json::parse(tests);
}
} // namespace
void write_scripts(snap::Output &out, const Scripts &scripts, ScriptSelection selected) {
  out.value4b(static_cast<std::uint32_t>(scripts.rows.size()));
  std::vector<Script> rows;
  for (const auto &row : scripts.rows)
    if (row.id &&
        (!selected || std::find(selected->begin(), selected->end(), row.id) != selected->end()))
      rows.push_back(row);
  out.container(rows, snap::max_collection(), script<snap::Output>);
}
void read_scripts(snap::Input &in, Scripts &scripts) {
  std::uint32_t size = 0;
  in.value4b(size);
  scripts.rows.clear();
  scripts.rows.resize(size);
  std::vector<Script> rows;
  in.container(rows, snap::max_collection(), script<snap::Input>);
  for (auto &row : rows) {
    if (!row.id || row.id >= size || scripts.rows[row.id].id)
      throw std::runtime_error("invalid script snapshot identity");
    scripts.rows[row.id] = std::move(row);
  }
}
} // namespace amber::catalog
