#include "revision.hpp"

#include "bindings/arguments/arguments.hpp"
#include "bindings/structural/revision.hpp"

#include <lua.h>
#include <lualib.h>

namespace amber::bindings {
namespace {
void push_masses(lua_State *L, const std::map<TermId, double> &masses) {
  lua_createtable(L, 0, 2);
  lua_createtable(L, static_cast<int>(masses.size()), 0);
  int index = 1;
  double total = 0;
  for (const auto &[material, kg] : masses) {
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, material);
    lua_setfield(L, -2, "material");
    lua_pushnumber(L, kg);
    lua_setfield(L, -2, "mass_kg");
    lua_setreadonly(L, -1, true);
    lua_rawseti(L, -2, index++);
    total += kg;
  }
  lua_setreadonly(L, -1, true);
  lua_setfield(L, -2, "materials");
  lua_pushnumber(L, total);
  lua_setfield(L, -2, "mass_kg");
  lua_setreadonly(L, -1, true);
}
const catalog::Model &root_model(const catalog::Models &models, ModelId id) {
  if (!id || id >= models.rows.size() || models.rows[id].id != id ||
      !models.rows[id].part_name.empty())
    throw std::invalid_argument("model revision requires a root model");
  return models.rows[id];
}
} // namespace
int measure_model_change_binding(lua_State *L) {
  const auto &binding = *static_cast<ModelRevisionMeasure *>(react::binding_data(L));
  const auto model = model_arg(L, 2), proposal = model_arg(L, 3);
  try {
    const auto masses = measure_model_change(binding.read, model, proposal);
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, masses.count);
    lua_setfield(L, -2, "instances");
    push_masses(L, masses.before);
    lua_setfield(L, -2, "before");
    push_masses(L, masses.after);
    lua_setfield(L, -2, "after");
    lua_setreadonly(L, -1, true);
  } catch (const std::exception &error) {
    luaL_error(L, "%s", error.what());
  }
  return 1;
}
int revise_model_binding(lua_State *L) {
  const auto &binding = *static_cast<ModelRevisionQueue *>(react::binding_data(L));
  if (!react::may_construct(react::invocation(L)))
    luaL_error(L, "model revision requires approved active world-rule dispatch");
  const auto model = model_arg(L, 2), proposal = model_arg(L, 3);
  std::size_t length = 0;
  const auto *source = luaL_checklstring(L, 4, &length);
  try {
    const auto &current = root_model(binding.models, model);
    (void)root_model(binding.models, proposal);
    if (model == proposal)
      throw std::invalid_argument("revision_model must be a separate baked submission");
    const std::string expected(source, length);
    if (!catalog::valid_hash(expected))
      throw std::invalid_argument("expected_source_hash must be a SHA-256 hash");
    if (current.source_hash != expected)
      throw std::invalid_argument("model source changed; inspect and rebake this revision");
    react::StructuralRequest request;
    request.kind = react::StructuralRequest::Kind::ReviseModel;
    request.revision = react::StructuralRequest::ModelRevision{model, proposal, expected};
    request.exchange = capture_exchange(L, 5, binding.properties, binding.processes, binding.terms,
                                        binding.exchange_max);
    if (!react::queue_structural(react::invocation(L), std::move(request)))
      throw std::logic_error("approved model revision was not queued");
    lua_createtable(L, 0, 1);
    lua_pushboolean(L, true);
    lua_setfield(L, -2, "queued");
  } catch (const std::exception &error) {
    luaL_error(L, "%s", error.what());
  }
  return 1;
}
} // namespace amber::bindings
