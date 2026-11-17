#include "react.hpp"

#include <Luau/Allocator.h>
#include <Luau/Ast.h>
#include <Luau/Compiler.h>
#include <Luau/Parser.h>
#include <lua.h>
#include <lualib.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace amber::react {
namespace {
constexpr int handle_tag = 1, state_tag = 2, iterator_tag = 3;
struct StateBinding {
  IdentityRead identity;
  react::State &react_state;
  const Terms &terms;
};
struct RelationBinding {
  const RelationsRead &relations;
  const Terms &terms;
};
struct ContributionBinding {
  IdentityRead identity;
  SharedColumns &shared;
  GivesTo &gives_to;
  const Terms &terms;
  const RelationsRead &relations;
};
int get_binding(lua_State *);
int setting_binding(lua_State *);
int set_binding(lua_State *);
int rate_binding(lua_State *);
int related_binding(lua_State *);
int contribute_binding(lua_State *);
int share_binding(lua_State *);
struct Binding {
  lua_CFunction function;
  void *data;
  std::shared_ptr<void> owned{};
};
struct EventBinding {
  const Terms &terms;
};
struct RunContext {
  Invocation *invocation = nullptr;
  std::size_t state_limit = 0;
  const std::size_t *heap_bytes = nullptr;
  std::size_t event_fields_max = 0;
  lua_State *thread = nullptr;
  int thread_ref = 0;
  std::optional<RunOutcome::Action> action{};
  const nlohmann::json *result = nullptr;
  const std::map<std::string, Binding> *bindings = nullptr;
  const HandlerRef *handler = nullptr;
  int module_ref = 0;
  bool missing = false;
};
nlohmann::json metadata_json(lua_State *, int, std::size_t &, int = 0, bool host_values = false);
void push_result(lua_State *, const nlohmann::json &);
struct StateProxy {
  ScriptState *state;
};
struct StateIterator {
  std::vector<StateKey> keys;
  std::size_t index = 0;
  ScriptState *state;
  std::size_t *charged_to;
  std::size_t charged_bytes = 0;
};
struct alignas(std::max_align_t) AllocationHeader {
  std::size_t size;
  ScriptId owner;
};
struct Allocator {
  ScriptId current = 0;
  std::size_t limit = 0;
  std::unordered_map<ScriptId, std::size_t> used;
  const std::size_t *state_bytes = nullptr;
};
void *allocate_lua(void *ud, void *ptr, std::size_t, std::size_t size) {
  auto &allocator = *static_cast<Allocator *>(ud);
  AllocationHeader *old = ptr ? static_cast<AllocationHeader *>(ptr) - 1 : nullptr;
  const auto owner = old ? old->owner : allocator.current;
  const std::size_t previous = old ? old->size : 0;
  if (size == 0) {
    if (old) {
      allocator.used[owner] -= previous;
      std::free(old);
    }
    return nullptr;
  }
  auto &used = allocator.used[owner];
  if (owner && size > previous &&
      used + size - previous +
              (owner == allocator.current && allocator.state_bytes ? *allocator.state_bytes : 0) >
          allocator.limit)
    return nullptr;
  auto *header =
      static_cast<AllocationHeader *>(std::realloc(old, sizeof(AllocationHeader) + size));
  if (!header)
    return nullptr;
  used = used - previous + size;
  header->size = size;
  header->owner = owner;
  return header + 1;
}
RunContext &context(lua_State *L) {
  auto *run = static_cast<RunContext *>(lua_getthreaddata(L));
  if (!run || !run->invocation)
    luaL_error(L, "host call outside a handler");
  return *run;
}
void interrupt(lua_State *L, int gc) {
  if (gc >= 0)
    return;
  auto *run = static_cast<RunContext *>(lua_getthreaddata(L));
  if (run && run->invocation && run->invocation->deadline.passed())
    luaL_error(L, "handler deadline exceeded");
}
StateKey state_key(lua_State *L, int index) {
  if (lua_type(L, index) == LUA_TSTRING) {
    std::size_t size;
    const char *text = lua_tolstring(L, index, &size);
    return std::string(text, size);
  }
  if (lua_type(L, index) == LUA_TNUMBER) {
    const double value = lua_tonumber(L, index);
    if (std::isfinite(value) && std::trunc(value) == value && value >= -0x1p63 && value < 0x1p63)
      return static_cast<std::int64_t>(value);
  }
  luaL_error(L, "state key must be a string or integer");
}
ScriptState::Entries::iterator find_state(lua_State *L, ScriptState &state, int index) {
  if (lua_type(L, index) == LUA_TSTRING) {
    std::size_t size;
    const char *text = lua_tolstring(L, index, &size);
    return state.values.find(StateStringKey{{text, size}});
  }
  return state.values.find(state_key(L, index));
}
StateAtom state_atom(lua_State *L, int index) {
  switch (lua_type(L, index)) {
  case LUA_TNUMBER:
    return lua_tonumber(L, index);
  case LUA_TBOOLEAN:
    return lua_toboolean(L, index) != 0;
  case LUA_TSTRING: {
    std::size_t size;
    const char *text = lua_tolstring(L, index, &size);
    return std::string(text, size);
  }
  case LUA_TUSERDATA:
    if (lua_userdatatag(L, index) == handle_tag)
      return check_handle(L, index);
    break;
  }
  luaL_error(L, "state accepts numbers, booleans, strings and handles");
}
void push_atom(lua_State *L, const StateAtom &atom) {
  std::visit(
      [&](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, double>)
          lua_pushnumber(L, value);
        else if constexpr (std::is_same_v<T, bool>)
          lua_pushboolean(L, value);
        else if constexpr (std::is_same_v<T, std::string>)
          lua_pushlstring(L, value.data(), value.size());
        else
          push_handle(L, value);
      },
      atom);
}
void push_key(lua_State *L, const StateKey &key) {
  if (const auto *number = std::get_if<std::int64_t>(&key))
    lua_pushnumber(L, static_cast<double>(*number));
  else {
    const auto &text = std::get<std::string>(key);
    lua_pushlstring(L, text.data(), text.size());
  }
}
int state_index(lua_State *L) {
  auto *proxy = static_cast<StateProxy *>(luaL_checkudatatagged(L, 1, state_tag));
  context(L);
  auto found = find_state(L, *proxy->state, 2);
  if (found == proxy->state->values.end())
    lua_pushnil(L);
  else
    push_atom(L, found->second);
  return 1;
}
int state_newindex(lua_State *L) {
  auto *proxy = static_cast<StateProxy *>(luaL_checkudatatagged(L, 1, state_tag));
  auto &run = context(L);
  auto found = find_state(L, *proxy->state, 2);
  if (lua_isnil(L, 3)) {
    if (found != proxy->state->values.end()) {
      proxy->state->erase(found);
      run.invocation->effects->changed = true;
    }
    return 0;
  }
  const std::size_t previous =
      found == proxy->state->values.end() ? 0 : state_entry_bytes(found->first, found->second);
  std::optional<StateKey> key;
  if (found == proxy->state->values.end())
    key = state_key(L, 2);
  auto atom = state_atom(L, 3);
  const auto next = state_entry_bytes(key ? *key : found->first, atom);
  if (proxy->state->bytes - previous + next + (run.heap_bytes ? *run.heap_bytes : 0) >
      run.state_limit)
    luaL_error(L, "script state memory limit exceeded");
  if (found == proxy->state->values.end() || found->second != atom)
    run.invocation->effects->changed = true;
  if (found == proxy->state->values.end())
    proxy->state->insert(std::move(*key), std::move(atom));
  else
    proxy->state->replace(found, std::move(atom));
  return 0;
}
void destroy_iterator(lua_State *, void *pointer) {
  auto *iterator = static_cast<StateIterator *>(pointer);
  *iterator->charged_to -= iterator->charged_bytes;
  iterator->~StateIterator();
}
int state_next(lua_State *L) {
  context(L);
  auto *iterator = static_cast<StateIterator *>(lua_touserdata(L, lua_upvalueindex(1)));
  while (iterator->index < iterator->keys.size()) {
    auto &key = iterator->keys[iterator->index++];
    auto found = iterator->state->values.find(key);
    if (found == iterator->state->values.end())
      continue;
    push_key(L, key);
    push_atom(L, found->second);
    return 2;
  }
  return 0;
}
int state_iter(lua_State *L) {
  auto *proxy = static_cast<StateProxy *>(luaL_checkudatatagged(L, 1, state_tag));
  context(L);
  std::size_t snapshot_bytes = 0;
  for (const auto &[key, value] : proxy->state->values) {
    (void)value;
    snapshot_bytes += state_key_bytes(key);
  }
  void *userdata = nullptr;
  lua_getallocf(L, &userdata);
  auto &allocator = *static_cast<Allocator *>(userdata);
  auto &charged = allocator.used[allocator.current];
  if (charged + proxy->state->bytes + snapshot_bytes + sizeof(StateIterator) > allocator.limit)
    luaL_error(L, "state iterator memory limit exceeded");
  auto *iterator = new (lua_newuserdatatagged(L, sizeof(StateIterator), iterator_tag))
      StateIterator{{}, 0, proxy->state, &charged, snapshot_bytes};
  charged += snapshot_bytes;
  iterator->keys.reserve(proxy->state->values.size());
  for (auto &[key, value] : proxy->state->values) {
    (void)value;
    iterator->keys.push_back(key);
  }
  lua_pushcclosure(L, state_next, "state iterator", 1);
  lua_pushnil(L);
  lua_pushnil(L);
  return 3;
}
int state_length(lua_State *L) {
  auto *proxy = static_cast<StateProxy *>(luaL_checkudatatagged(L, 1, state_tag));
  context(L);
  std::int64_t length = 0;
  while (proxy->state->values.contains(StateKey{length + 1}))
    ++length;
  lua_pushnumber(L, static_cast<double>(length));
  return 1;
}
int handle_equal(lua_State *L) {
  lua_pushboolean(L, check_handle(L, 1) == check_handle(L, 2));
  return 1;
}
int handle_string(lua_State *L) {
  auto text = to_string(check_handle(L, 1));
  lua_pushlstring(L, text.data(), text.size());
  return 1;
}
void push_value(lua_State *L, Value value) {
  switch (value.kind) {
  case Value::Kind::None:
    lua_pushnil(L);
    break;
  case Value::Kind::Number:
    lua_pushnumber(L, value.number);
    break;
  case Value::Kind::Integer:
    lua_pushnumber(L, static_cast<double>(value.integer));
    break;
  case Value::Kind::Boolean:
    lua_pushboolean(L, value.boolean);
    break;
  case Value::Kind::Object:
    push_handle(L, value.object);
    break;
  }
}
Value check_value(lua_State *L, int index) {
  if (lua_isboolean(L, index))
    return Value(lua_toboolean(L, index) != 0);
  if (lua_isnumber(L, index))
    return Value(lua_tonumber(L, index));
  if (lua_type(L, index) == LUA_TUSERDATA && lua_userdatatag(L, index) == handle_tag)
    return Value(check_handle(L, index));
  luaL_error(L, "expected a scalar value or handle");
}
void push_payload(lua_State *L, const EventPayload &payload) {
  auto push_event_atom = [&](const EventAtom &atom) {
    std::visit(
        [&](const auto &v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, Value>)
            push_value(L, v);
          else if constexpr (std::is_same_v<T, Vec3>)
            lua_pushvector(L, v.x, v.y, v.z);
          else if constexpr (std::is_same_v<T, std::string>)
            lua_pushlstring(L, v.data(), v.size());
          else
            lua_pushinteger(L, static_cast<int>(v.id));
        },
        atom);
  };
  std::visit(
      [&](const auto &v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Value>)
          push_value(L, v);
        else if constexpr (std::is_same_v<T, Vec3>)
          lua_pushvector(L, v.x, v.y, v.z);
        else if constexpr (std::is_same_v<T, std::string>)
          lua_pushlstring(L, v.data(), v.size());
        else {
          lua_createtable(L, 0, static_cast<int>(v.size()));
          for (auto &field : v) {
            push_event_atom(field.value);
            lua_setfield(L, -2, field.name.c_str());
          }
          lua_setreadonly(L, -1, 1);
        }
      },
      payload);
}
EventPayload check_payload(lua_State *L, int index) {
  switch (lua_type(L, index)) {
  case LUA_TNONE:
  case LUA_TNIL:
    return Value{};
  case LUA_TBOOLEAN:
  case LUA_TNUMBER:
  case LUA_TUSERDATA:
    return check_value(L, index);
  case LUA_TVECTOR: {
    const auto *v = lua_tovector(L, index);
    return Vec3{v[0], v[1], v[2]};
  }
  case LUA_TSTRING: {
    std::size_t size;
    const char *text = lua_tolstring(L, index, &size);
    return std::string(text, size);
  }
  case LUA_TTABLE: {
    std::vector<EventField> fields;
    lua_pushnil(L);
    const int table = index > 0 ? index : lua_gettop(L) + index;
    while (lua_next(L, table)) {
      if (fields.size() >= context(L).event_fields_max)
        luaL_error(L, "event record has too many fields");
      if (lua_type(L, -2) != LUA_TSTRING)
        luaL_error(L, "event field names must be strings");
      if (lua_istable(L, -1))
        luaL_error(L, "event records must be flat");
      std::size_t size;
      const char *name = luaL_checklstring(L, -2, &size);
      auto payload = check_payload(L, -1);
      EventAtom atom = std::visit(
          [&](auto &&v) -> EventAtom {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::vector<EventField>>)
              luaL_error(L, "event records must be flat");
            else
              return std::forward<decltype(v)>(v);
          },
          std::move(payload));
      fields.push_back({std::string(name, size), std::move(atom)});
      lua_pop(L, 1);
    }
    return fields;
  }
  }
  luaL_error(L, "unsupported event payload");
}
TermId term_arg(lua_State *L, int index, const Terms &terms) {
  if (lua_type(L, index) == LUA_TNUMBER) {
    const auto id = luaL_checkunsigned(L, index);
    if (id > 0 && id < terms.rows.size())
      return id;
  } else if (lua_type(L, index) == LUA_TSTRING) {
    const auto *name = lua_tostring(L, index);
    auto it = terms.by_name.find(name);
    if (it != terms.by_name.end())
      return it->second;
  }
  luaL_error(L, "unknown term");
}
bool acquired(const Invocation &inv, Handle target) {
  return target == inv.self.object ||
         std::find(inv.acquired.begin(), inv.acquired.end(), target) != inv.acquired.end() ||
         std::find(inv.parts.begin(), inv.parts.end(), target) != inv.parts.end();
}
int now_method(lua_State *L) {
  auto &inv = invocation(L);
  lua_pushnumber(L, static_cast<double>(inv.step) / inv.hz);
  return 1;
}
int random_method(lua_State *L) {
  auto &inv = invocation(L);
  if (!inv.random)
    luaL_error(L, "random state is not bound");
  auto &state = *inv.random;
  state += 0x9e3779b97f4a7c15ULL;
  auto value = state;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  value ^= value >> 31;
  lua_pushnumber(L, static_cast<double>(value >> 11) * 0x1p-53);
  return 1;
}
int parts_method(lua_State *L) {
  auto &inv = invocation(L);
  lua_createtable(L, static_cast<int>(inv.parts.size()), 0);
  int i = 1;
  for (auto h : inv.parts) {
    push_handle(L, h);
    lua_rawseti(L, -2, i++);
  }
  return 1;
}
// Core event methods resolve term names using a read-only registry supplied by register_binding.
int emit_method(lua_State *L) {
  auto &inv = invocation(L);
  const auto *terms = &static_cast<const EventBinding *>(binding_data(L))->terms;
  if (!terms)
    luaL_error(L, "event registry is not bound");
  const auto target = check_handle(L, 2);
  const auto event = term_arg(L, 3, *terms);
  if (terms->rows[event].kind != Kind::Event)
    luaL_error(L, "emit needs an event term");
  if (target != inv.self.object && std::ranges::find(inv.parts, target) == inv.parts.end() &&
      std::ranges::find(inv.event_targets, target) == inv.event_targets.end())
    luaL_error(L, "event target must be self, a declared part, related, or from this event");
  inv.effects->ordered.push_back(
      react::EventWork{target, inv.self.object, event, check_payload(L, 4), {}});
  return 0;
}
int after_method(lua_State *L) {
  auto &inv = invocation(L);
  const auto *terms = &static_cast<const EventBinding *>(binding_data(L))->terms;
  if (!terms)
    luaL_error(L, "event registry is not bound");
  const auto seconds = luaL_checknumber(L, 2);
  if (seconds < 0 || !std::isfinite(seconds) ||
      seconds > static_cast<double>(std::numeric_limits<Step>::max() - inv.step) / inv.hz)
    luaL_error(L, "invalid timer delay");
  const auto event = term_arg(L, 3, *terms);
  if (terms->rows[event].kind != Kind::Event)
    luaL_error(L, "after needs an event term");
  react::Timer timer;
  timer.due = inv.step + std::max<Step>(1, static_cast<Step>(std::ceil(seconds * inv.hz)));
  timer.event = {inv.self.object, inv.self.object, event, check_payload(L, 4), {}};
  if (!inv.self.object)
    timer.event.recipient_script = inv.self.script;
  inv.effects->ordered.push_back(std::move(timer));
  return 0;
}
int act_method(lua_State *L) {
  auto &inv = invocation(L);
  const auto *terms = &static_cast<const EventBinding *>(binding_data(L))->terms;
  if (!terms)
    luaL_error(L, "action registry is not bound");
  const auto action = term_arg(L, 2, *terms);
  if (terms->rows[action].kind != Kind::Action)
    luaL_error(L, "act needs an action term");
  std::size_t nodes = 0;
  nlohmann::json arguments;
  try {
    arguments =
        lua_isnoneornil(L, 4) ? nlohmann::json::object() : metadata_json(L, 4, nodes, 0, true);
  } catch (const std::runtime_error &error) {
    luaL_error(L, "%s", error.what());
  }
  if (arguments.is_array() && arguments.empty())
    arguments = nlohmann::json::object();
  if (!arguments.is_object())
    luaL_error(L, "action args must be an input record");
  context(L).action = RunOutcome::Action{{ScriptHolder{inv.self.script, inv.self.object},
                                          action,
                                          lua_isnoneornil(L, 3) ? Handle{} : check_handle(L, 3),
                                          {}},
                                         std::move(arguments)};
  return lua_yield(L, 0);
}
int act_result(lua_State *L, int) {
  push_result(L, *context(L).result);
  return 1;
}
int handler_done(lua_State *, int) {
  return 0;
}
int handler_entry(lua_State *L) {
  auto &run = context(L);
  auto &inv = *run.invocation;
  lua_getref(L, run.module_ref);
  lua_getfield(L, -1, "on");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, run.handler->function.c_str());
    lua_remove(L, -2);
    lua_remove(L, -2);
  } else {
    lua_pop(L, 1);
    lua_getfield(L, -1, run.handler->function.c_str());
    lua_remove(L, -2);
  }
  if (!lua_isfunction(L, -1)) {
    run.missing = true;
    return 0;
  }
  lua_createtable(L, 0, static_cast<int>(run.bindings->size() + 2));
  if (inv.self.object)
    push_handle(L, inv.self.object);
  else
    lua_pushnil(L);
  lua_setfield(L, -2, "handle");
  lua_pushunsigned(L, inv.self.script);
  lua_setfield(L, -2, "script");
  auto *proxy = static_cast<StateProxy *>(
      lua_newuserdatataggedwithmetatable(L, sizeof(StateProxy), state_tag));
  *proxy = {inv.state};
  lua_setfield(L, -2, "state");
  for (const auto &[name, binding] : *run.bindings) {
    if (name == "world.near" || name == "world.raycast")
      continue;
    lua_pushlightuserdata(L, binding.data);
    lua_pushcclosurek(L, binding.function, name.c_str(), 1,
                      binding.function == act_method ? act_result : nullptr);
    lua_setfield(L, -2, name.c_str());
  }
  lua_setreadonly(L, -1, 1);
  push_handle(L, inv.sender);
  push_payload(L, inv.payload);
  if (!inv.action_caller) {
    lua_pushnil(L);
  } else if (const auto *player = std::get_if<PlayerId>(&*inv.action_caller)) {
    const auto name = "p:" + std::to_string(*player);
    lua_pushlstring(L, name.data(), name.size());
  } else {
    const auto &script = std::get<ScriptHolder>(*inv.action_caller);
    lua_createtable(L, 0, 2);
    const auto name = "s:" + std::to_string(script.script);
    lua_pushlstring(L, name.data(), name.size());
    lua_setfield(L, -2, "script");
    if (script.object)
      push_handle(L, script.object);
    else
      lua_pushnil(L);
    lua_setfield(L, -2, "on");
    lua_setreadonly(L, -1, 1);
  }
  return lua_callyieldable(L, 4, 0);
}
struct WriteCheck : Luau::AstVisitor {
  std::vector<std::string> &errors;
  explicit WriteCheck(std::vector<std::string> &errors) : errors(errors) {}
  void check(Luau::AstExpr *expr) {
    while (true) {
      if (auto *group = expr->as<Luau::AstExprGroup>())
        expr = group->expr;
      else if (auto *assertion = expr->as<Luau::AstExprTypeAssertion>())
        expr = assertion->expr;
      else
        break;
    }
    if (auto *local = expr->as<Luau::AstExprLocal>(); local && local->upvalue)
      errors.push_back("assignment to a captured local is forbidden; use self.state");
    if (expr->is<Luau::AstExprGlobal>())
      errors.push_back("global assignment is forbidden; use a local");
  }
  bool visit(Luau::AstStatAssign *node) override {
    for (auto *var : node->vars)
      check(var);
    return true;
  }
  bool visit(Luau::AstStatCompoundAssign *node) override {
    check(node->var);
    return true;
  }
  bool visit(Luau::AstStatFunction *node) override {
    check(node->name);
    return true;
  }
};
std::vector<std::string> validate_ast(std::string_view source) {
  Luau::Allocator allocator;
  Luau::AstNameTable names(allocator);
  auto parsed = Luau::Parser::parse(source.data(), source.size(), names, allocator);
  std::vector<std::string> errors;
  for (auto &error : parsed.errors)
    errors.push_back(error.getMessage());
  if (parsed.root) {
    WriteCheck checker(errors);
    parsed.root->visit(&checker);
  }
  return errors;
}
void freeze(lua_State *L, int index, std::unordered_set<const void *> &visited) {
  index = lua_absindex(L, index);
  const int type = lua_type(L, index);
  if (type == LUA_TTABLE) {
    if (!visited.insert(lua_topointer(L, index)).second)
      return;
    lua_pushnil(L);
    while (lua_next(L, index)) {
      freeze(L, -2, visited);
      freeze(L, -1, visited);
      lua_pop(L, 1);
    }
    if (lua_getmetatable(L, index)) {
      freeze(L, -1, visited);
      lua_pop(L, 1);
    }
    lua_setreadonly(L, index, 1);
  } else if (type == LUA_TFUNCTION && !lua_iscfunction(L, index)) {
    if (!visited.insert(lua_topointer(L, index)).second)
      return;
    for (int n = 1; lua_getupvalue(L, index, n); ++n) {
      freeze(L, -1, visited);
      lua_pop(L, 1);
    }
  } else if (type == LUA_TBUFFER || type == LUA_TTHREAD || type == LUA_TCLASS ||
             type == LUA_TOBJECT ||
             (type == LUA_TUSERDATA && lua_userdatatag(L, index) != handle_tag))
    throw std::runtime_error("captured mutable value is forbidden");
}
void cfunction(lua_State *L, const char *name, lua_CFunction fn) {
  lua_pushcfunction(L, fn, name);
  lua_setfield(L, -2, name);
}
void metatable(lua_State *L, int tag,
               std::initializer_list<std::pair<const char *, lua_CFunction>> methods) {
  lua_newtable(L);
  for (auto &[name, fn] : methods)
    cfunction(L, name, fn);
  lua_pushstring(L, "locked");
  lua_setfield(L, -2, "__metatable");
  lua_setreadonly(L, -1, 1);
  lua_setuserdatametatable(L, tag);
}
} // namespace
struct Vm::Impl {
  struct Script {
    lua_State *thread;
    int thread_ref, module_ref;
    ScriptDeclaration declaration;
  };
  Allocator allocator;
  VmSettings settings;
  lua_State *main = nullptr;
  std::unordered_map<ScriptId, Script> scripts;
  std::map<std::string, Binding> bindings;
  std::vector<std::unique_ptr<RunContext>> calls;
  RunOutcome advance(Invocation &);
  explicit Impl(VmSettings supplied) : settings(supplied) {
    allocator.limit = settings.script_kb * 1024;
    main = lua_newstate(allocate_lua, &allocator);
    if (!main)
      throw std::bad_alloc();
    luaL_openlibs(main);
    for (const char *name :
         {"getfenv", "setfenv", "loadstring", "load", "require", "collectgarbage", "newproxy",
          "print", "warn", "os", "io", "debug", "coroutine"}) {
      lua_pushnil(main);
      lua_setglobal(main, name);
    }
    lua_getglobal(main, "math");
    for (const char *name : {"random", "randomseed"}) {
      lua_pushnil(main);
      lua_setfield(main, -2, name);
    }
    lua_pop(main, 1);
    metatable(main, handle_tag, {{"__eq", handle_equal}, {"__tostring", handle_string}});
    metatable(main, state_tag,
              {{"__index", state_index},
               {"__newindex", state_newindex},
               {"__iter", state_iter},
               {"__len", state_length}});
    lua_setuserdatadtor(main, iterator_tag, destroy_iterator);
    luaL_sandbox(main);
    lua_callbacks(main)->interrupt = interrupt;
    bindings.emplace("now", Binding{now_method, nullptr});
    bindings.emplace("random", Binding{random_method, nullptr});
    bindings.emplace("parts", Binding{parts_method, nullptr});
  }
  ~Impl() { lua_close(main); }
};
Vm::Vm(VmSettings settings) : impl_(std::make_unique<Impl>(settings)) {}
Vm::~Vm() = default;
void register_binding(Vm &vm, std::string_view name, int (*function)(lua_State *),
                      std::shared_ptr<void> local) {
  if (!vm.impl_->scripts.empty())
    throw std::logic_error("register bindings before loading scripts");
  vm.impl_->bindings[std::string(name)] = {function, local.get(), std::move(local)};
}
void bind_values(Vm &vm, const ValueRead &values, State &react_state) {
  auto &shared = react_state.shared;
  auto &gives_to = react_state.gives_to;
  const auto read = std::make_shared<ValueRead>(values);
  const auto state =
      std::make_shared<StateBinding>(StateBinding{values.identity, react_state, values.terms});
  const auto relations =
      std::make_shared<RelationBinding>(RelationBinding{values.relations, values.terms});
  const auto contribution = std::make_shared<ContributionBinding>(
      ContributionBinding{values.identity, shared, gives_to, values.terms, values.relations});
  register_binding(vm, "get", get_binding, read);
  register_binding(vm, "setting", setting_binding, read);
  register_binding(vm, "get_share", share_binding, read);
  register_binding(vm, "set", set_binding, state);
  register_binding(vm, "rate", rate_binding, state);
  register_binding(vm, "related", related_binding, relations);
  register_binding(vm, "contribute", contribute_binding, contribution);
}
void bind_events(Vm &vm, const Terms &terms) {
  const auto events = std::make_shared<EventBinding>(EventBinding{terms});
  register_binding(vm, "emit", emit_method, events);
  register_binding(vm, "after", after_method, events);
  register_binding(vm, "act", act_method, events);
}
Invocation &invocation(lua_State *L) {
  return *context(L).invocation;
}
EventPayload check_event_payload(lua_State *L, int index) {
  return check_payload(L, index);
}
void *binding_data(lua_State *L) {
  return lua_tolightuserdata(L, lua_upvalueindex(1));
}
void push_handle(lua_State *L, Handle handle) {
  auto *value =
      static_cast<Handle *>(lua_newuserdatataggedwithmetatable(L, sizeof(Handle), handle_tag));
  *value = handle;
}
Handle check_handle(lua_State *L, int index) {
  return *static_cast<Handle *>(luaL_checkudatatagged(L, index, handle_tag));
}
CompiledScript compile_script(std::string_view source) {
  CompiledScript result;
  result.errors = validate_ast(source);
  if (!result.errors.empty())
    return result;
  Luau::CompileOptions options;
  options.optimizationLevel = 2;
  options.debugLevel = 1;
  result.bytecode = Luau::compile(std::string(source), options);
  if (result.bytecode.empty() || result.bytecode.front() == 0) {
    result.errors.push_back(result.bytecode.empty() ? "empty bytecode" : result.bytecode.substr(1));
    result.bytecode.clear();
  }
  return result;
}
ScriptValidation validate_script(std::string_view source, const Terms &terms,
                                 const ScriptDeclaration &declaration,
                                 const RightsDeclaration &rights) {
  ScriptValidation result;
  result.errors = validate_ast(source);
  auto term = [&](TermId id) -> const Term * {
    if (!id || id >= terms.rows.size()) {
      result.errors.push_back("unknown declared term");
      return nullptr;
    }
    return &terms.rows[id];
  };
  if (declaration.kind == ScriptDeclaration::Kind::World && !declaration.watches.empty())
    result.errors.push_back(
        "world scripts receive setting-change events and cannot declare object watches");
  for (const auto &watch : declaration.watches)
    if (auto *t = term(watch.term);
        t && t->kind != Kind::Quantity && t->kind != Kind::Flag && t->kind != Kind::Relation)
      result.errors.push_back("invalid watch kind");
  for (auto id : declaration.events)
    if (auto *t = term(id); t && t->kind != Kind::Event)
      result.errors.push_back("invalid event handler kind");
  for (auto id : declaration.handlers)
    if (auto *t = term(id); t && t->kind == Kind::Material)
      result.errors.push_back("material terms cannot be handler triggers");
  for (const auto &right : declaration.slots)
    if (std::find(rights.names.begin(), rights.names.end(), right) == rights.names.end() &&
        std::find(rights.slot_names.begin(), rights.slot_names.end(), right) ==
            rights.slot_names.end())
      result.errors.push_back("unknown requested right");
  for (auto id : declaration.contributes)
    if (auto *t = term(id); t && t->combine == Combine::None)
      result.errors.push_back("contribution needs a combine rule");
  for (auto id : declaration.sets)
    if (auto *t = term(id); t && (t->source != Source::State || t->combine != Combine::None ||
                                  (t->kind != Kind::Quantity && t->kind != Kind::Flag)))
      result.errors.push_back("set needs a state term");
  for (auto &action : declaration.actions) {
    if (auto *t = term(action.action); t && t->kind != Kind::Action)
      result.errors.push_back("invalid action kind");
    if (declaration.kind == ScriptDeclaration::Kind::World) {
      if (!action.right.empty() || action.reach)
        result.errors.push_back("world actions cannot require an object right or reach");
    } else if (std::find(rights.names.begin(), rights.names.end(), action.right) ==
               rights.names.end())
      result.errors.push_back("unknown action right");
  }
  if (declaration.kind == ScriptDeclaration::Kind::Rule &&
      (declaration.selector.empty() || declaration.tests == 0))
    result.errors.push_back("a rule needs a selector and tests");
  return result;
}
RunOutcome load_script(Vm &vm, const CompiledScript &compiled,
                       const ScriptDeclaration &declaration) {
  auto &impl = *vm.impl_;
  if (!compiled.errors.empty() || compiled.bytecode.empty())
    return {RunOutcome::Status::Error, "script did not compile", 0};
  if (impl.scripts.contains(declaration.id))
    return {RunOutcome::Status::Error, "script ID already loaded", 0};
  impl.allocator.current = declaration.id;
  auto *L = lua_newthread(impl.main);
  const auto thread_ref = lua_ref(impl.main, -1);
  lua_pop(impl.main, 1);
  luaL_sandboxthread(L);
  lua_newtable(L);
  for (const auto name : {"near", "raycast"}) {
    const auto found = impl.bindings.find(std::string("world.") + name);
    if (found != impl.bindings.end()) {
      lua_pushlightuserdata(L, found->second.data);
      lua_pushcclosure(L, found->second.function, name, 1);
      lua_setfield(L, -2, name);
    }
  }
  lua_setreadonly(L, -1, true);
  lua_setglobal(L, "world");
  Invocation startup;
  startup.deadline = RunDeadline::after(impl.settings.declaration_ms);
  RunContext run{&startup, 0, 0};
  lua_setthreaddata(L, &run);
  const int loaded = luau_load(L, "script", compiled.bytecode.data(), compiled.bytecode.size(), 0);
  const int status = loaded ? loaded : lua_pcall(L, 0, 1, 0);
  RunOutcome result;
  result.status = RunOutcome::Status::Ok;
  if (status || !lua_istable(L, -1)) {
    result.status = RunOutcome::Status::Error;
    result.error = status ? lua_tostring(L, -1) : "script must return a table";
  } else {
    try {
      std::unordered_set<const void *> visited;
      freeze(L, -1, visited);
      lua_pushvalue(L, LUA_GLOBALSINDEX);
      freeze(L, -1, visited);
      lua_pop(L, 1);
      const int module_ref = lua_ref(L, -1);
      impl.scripts.emplace(declaration.id,
                           Vm::Impl::Script{L, thread_ref, module_ref, declaration});
    } catch (const std::exception &error) {
      result.status = RunOutcome::Status::Error;
      result.error = error.what();
    }
  }
  lua_setthreaddata(L, nullptr);
  lua_settop(L, 0);
  impl.allocator.current = 0;
  result.memory_bytes = impl.allocator.used[declaration.id];
  if (result.status != RunOutcome::Status::Ok)
    lua_unref(impl.main, thread_ref);
  return result;
}
RunOutcome Vm::Impl::advance(Invocation &inv) {
  auto &run = *calls.back();
  assert(run.invocation == &inv);
  auto *L = run.thread;
  const auto script = inv.self.script;
  allocator.current = script;
  allocator.state_bytes = inv.state ? &inv.state->bytes : nullptr;
  int status = LUA_ERRRUN;
  if (inv.state)
    status = lua_resume(L, nullptr, 0);
  RunOutcome result;
  result.memory_bytes = allocator.used[script] + (inv.state ? inv.state->bytes : 0);
  allocator.state_bytes = nullptr;
  allocator.current = 0;
  if (status == LUA_YIELD && run.action) {
    result.status = RunOutcome::Status::Action;
    result.action = std::move(run.action);
    run.action.reset();
    return result;
  }
  result.status = run.missing ? RunOutcome::Status::Missing : RunOutcome::Status::Ok;
  if (status) {
    if (!inv.state)
      result.error = "script state was retired during its action";
    else
      result.error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "script failure";
    result.status = inv.deadline.passed() ? RunOutcome::Status::Cutoff : RunOutcome::Status::Error;
    inv.effects->ordered.push_back(
        Diagnostic{result.status == RunOutcome::Status::Cutoff ? "cutoff" : "error", result.error});
  }
  lua_setthreaddata(L, nullptr);
  lua_resetthread(L);
  lua_unref(main, run.thread_ref);
  calls.pop_back();
  return result;
}
RunOutcome run_handler(Vm &vm, const HandlerRef &handler, Invocation &inv) {
  ZoneScopedN("Luau handler");
  ZoneValue(handler.script);
  auto &impl = *vm.impl_;
  const auto found = impl.scripts.find(handler.script);
  if (found == impl.scripts.end() || !handler.enabled)
    return {};
  auto &script = found->second;
  if (!script.declaration.active ||
      (script.declaration.kind == ScriptDeclaration::Kind::Rule && !script.declaration.approved))
    return {};
  if (!inv.state || !inv.effects)
    return {RunOutcome::Status::Error, "state and effects rows must be bound", 0};
  // Each synchronous act crosses a native frame. Preserve Luau's native-call
  // depth bound even though the callee uses a separate resumable thread.
  if (impl.calls.size() >= LUAI_MAXCCALLS)
    return {RunOutcome::Status::Error, "native action call stack overflow", 0};
  inv.self.script = handler.script;
  inv.declaration = &script.declaration;
  if (inv.deadline.wall == std::chrono::steady_clock::time_point::max())
    inv.deadline = RunDeadline::after(impl.settings.handler_ms);
  auto run = std::make_unique<RunContext>();
  run->invocation = &inv;
  run->state_limit = impl.allocator.limit;
  run->heap_bytes = &impl.allocator.used[handler.script];
  run->event_fields_max = impl.settings.event_fields_max;
  run->module_ref = script.module_ref;
  run->handler = &handler;
  run->bindings = &impl.bindings;
  run->thread = lua_newthread(impl.main);
  run->thread_ref = lua_ref(impl.main, -1);
  lua_pop(impl.main, 1);
  lua_setthreaddata(run->thread, run.get());
  lua_pushcclosurek(run->thread, handler_entry, "handler", 0, handler_done);
  impl.calls.push_back(std::move(run));
  return impl.advance(inv);
}
RunOutcome resume_handler(Vm &vm, Invocation &inv, const nlohmann::json &result) {
  auto &impl = *vm.impl_;
  auto &run = *impl.calls.back();
  assert(run.invocation == &inv);
  run.result = &result;
  return impl.advance(inv);
}

namespace {
nlohmann::json metadata_json(lua_State *L, int index, std::size_t &nodes, int depth,
                             bool host_values) {
  if (host_values && invocation(L).deadline.passed())
    luaL_error(L, "handler deadline exceeded");
  if (++nodes > 100000)
    throw std::runtime_error("declaration node limit exceeded");
  if (depth > 16)
    throw std::runtime_error("declaration nesting limit exceeded");
  index = lua_absindex(L, index);
  switch (lua_type(L, index)) {
  case LUA_TNIL:
    return nullptr;
  case LUA_TBOOLEAN:
    return lua_toboolean(L, index) != 0;
  case LUA_TNUMBER: {
    double value = lua_tonumber(L, index);
    if (!std::isfinite(value))
      throw std::runtime_error("declaration number must be finite");
    if (host_values && value >= -0x1p63 && value < 0x1p63 && std::trunc(value) == value)
      return static_cast<std::int64_t>(value);
    return value;
  }
  case LUA_TSTRING: {
    std::size_t size;
    const char *text = lua_tolstring(L, index, &size);
    return std::string(text, size);
  }
  case LUA_TVECTOR:
    if (host_values) {
      const float *v = lua_tovector(L, index);
      return nlohmann::json::array({v[0], v[1], v[2]});
    }
    throw std::runtime_error("declaration data must not contain vectors");
  case LUA_TUSERDATA:
    if (host_values && lua_userdatatag(L, index) == handle_tag)
      return to_string(check_handle(L, index));
    throw std::runtime_error("declaration data must not contain host values");
  case LUA_TTABLE: {
    bool array = true;
    std::size_t count = 0;
    lua_pushnil(L);
    while (lua_next(L, index)) {
      ++count;
      if (lua_type(L, -2) != LUA_TNUMBER || lua_tonumber(L, -2) < 1 ||
          std::trunc(lua_tonumber(L, -2)) != lua_tonumber(L, -2))
        array = false;
      lua_pop(L, 1);
      if (count > 100000)
        throw std::runtime_error("declaration is too large");
    }
    if (array && count == static_cast<std::size_t>(lua_objlen(L, index))) {
      auto result = nlohmann::json::array();
      for (std::size_t i = 1; i <= count; ++i) {
        lua_rawgeti(L, index, static_cast<int>(i));
        result.push_back(metadata_json(L, -1, nodes, depth + 1, host_values));
        lua_pop(L, 1);
      }
      return result;
    }
    auto result = nlohmann::json::object();
    lua_pushnil(L);
    while (lua_next(L, index)) {
      if (lua_type(L, -2) != LUA_TSTRING)
        throw std::runtime_error("declaration maps require string keys");
      std::string key = lua_tostring(L, -2);
      result[key] = metadata_json(L, -1, nodes, depth + 1, host_values);
      lua_pop(L, 1);
    }
    return result;
  }
  default:
    throw std::runtime_error("declaration data must not contain functions or host values");
  }
}
void push_result(lua_State *L, const nlohmann::json &value) {
  if (invocation(L).deadline.passed())
    luaL_error(L, "handler deadline exceeded");
  if (value.is_null())
    lua_pushnil(L);
  else if (value.is_boolean())
    lua_pushboolean(L, value.get<bool>());
  else if (value.is_number())
    lua_pushnumber(L, value.get<double>());
  else if (value.is_string()) {
    const auto &text = value.get_ref<const std::string &>();
    if (const auto handle = parse_handle(text)) {
      auto &inv = invocation(L);
      if (!acquired(inv, *handle))
        inv.acquired.push_back(*handle);
      push_handle(L, *handle);
    } else
      lua_pushlstring(L, text.data(), text.size());
  } else if (value.is_array()) {
    lua_createtable(L, static_cast<int>(value.size()), 0);
    int index = 1;
    for (const auto &entry : value) {
      push_result(L, entry);
      lua_rawseti(L, -2, index++);
    }
  } else {
    lua_createtable(L, 0, static_cast<int>(value.size()));
    for (const auto &[name, entry] : value.items()) {
      push_result(L, entry);
      lua_setfield(L, -2, name.c_str());
    }
  }
}
TermId named_term(const Terms &terms, const std::string &name) {
  auto found = terms.by_name.find(name);
  if (found == terms.by_name.end())
    throw std::runtime_error("unknown term: " + name);
  return found->second;
}
struct CallsCheck : Luau::AstVisitor {
  const Terms &terms;
  ScriptDeclaration &declaration;
  std::vector<std::string> &errors;
  CallsCheck(const Terms &terms, ScriptDeclaration &declaration, std::vector<std::string> &errors)
      : terms(terms), declaration(declaration), errors(errors) {}
  bool visit(Luau::AstExprCall *call) override {
    auto *method = call->func->as<Luau::AstExprIndexName>();
    if (!method || !call->self)
      return true;
    const std::string name = method->index.value;
    int index = -1;
    if (name == "set" || name == "rate" || name == "get" || name == "setting" || name == "act" ||
        name == "related")
      index = 0;
    if (name == "contribute" || name == "get_share" || name == "emit" || name == "after")
      index = 1;
    if (index < 0 || static_cast<std::size_t>(index) >= call->args.size)
      return true;
    auto *literal = call->args.data[index]->as<Luau::AstExprConstantString>();
    if (!literal) {
      if ((name == "set" || name == "rate") && declaration.sets.empty())
        errors.push_back("dynamic state terms require an explicit sets declaration");
      return true;
    }
    std::string spelling(literal->value.data, literal->value.size);
    auto found = terms.by_name.find(spelling);
    if (found == terms.by_name.end()) {
      errors.push_back("unknown host-call term: " + spelling);
      return true;
    }
    auto id = found->second;
    const auto &term = terms.rows[id];
    if (name == "set" || name == "rate") {
      if (std::find(declaration.sets.begin(), declaration.sets.end(), id) == declaration.sets.end())
        declaration.sets.push_back(id);
    }
    if (name == "contribute" &&
        std::find(declaration.contributes.begin(), declaration.contributes.end(), id) ==
            declaration.contributes.end())
      errors.push_back("contribution term not declared: " + spelling);
    if ((name == "emit" || name == "after") && term.kind != Kind::Event)
      errors.push_back("event call has wrong term kind: " + spelling);
    if (name == "act" && term.kind != Kind::Action)
      errors.push_back("act has wrong term kind: " + spelling);
    if (name == "related" && term.kind != Kind::Relation)
      errors.push_back("related has wrong term kind: " + spelling);
    return true;
  }
};
} // namespace
ScriptAnalysis analyze_script(std::string_view source, const Terms &terms,
                              const RightsDeclaration &rights, VmSettings settings) {
  ScriptAnalysis result;
  result.compiled = compile_script(source);
  result.errors = result.compiled.errors;
  if (!result.errors.empty())
    return result;
  Vm vm(settings);
  ScriptDeclaration provisional;
  provisional.id = 1;
  auto loaded = load_script(vm, result.compiled, provisional);
  if (loaded.status != RunOutcome::Status::Ok) {
    result.errors.push_back(loaded.error);
    return result;
  }
  auto *L = vm.impl_->scripts.at(1).thread;
  lua_getref(L, vm.impl_->scripts.at(1).module_ref);
  try {
    auto &data = result.metadata;
    std::size_t metadata_nodes = 0;
    for (const char *field :
         {"kind", "applies_to", "watches", "contributes", "sets", "slots", "actions", "tests"}) {
      lua_getfield(L, -1, field);
      if (!lua_isnil(L, -1))
        data[field] = metadata_json(L, -1, metadata_nodes);
      lua_pop(L, 1);
    }
    auto &d = result.declaration;
    const auto kind = data.value("kind", std::string{});
    if (kind == "model")
      d.kind = ScriptDeclaration::Kind::Model;
    else if (kind == "instance")
      d.kind = ScriptDeclaration::Kind::Instance;
    else if (kind == "rule")
      d.kind = ScriptDeclaration::Kind::Rule;
    else if (kind == "world")
      d.kind = ScriptDeclaration::Kind::World;
    else
      throw std::runtime_error("script kind must be model, instance, rule or world");
    d.active = d.kind != ScriptDeclaration::Kind::Rule;
    d.approved = false;
    if (data.contains("applies_to")) {
      const auto &selector = data["applies_to"];
      if (!selector.is_object() || selector.size() != 1)
        throw std::runtime_error("rule selector requires material or flag");
      const auto field = selector.begin().key();
      if (field != "material" && field != "flag")
        throw std::runtime_error("unknown selector kind");
      d.selector = selector.begin().value().get<std::string>();
      const auto id = named_term(terms, d.selector);
      if (terms.rows[id].kind != (field == "material" ? Kind::Material : Kind::Flag))
        throw std::runtime_error("selector term kind does not match");
    }
    auto names = [&](const char *field, std::vector<TermId> &out) {
      if (!data.contains(field))
        return;
      for (const auto &value : data[field])
        out.push_back(named_term(terms, value.get<std::string>()));
    };
    names("contributes", d.contributes);
    names("sets", d.sets);
    if (data.contains("slots"))
      d.slots = data["slots"].get<std::vector<std::string>>();
    if (data.contains("watches"))
      for (const auto &watch : data["watches"]) {
        WatchDeclaration declaration;
        if (watch.is_string())
          declaration.term = named_term(terms, watch.get<std::string>());
        else {
          for (const auto &[name, unused] : watch.items()) {
            (void)unused;
            if (name != "term" && name != "thresholds")
              throw std::runtime_error("unknown watch declaration field");
          }
          declaration.term = named_term(terms, watch.at("term").get<std::string>());
          if (watch.contains("thresholds"))
            for (const auto &entry : watch["thresholds"]) {
              Threshold t;
              auto predicate = entry.at("predicate").get<std::string>();
              if (predicate == "above")
                t.predicate = Threshold::Predicate::Above;
              else if (predicate == "at_least")
                t.predicate = Threshold::Predicate::AtLeast;
              else if (predicate == "below")
                t.predicate = Threshold::Predicate::Below;
              else if (predicate == "at_most")
                t.predicate = Threshold::Predicate::AtMost;
              else
                throw std::runtime_error("unknown threshold predicate");
              t.value = entry.at("value").get<double>();
              declaration.thresholds.push_back(t);
            }
        }
        d.watches.push_back(std::move(declaration));
      }
    if (data.contains("actions"))
      for (auto &[name, value] : data["actions"].items()) {
        if (value.is_array() && value.empty())
          value = nlohmann::json::object();
        d.actions.push_back({named_term(terms, name),
                             d.kind == ScriptDeclaration::Kind::World
                                 ? value.value("right", std::string{})
                                 : value.at("right").get<std::string>(),
                             name, value.value("reach", false)});
      }
    d.tests = data.contains("tests") ? data["tests"].size() : 0;
    lua_getfield(L, -1, "on");
    if (!lua_istable(L, -1))
      throw std::runtime_error("script needs an on table");
    std::unordered_set<TermId> handlers;
    lua_pushnil(L);
    data["handlers"] = nlohmann::json::array();
    while (lua_next(L, -2)) {
      if (lua_type(L, -2) != LUA_TSTRING || !lua_isfunction(L, -1))
        throw std::runtime_error("on maps registered term names to functions");
      std::string name = lua_tostring(L, -2);
      const auto id = named_term(terms, name);
      if (terms.rows[id].kind == Kind::Material)
        throw std::runtime_error("material terms cannot be handler triggers");
      handlers.insert(id);
      d.handlers.push_back(id);
      data["handlers"].push_back(name);
      if (terms.rows[id].kind == Kind::Event)
        d.events.push_back(id);
      lua_pop(L, 1);
    }
    std::sort(d.events.begin(), d.events.end());
    std::sort(d.handlers.begin(), d.handlers.end());
    std::sort(data["handlers"].begin(), data["handlers"].end());
    for (const auto &watch : d.watches)
      if (!handlers.contains(watch.term))
        throw std::runtime_error("watch has no handler");
    Luau::Allocator allocator;
    Luau::AstNameTable table(allocator);
    auto parsed = Luau::Parser::parse(source.data(), source.size(), table, allocator);
    CallsCheck calls(terms, d, result.errors);
    parsed.root->visit(&calls);
    auto validation = validate_script(source, terms, d, rights);
    result.errors.insert(result.errors.end(), validation.errors.begin(), validation.errors.end());
    data["sets"] = nlohmann::json::array();
    for (auto id : d.sets)
      data["sets"].push_back(terms.rows[id].name);
  } catch (const std::exception &error) {
    result.errors.push_back(error.what());
  }
  return result;
}
bool set_activation(Vm &vm, ScriptId script, bool approved, bool active) {
  auto found = vm.impl_->scripts.find(script);
  if (found == vm.impl_->scripts.end())
    return false;
  found->second.declaration.approved = approved;
  found->second.declaration.active = active;
  return true;
}
void consult_object(const Invocation &inv, Handle object) {
  auto &handles = inv.effects->consulted_handles;
  if (object && std::find(handles.begin(), handles.end(), object) == handles.end())
    handles.push_back(object);
}
namespace {
int get_binding(lua_State *L) {
  const auto &values = *static_cast<const ValueRead *>(binding_data(L));
  auto &inv = invocation(L);
  const auto id = term_arg(L, 2, values.terms);
  if (values.terms.rows[id].kind != Kind::Quantity && values.terms.rows[id].kind != Kind::Flag)
    luaL_error(L, "get needs a quantity or flag");
  if (!lua_isnoneornil(L, 3))
    luaL_error(L, "get reads this object only");
  const auto target = inv.self.object;
  if (!target)
    luaL_error(L, "get needs an object");
  consult_object(inv, target);
  auto value = read(values, target, id, inv.step, inv.hz);
  if (value.present)
    push_value(L, value.value);
  else
    lua_pushnil(L);
  return 1;
}
int setting_binding(lua_State *L) {
  const auto &values = *static_cast<const ValueRead *>(binding_data(L));
  const auto id = term_arg(L, 2, values.terms);
  if (values.terms.rows[id].source != Source::Setting)
    luaL_error(L, "term is not a setting");
  if (!invocation(L).self.object) {
    if (!lua_isnoneornil(L, 3))
      luaL_error(L, "setting reads this installation only");
    const auto found = values.world_settings.values.find(id);
    if (found == values.world_settings.values.end())
      lua_pushnil(L);
    else
      push_value(L, found->second);
    return 1;
  }
  return get_binding(L);
}
int set_binding(lua_State *L) {
  auto &binding = *static_cast<StateBinding *>(binding_data(L));
  auto &inv = invocation(L);
  const auto id = term_arg(L, 2, binding.terms);
  if (!inv.declaration || std::find(inv.declaration->sets.begin(), inv.declaration->sets.end(),
                                    id) == inv.declaration->sets.end())
    luaL_error(L, "state term was not declared");
  Handle target = inv.self.object;
  if (!lua_isnoneornil(L, 4)) {
    target = check_handle(L, 4);
    if (target != inv.self.object &&
        std::find(inv.parts.begin(), inv.parts.end(), target) == inv.parts.end())
      luaL_error(L, "state target is not an owned part");
  }
  if (!binding.identity.valid(target))
    luaL_error(L, "stale state target");
  try {
    set_state(binding.react_state.properties, binding.react_state.processes, binding.terms.rows[id],
              target, check_value(L, 3), inv.step, inv.hz, *inv.effects);
  } catch (const std::exception &e) {
    luaL_error(L, "%s", e.what());
  }
  return 0;
}
int rate_binding(lua_State *L) {
  auto &binding = *static_cast<StateBinding *>(binding_data(L));
  auto &inv = invocation(L);
  const auto id = term_arg(L, 2, binding.terms);
  if (!inv.declaration || std::find(inv.declaration->sets.begin(), inv.declaration->sets.end(),
                                    id) == inv.declaration->sets.end())
    luaL_error(L, "state term was not declared");
  Handle target = inv.self.object;
  if (!lua_isnoneornil(L, 6)) {
    target = check_handle(L, 6);
    if (target != inv.self.object &&
        std::find(inv.parts.begin(), inv.parts.end(), target) == inv.parts.end())
      luaL_error(L, "process target is not an owned part");
  }
  if (!binding.identity.valid(target))
    luaL_error(L, "stale process target");
  try {
    set_rate(binding.react_state, binding.terms.rows[id], target, luaL_checknumber(L, 3),
             luaL_checknumber(L, 4), luaL_checknumber(L, 5), inv.step, inv.hz, *inv.effects);
  } catch (const std::exception &e) {
    luaL_error(L, "%s", e.what());
  }
  return 0;
}
int related_binding(lua_State *L) {
  auto &binding = *static_cast<RelationBinding *>(binding_data(L));
  auto &inv = invocation(L);
  if (!inv.self.object)
    luaL_error(L, "related needs an object");
  const auto id = term_arg(L, 2, binding.terms);
  const auto *direction = luaL_checkstring(L, 3);
  if (std::strcmp(direction, "out") && std::strcmp(direction, "in"))
    luaL_error(L, "relation direction must be out or in");
  if (binding.terms.rows[id].kind != Kind::Relation)
    luaL_error(L, "term is not a relation");
  lua_newtable(L);
  auto found = binding.relations.columns.find(id);
  if (found == binding.relations.columns.end())
    return 1;
  const auto &column = std::strcmp(direction, "in") == 0 ? found->second->in : found->second->out;
  if (const auto *edges = column.get(inv.self.object.slot())) {
    int i = 1;
    for (auto handle : *edges) {
      push_handle(L, handle);
      lua_rawseti(L, -2, i++);
      if (!acquired(inv, handle))
        inv.acquired.push_back(handle);
      if (std::ranges::find(inv.event_targets, handle) == inv.event_targets.end())
        inv.event_targets.push_back(handle);
    }
  }
  return 1;
}
int contribute_binding(lua_State *L) {
  auto &binding = *static_cast<ContributionBinding *>(binding_data(L));
  auto &inv = invocation(L);
  if (!binding.identity.valid(inv.self.object))
    luaL_error(L, "contribution needs a live object");
  const auto subject = check_handle(L, 2);
  const auto id = term_arg(L, 3, binding.terms);
  const double amount = luaL_checknumber(L, 4);
  if (!inv.declaration ||
      std::find(inv.declaration->contributes.begin(), inv.declaration->contributes.end(), id) ==
          inv.declaration->contributes.end())
    luaL_error(L, "contribution term was not declared");
  // Retirement has already removed every contribution to a dead subject.
  if (amount == 0 && !binding.identity.valid(subject))
    return 0;
  bool allowed = subject == inv.self.object;
  for (const auto &[term, relation] : binding.relations.columns) {
    (void)term;
    if (const auto *edges = relation->out.get(inv.self.object.slot());
        edges && std::find(edges->begin(), edges->end(), subject) != edges->end())
      allowed = true;
    if (const auto *edges = relation->in.get(inv.self.object.slot());
        edges && std::find(edges->begin(), edges->end(), subject) != edges->end())
      allowed = true;
  }
  if (amount == 0)
    if (const auto *old = binding.gives_to.get(inv.self.object.slot()))
      if (std::find(old->begin(), old->end(), SubjectTerm{subject, id}) != old->end())
        allowed = true;
  if (!allowed)
    luaL_error(L, "contribution subject is not related");
  try {
    set_contribution(binding.shared, binding.gives_to, binding.terms.rows[id], inv.self.object,
                     subject, amount, *inv.effects);
  } catch (const std::exception &e) {
    luaL_error(L, "%s", e.what());
  }
  return 0;
}
int share_binding(lua_State *L) {
  const auto &values = *static_cast<const ValueRead *>(binding_data(L));
  auto &inv = invocation(L);
  if (!values.identity.valid(inv.self.object))
    luaL_error(L, "get_share needs a live object");
  const auto subject = check_handle(L, 2);
  const auto id = term_arg(L, 3, values.terms);
  const auto &term = values.terms.rows[id];
  if (term.combine != Combine::ShareEqual && term.combine != Combine::SharePriority)
    luaL_error(L, "get_share needs a shared-demand term");
  lua_pushnumber(L, live_share(values, subject, id, inv.self.object, inv.step, inv.hz));
  return 1;
}
} // namespace
} // namespace amber::react
