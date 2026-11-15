#include "construction.hpp"
#include "bindings/arguments/arguments.hpp"

#include <algorithm>
#include <lua.h>
#include <lualib.h>

namespace amber::bindings {
namespace {
react::StructuralRequest::Binding binding(lua_State *L, const ConstructionBinding &b,
                                          int model_index, int params_index, int material_index) {
  const auto model = model_arg(L, model_index);
  const auto params = lua_isnoneornil(L, params_index) ? 0 : unsigned_arg(L, params_index, true);
  try {
    (void)catalog::resolve_asset(b.models, b.parameters, model, params);
  } catch (const std::exception &error) {
    luaL_error(L, "%s", error.what());
  }
  const auto material = lua_isnoneornil(L, material_index) ? b.models.rows[model].material
                                                           : term_arg(L, material_index, b.terms);
  if (b.terms.rows[material].kind != react::Kind::Material)
    luaL_error(L, "binding material is not a material");
  return {model, material, params};
}
Handle target(lua_State *L, int index) {
  const auto object = react::check_handle(L, index);
  const auto &inv = react::invocation(L);
  if (object != inv.self.object &&
      std::find(inv.acquired.begin(), inv.acquired.end(), object) == inv.acquired.end() &&
      std::find(inv.parts.begin(), inv.parts.end(), object) == inv.parts.end())
    luaL_error(L, "structural target was not acquired");
  return object;
}
int queued(lua_State *L, react::StructuralRequest request) {
  if (!react::queue_structural(react::invocation(L), std::move(request)))
    luaL_error(L, "structural creation/replacement requires approved active world-rule dispatch");
  lua_createtable(L, 0, 1);
  lua_pushboolean(L, true);
  lua_setfield(L, -2, "queued");
  return 1;
}
void world_rule(lua_State *L) {
  if (!react::may_construct(react::invocation(L)))
    luaL_error(L, "creation/replacement requires approved active world-rule dispatch");
}
} // namespace
int create_binding(lua_State *L) {
  const auto &b = *static_cast<ConstructionBinding *>(react::binding_data(L));
  world_rule(L);
  react::StructuralRequest request;
  request.kind = react::StructuralRequest::Kind::Create;
  request.binding = binding(L, b, 2, 3, 4);
  request.position = vector_arg(L, 5);
  request.rotation = rotation_arg(L, 6);
  request.exchange = capture_exchange(L, 7, b.properties, b.processes, b.terms, b.exchange_max);
  return queued(L, std::move(request));
}
int remove_binding(lua_State *L) {
  const auto &b = *static_cast<RemovalBinding *>(react::binding_data(L));
  const auto object = target(L, 2);
  if (!b.identity.valid(object))
    luaL_error(L, "structural target is no longer alive");
  react::StructuralRequest request;
  request.kind = react::StructuralRequest::Kind::Destroy;
  request.target = object;
  request.exchange = capture_exchange(L, 3, b.properties, b.processes, b.terms, b.exchange_max);
  return queued(L, std::move(request));
}
int replace_binding(lua_State *L) {
  const auto &b = *static_cast<ConstructionBinding *>(react::binding_data(L));
  world_rule(L);
  react::StructuralRequest request;
  request.kind = react::StructuralRequest::Kind::ReplaceBinding;
  request.target = target(L, 2);
  request.binding = binding(L, b, 3, 4, 5);
  request.exchange = capture_exchange(L, 6, b.properties, b.processes, b.terms, b.exchange_max);
  return queued(L, std::move(request));
}
} // namespace amber::bindings
