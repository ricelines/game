#include "resources.hpp"
#include "bindings/arguments/arguments.hpp"

#include <algorithm>
#include <lua.h>
#include <lualib.h>

namespace amber::bindings {
react::ResourceExchange capture_exchange(lua_State *L, int index,
                                         const react::Properties &properties,
                                         const react::Processes &processes,
                                         const react::Terms &terms, std::size_t entries_max) {
  const auto &inv = react::invocation(L);
  react::ResourceExchange result{inv.self, {}, {}};
  if (lua_isnoneornil(L, index))
    return result;
  luaL_checktype(L, index, LUA_TTABLE);
  const auto table = lua_absindex(L, index);
  lua_pushnil(L);
  while (lua_next(L, table)) {
    const auto *name = luaL_checkstring(L, -2);
    if (std::string_view(name) != "terms" && std::string_view(name) != "state")
      luaL_error(L, "exchange accepts terms and state only");
    lua_pop(L, 1);
  }
  std::size_t entries = 0;
  lua_getfield(L, table, "terms");
  if (!lua_isnil(L, -1)) {
    luaL_checktype(L, -1, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
      if (++entries > entries_max)
        luaL_error(L, "exchange has too many entries");
      const auto id = term_arg(L, -2, terms);
      if (!inv.declaration || std::find(inv.declaration->sets.begin(), inv.declaration->sets.end(),
                                        id) == inv.declaration->sets.end())
        luaL_error(L, "exchange state term was not declared in sets");
      const auto atom = react::check_event_payload(L, -1);
      const auto *after = std::get_if<Value>(&atom);
      if (!after)
        luaL_error(L, "exchange term needs a scalar or handle");
      const auto before =
          react::read_state(properties, processes, inv.self.object, id, inv.step, inv.hz);
      result.terms.push_back({id, before.present ? before.value : Value{}, *after});
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
  lua_getfield(L, table, "state");
  if (!lua_isnil(L, -1)) {
    luaL_checktype(L, -1, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
      if (++entries > entries_max)
        luaL_error(L, "exchange has too many entries");
      auto key = react::check_state_key(L, -2);
      const auto before = inv.state->find(key);
      result.state.push_back(
          {std::move(key),
           before == inv.state->end() ? std::optional<react::StateAtom>{} : before->second,
           react::check_state_atom(L, -1)});
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
  return result;
}
int exchange_binding(lua_State *L) {
  auto &b = *static_cast<ExchangeBinding *>(react::binding_data(L));
  auto &inv = react::invocation(L);
  auto exchange = capture_exchange(L, 2, b.properties, b.processes, b.terms, b.entries_max);
  std::string error;
  const auto applied =
      react::apply_exchange(b.properties, b.processes, *inv.state, b.terms, exchange, inv.step,
                            inv.hz, react::state_capacity(L), *inv.effects, error);
  if (!applied)
    luaL_error(L, "%s", error.c_str());
  react::refresh_state_memory(L);
  lua_pushboolean(L, true);
  return 1;
}
} // namespace amber::bindings
