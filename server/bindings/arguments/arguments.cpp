#include "arguments.hpp"

#include <array>
#include <charconv>
#include <lua.h>
#include <lualib.h>

namespace amber::bindings {
std::uint32_t unsigned_arg(lua_State *L, int index, bool allow_zero) {
  if (lua_type(L, index) == LUA_TNUMBER) {
    const auto value = lua_tonumber(L, index);
    if (std::isfinite(value) && value >= (allow_zero ? 0 : 1) && value <= UINT32_MAX &&
        std::trunc(value) == value)
      return static_cast<std::uint32_t>(value);
  }
  luaL_error(L, "expected a %s integral id", allow_zero ? "nonnegative" : "positive");
}
namespace {
std::uint32_t tagged_id(lua_State *L, int index, char prefix) {
  if (lua_type(L, index) == LUA_TNUMBER)
    return unsigned_arg(L, index);
  std::size_t size = 0;
  const auto *value = luaL_checklstring(L, index, &size);
  std::uint32_t id = 0;
  if (size > 2 && value[0] == prefix && value[1] == ':') {
    const auto parsed = std::from_chars(value + 2, value + size, id);
    if (parsed.ec == std::errc{} && parsed.ptr == value + size && id)
      return id;
  }
  luaL_error(L, "expected a positive id or %c:N", prefix);
}
} // namespace
ModelId model_arg(lua_State *L, int index) {
  return tagged_id(L, index, 'm');
}
PlayerId player_arg(lua_State *L, int index) {
  return tagged_id(L, index, 'p');
}
TermId term_arg(lua_State *L, int index, const react::Terms &terms) {
  if (lua_type(L, index) == LUA_TSTRING) {
    std::size_t size;
    const auto *name = lua_tolstring(L, index, &size);
    const auto found = terms.by_name.find(std::string(name, size));
    if (found != terms.by_name.end())
      return found->second;
  } else if (lua_type(L, index) == LUA_TNUMBER) {
    const auto id = unsigned_arg(L, index);
    if (id < terms.rows.size())
      return id;
  }
  luaL_error(L, "unknown registered term");
}
Vec3 vector_arg(lua_State *L, int index) {
  if (lua_type(L, index) != LUA_TVECTOR)
    luaL_error(L, "expected vector.create(x,y,z)");
  const auto *v = lua_tovector(L, index);
  const Vec3 result{v[0], v[1], v[2]};
  if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z))
    luaL_error(L, "vector must be finite");
  return result;
}
Quat rotation_arg(lua_State *L, int index) {
  if (lua_isnoneornil(L, index))
    return {};
  luaL_checktype(L, index, LUA_TTABLE);
  const auto table = lua_absindex(L, index);
  std::array<double, 4> values;
  double norm = 0;
  for (int i = 0; i < 4; ++i) {
    lua_rawgeti(L, table, i + 1);
    values[i] = luaL_checknumber(L, -1);
    lua_pop(L, 1);
    if (!std::isfinite(values[i]))
      luaL_error(L, "rotation must be finite");
    norm += values[i] * values[i];
  }
  norm = std::sqrt(norm);
  if (!(norm > 0) || !std::isfinite(norm))
    luaL_error(L, "rotation needs a nonzero finite quaternion");
  return {float(values[0] / norm), float(values[1] / norm), float(values[2] / norm),
          float(values[3] / norm)};
}
} // namespace amber::bindings
