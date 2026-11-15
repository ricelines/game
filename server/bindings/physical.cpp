#include "physical.hpp"
#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace amber::bindings {
namespace {
using perm::Holder;
using perm::Reason;
using perm::Verb;
using react::Invocation;
const char *verb(Verb v) {
  constexpr const char *names[]{"use", "take", "move", "edit", "copy", "transfer", "destroy"};
  return names[static_cast<unsigned>(v)];
}
bool part_of(const Relation &parts, Handle root, Handle target) {
  if (root == target)
    return true;
  auto *parents = parts.in.get(target.slot());
  return parents && std::any_of(parents->begin(), parents->end(),
                                [&](Handle p) { return part_of(parts, root, p); });
}
bool own(const Invocation &i, const Relation &parts, Handle target) {
  return part_of(parts, i.self.object, target);
}
bool immediate(const Invocation &i, std::string_view primitive, IdentityRead id,
               AuthorityRead auth) {
  if (!i.direct_body)
    return false;
  const auto &b = *i.direct_body;
  return b.actor && b.body == i.self.object && id.valid(b.body) &&
         (id.flags[b.body.slot()] & Body) && auth.owner[b.body.slot()] == b.actor &&
         std::ranges::find(b.mechanics, primitive) != b.mechanics.end();
}
bool direct_scope(const Invocation &i, const Relation &parts, Handle target,
                  Handle live_held = {}) {
  const auto &b = *i.direct_body;
  return part_of(parts, b.body, target) || (b.selected && part_of(parts, b.selected, target)) ||
         (live_held && part_of(parts, live_held, target));
}
bool acquired(const Invocation &i, Handle h) {
  return h == i.self.object || std::ranges::find(i.parts, h) != i.parts.end() ||
         std::ranges::find(i.acquired, h) != i.acquired.end() ||
         (i.direct_body &&
          (h == i.direct_body->selected || h == i.direct_body->held || h == i.direct_body->body));
}
Handle handle(lua_State *L, int index, IdentityRead id) {
  auto h = react::check_handle(L, index);
  if (!id.valid(h))
    luaL_error(L, "stale object handle");
  if (!acquired(react::invocation(L), h))
    luaL_error(L, "object handle was not acquired");
  return h;
}
Handle live_held(const Invocation &i, IdentityRead identity, ParentRead parents,
                 const Relation &attached) {
  if (!i.direct_body)
    return {};
  auto h = i.direct_body->held;
  if (!h || !identity.valid(h) || !(identity.flags[h.slot()] & Held) ||
      parents.parent[h.slot()] != i.direct_body->body)
    return {};
  auto *list = attached.out.get(h.slot());
  return list && std::ranges::find(*list, i.direct_body->body) != list->end() ? h : Handle{};
}
perm::Decision right(const Invocation &i, std::string_view primitive, Handle target, Verb v,
                     IdentityRead id, AuthorityRead auth, const perm::OwnerLimits &limits,
                     const perm::Players &players, const perm::Grants &grants,
                     const Relation &parts, Handle held = {}) {
  auto script = Holder::script(i.self.script, i.self.object);
  bool self = own(i, parts, target);
  if (self && (v == Verb::Move || v == Verb::Edit))
    return {Reason::Allowed};
  auto normal =
      (id.flags[target.slot()] & Body) && v == Verb::Move
          ? perm::check_scope(script, perm::Scope::avatar(target), 1, grants, i.step)
          : perm::check(script, v, perm::target(id, auth, limits, target), players, grants, i.step);
  if (normal)
    return normal;
  if (immediate(i, primitive, id, auth) && direct_scope(i, parts, target, held)) {
    if ((id.flags[target.slot()] & Body) && v == Verb::Move &&
        auth.owner[target.slot()] != i.direct_body->actor)
      return {Reason::MissingRight};
    return perm::check(Holder::player(i.direct_body->actor), v,
                       perm::target(id, auth, limits, target), players, grants, i.step);
  }
  return normal;
}
int denied(lua_State *L, const char *need, Handle on, Reason why = Reason::MissingRight) {
  lua_createtable(L, 0, 4);
  lua_pushstring(L, "denied");
  lua_setfield(L, -2, "effect");
  lua_pushstring(L, need);
  lua_setfield(L, -2, "need");
  react::push_handle(L, on);
  lua_setfield(L, -2, "on");
  auto text = perm::reason_text(why);
  lua_pushlstring(L, text.data(), text.size());
  lua_setfield(L, -2, "reason");
  return 1;
}
int denied(lua_State *L, Verb need, Handle on, perm::Decision decision) {
  return denied(L, verb(need), on, decision.reason);
}
Vec3 vec(lua_State *L, int index) {
  const float *v = lua_tovector(L, index);
  if (!v || !std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]))
    luaL_error(L, "expected finite Luau vector");
  return {v[0], v[1], v[2]};
}
double number(lua_State *L, int index) {
  auto v = luaL_checknumber(L, index);
  if (!std::isfinite(v))
    luaL_error(L, "expected finite number");
  return v;
}
space::Pose pose(lua_State *L, int index) {
  space::Pose p{};
  if (lua_isnoneornil(L, index))
    return p;
  luaL_checktype(L, index, LUA_TTABLE);
  lua_getfield(L, index, "position");
  if (!lua_isnil(L, -1))
    p.position = vec(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, index, "rotation");
  if (!lua_isnil(L, -1)) {
    luaL_checktype(L, -1, LUA_TTABLE);
    float *elements[]{&p.rotation.x, &p.rotation.y, &p.rotation.z, &p.rotation.w};
    const char *keys[]{"x", "y", "z", "w"};
    for (int j = 0; j < 4; ++j) {
      lua_getfield(L, -1, keys[j]);
      *elements[j] = float(number(L, -1));
      lua_pop(L, 1);
    }
    float length = std::sqrt(p.rotation.x * p.rotation.x + p.rotation.y * p.rotation.y +
                             p.rotation.z * p.rotation.z + p.rotation.w * p.rotation.w);
    if (length == 0)
      luaL_error(L, "rotation quaternion has zero length");
    for (auto element : elements)
      *element /= length;
  }
  lua_pop(L, 1);
  return p;
}
int operation(lua_State *L, space::OperationResult result) {
  lua_createtable(L, 0, 2);
  lua_pushstring(L, space::status_text(result.status));
  lua_setfield(L, -2, "effect");
  if (result.obstacle) {
    react::push_handle(L, result.obstacle);
    lua_setfield(L, -2, "obstacle");
  }
  return 1;
}
bool within_reach(AttachmentBinding &b, Handle body, Handle target) {
  auto bounds = space::bounds(b.space, b.identity, target);
  if (!bounds)
    return false;
  auto p = b.transforms.position[body.slot()];
  Vec3 closest{std::clamp(p.x, bounds->min.x, bounds->max.x),
               std::clamp(p.y, bounds->min.y, bounds->max.y),
               std::clamp(p.z, bounds->min.z, bounds->max.z)};
  return (p - closest).length() <= b.reach_m;
}
TermId term(lua_State *L, int index, const react::Terms &terms) {
  if (lua_type(L, index) == LUA_TSTRING) {
    auto it = terms.by_name.find(lua_tostring(L, index));
    if (it != terms.by_name.end())
      return it->second;
  } else if (lua_type(L, index) == LUA_TNUMBER) {
    auto id = luaL_checkinteger(L, index);
    if (id > 0 && std::size_t(id) < terms.rows.size())
      return TermId(id);
  }
  luaL_error(L, "unknown term");
}
void navigation_origin(NavigationBinding &binding, Invocation &invocation, Handle body,
                       step::IntentId intent, std::optional<std::size_t> ending = {}) {
  const auto receipt = binding.next_origin++;
  binding.pending_origins.emplace(receipt, NavigationBinding::PendingOrigin{body, intent, ending});
  invocation.effects->ordered.emplace_back(react::CausalReceipt{
      &binding, receipt, [](void *owner, std::uint64_t receipt, const record::Origin &origin) {
        auto &b = *static_cast<NavigationBinding *>(owner);
        auto pending = b.pending_origins.at(receipt);
        b.pending_origins.erase(receipt);
        if (pending.ending) {
          auto &row = b.endings.at(*pending.ending);
          if (row.body == pending.body && row.intent.id == pending.intent)
            row.intent.origin = origin;
        } else if (auto found = b.intents.moving.find(pending.body.bits);
                   found != b.intents.moving.end() && found->second.id == pending.intent)
          found->second.origin = origin;
      }});
}
void mechanical_result(Invocation &i, MechanicalCauses &causes, space::CauseToken token,
                       bool changed, std::span<const Handle> objects = {}) {
  if (!changed) {
    causes.abandon(token);
    return;
  }
  i.effects->ordered.emplace_back(motion_receipt(causes, token));
  for (auto h : objects)
    react::consult_object(i, h);
  i.effects->changed = true;
}
} // namespace
int navigate_binding(lua_State *L) {
  auto &b = *static_cast<NavigationBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  auto body = i.self.object;
  auto permit = right(i, "navigation", body, Verb::Move, b.identity, b.authority, b.limits,
                      b.players, b.grants, b.declared_parts);
  if (!permit)
    return denied(L, Verb::Move, body, permit);
  step::MoveGoal goal = lua_type(L, 2) == LUA_TVECTOR ? step::MoveGoal(vec(L, 2))
                                                      : step::MoveGoal(handle(L, 2, b.identity));
  if (auto *point = std::get_if<Vec3>(&goal)) {
    for (const auto &plot : b.plots.rows)
      if (plot.id && point->x >= plot.bounds[0] && point->y >= plot.bounds[1] &&
          point->x <= plot.bounds[2] && point->y <= plot.bounds[3]) {
        auto holder = immediate(i, "navigation", b.identity, b.authority)
                          ? Holder::player(i.direct_body->actor)
                          : Holder::script(i.self.script, i.self.object);
        auto decision =
            perm::check_land(holder, perm::LandVerb::Enter, plot, b.players, b.grants, i.step);
        if (!decision)
          return denied(L, "enter", body, decision.reason);
      }
  }
  std::optional<float> speed;
  if (!lua_isnoneornil(L, 3)) {
    luaL_checktype(L, 3, LUA_TTABLE);
    lua_getfield(L, 3, "speed_m_s");
    if (!lua_isnil(L, -1)) {
      double value = number(L, -1);
      if (value < 0 || value > std::numeric_limits<float>::max())
        luaL_error(L, "speed_m_s must be a nonnegative float");
      speed = float(value);
    }
    lua_pop(L, 1);
  }
  auto result = step::start_move(b.intents, body, goal, i.origin, speed);
  if (result.replaced) {
    b.endings.push_back(std::move(*result.replaced));
    navigation_origin(b, i, body, b.endings.back().intent.id, b.endings.size() - 1);
  }
  navigation_origin(b, i, body, result.id);
  lua_createtable(L, 0, 2);
  lua_pushstring(L, "started");
  lua_setfield(L, -2, "effect");
  lua_pushnumber(L, double(result.id));
  lua_setfield(L, -2, "intent");
  return 1;
}
int cancel_motion_binding(lua_State *L) {
  auto &b = *static_cast<NavigationBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  if (auto ending = step::cancel_move(b.intents, i.self.object)) {
    b.endings.push_back(std::move(*ending));
    navigation_origin(b, i, i.self.object, b.endings.back().intent.id, b.endings.size() - 1);
  }
  lua_pushboolean(L, true);
  return 1;
}
int move_part_binding(lua_State *L) {
  auto &b = *static_cast<PartPoseBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  auto part = handle(L, 2, b.identity);
  if (part == i.self.object || !part_of(b.declared_parts, i.self.object, part))
    return denied(L, "declared_part", part);
  auto motion = space::body_motion(b.space, b.identity, part);
  if ((b.identity.flags[part.slot()] & Body) || !motion || !motion->independent ||
      motion->motion != space::Motion::Kinematic)
    return operation(L, {space::Status::UnsupportedMotion, {}});
  const auto position = vec(L, 3);
  luaL_checktype(L, 4, LUA_TTABLE);
  Quat rotation;
  float *elements[]{&rotation.x, &rotation.y, &rotation.z, &rotation.w};
  const char *keys[]{"x", "y", "z", "w"};
  float norm = 0;
  for (unsigned j = 0; j < 4; ++j) {
    lua_getfield(L, 4, keys[j]);
    *elements[j] = float(number(L, -1));
    norm += *elements[j] * *elements[j];
    lua_pop(L, 1);
  }
  if (!std::isfinite(norm) || std::abs(norm - 1.f) > 0.001f)
    luaL_error(L, "expected unit rotation quaternion");
  const bool changed = b.transforms.position[part.slot()] != position ||
                       b.transforms.rotation[part.slot()] != rotation;
  if (!changed)
    return operation(L, {space::Status::Ok, {}});
  auto token = b.causes.begin(0, false);
  auto result =
      space::set_pose(b.space, b.identity, b.transforms, part, {position, rotation}, token);
  mechanical_result(i, b.causes, token, result.status == space::Status::Ok, std::span(&part, 1));
  return operation(L, result);
}
int attach_binding(lua_State *L) {
  auto &b = *static_cast<AttachmentBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  auto subject = handle(L, 2, b.identity), parent = handle(L, 3, b.identity);
  auto local = pose(L, 4);
  auto held = live_held(i, b.identity, b.parents, b.relations.attached_to);
  auto move = right(i, "attachment", subject, Verb::Move, b.identity, b.authority, b.limits,
                    b.players, b.grants, b.declared_parts, held);
  if (!move)
    return denied(L, Verb::Move, subject, move);
  auto use = right(i, "attachment", parent, Verb::Use, b.identity, b.authority, b.limits, b.players,
                   b.grants, b.declared_parts, held);
  if (!own(i, b.declared_parts, parent) && !use)
    return denied(L, Verb::Use, parent, use);
  if (immediate(i, "attachment", b.identity, b.authority) &&
      !within_reach(b, i.self.object, parent))
    return denied(L, "reach", parent);
  auto token = b.causes.begin(0, false);
  std::vector<space::RelationChange> changes;
  auto result = space::attach(b.space, b.identity, b.transforms, b.parents, b.flags,
                              b.relations.contains, b.relations.supports, b.relations.attached_to,
                              {subject, parent, local, false}, changes, token);
  relation_effects(*i.effects, b.relation_terms, changes);
  const Handle affected[]{subject, parent};
  mechanical_result(i, b.causes, token, result.status == space::Status::Ok, affected);
  return operation(L, result);
}
int grasp_binding(lua_State *L) {
  auto &b = *static_cast<AttachmentBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  auto target = handle(L, 2, b.identity);
  auto local = pose(L, 3);
  auto take = right(i, "grasp", target, Verb::Take, b.identity, b.authority, b.limits, b.players,
                    b.grants, b.declared_parts);
  if (!take)
    return denied(L, Verb::Take, target, take);
  auto move = right(i, "grasp", i.self.object, Verb::Move, b.identity, b.authority, b.limits,
                    b.players, b.grants, b.declared_parts);
  if (!move)
    return denied(L, Verb::Move, i.self.object, move);
  if (immediate(i, "grasp", b.identity, b.authority) &&
      (!within_reach(b, i.self.object, target) || local.position.length() > b.reach_m))
    return denied(L, "reach", target);
  if (b.relations.attached_to.out.get(target.slot()))
    return operation(L, {space::Status::AlreadyParented, {}});
  for (auto ancestor = i.self.object; ancestor; ancestor = b.parents.parent[ancestor.slot()])
    if (ancestor == target)
      return operation(L, {space::Status::AttachmentCycle, {}});
  auto token = b.causes.begin(0, false);
  if (space::is_logical_root(b.space, b.identity, target) ||
      b.declared_parts.out.get(target.slot()) != nullptr) {
    std::vector<Handle> descendants;
    auto collect = [&](auto &&self, Handle parent) -> void {
      if (auto *children = b.declared_parts.out.get(parent.slot()))
        for (auto child : *children) {
          descendants.push_back(child);
          self(self, child);
        }
    };
    collect(collect, target);
    auto prepared = space::assemble(b.space, b.identity, b.transforms, target, descendants,
                                    space::Motion::Kinematic, token);
    if (prepared.status != space::Status::Ok) {
      b.causes.abandon(token);
      return operation(L, prepared);
    }
  }
  std::vector<space::RelationChange> changes;
  auto result = space::attach(b.space, b.identity, b.transforms, b.parents, b.flags,
                              b.relations.contains, b.relations.supports, b.relations.attached_to,
                              {target, i.self.object, local, true}, changes, token);
  relation_effects(*i.effects, b.relation_terms, changes);
  mechanical_result(i, b.causes, token, result.status == space::Status::Ok, std::span(&target, 1));
  return operation(L, result);
}
int release_binding(lua_State *L) {
  auto &b = *static_cast<AttachmentBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  auto target = handle(L, 2, b.identity);
  bool held_by_self =
      (b.identity.flags[target.slot()] & Held) && b.parents.parent[target.slot()] == i.self.object;
  if (!held_by_self && !own(i, b.declared_parts, target))
    return denied(L, "held_or_owned_part", target);
  auto control = right(i, "release", i.self.object, Verb::Move, b.identity, b.authority, b.limits,
                       b.players, b.grants, b.declared_parts);
  if (!control)
    return denied(L, Verb::Move, i.self.object, control);
  auto linear = lua_isnoneornil(L, 3) ? Vec3{} : vec(L, 3),
       angular = lua_isnoneornil(L, 4) ? Vec3{} : vec(L, 4);
  auto token = b.causes.begin(0, false);
  std::vector<space::RelationChange> changes;
  auto result =
      space::detach(b.space, b.identity, b.transforms, b.parents, b.flags, b.relations.attached_to,
                    {target, space::Motion::Dynamic, linear, angular}, changes, token);
  relation_effects(*i.effects, b.relation_terms, changes);
  mechanical_result(i, b.causes, token, result.status == space::Status::Ok, std::span(&target, 1));
  return operation(L, result);
}
int impulse_binding(lua_State *L) {
  auto &b = *static_cast<ImpulseBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  auto subject = handle(L, 2, b.identity);
  auto momentum = vec(L, 3), point = vec(L, 4);
  auto permit = right(i, "impulse", subject, Verb::Move, b.identity, b.authority, b.limits,
                      b.players, b.grants, b.declared_parts);
  if (!permit)
    return denied(L, Verb::Move, subject, permit);
  if (momentum == Vec3{})
    return operation(L, {space::Status::Ok, {}});
  auto token = b.causes.begin(space::motion_cause(b.space, b.identity, subject), true);
  auto result = space::impulse(b.space, b.identity, subject, momentum, point, token);
  mechanical_result(i, b.causes, token, result.status == space::Status::Ok, std::span(&subject, 1));
  return operation(L, result);
}
int sweep_binding(lua_State *L) {
  auto &b = *static_cast<SweepBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  auto subject = handle(L, 2, b.identity);
  auto translation = vec(L, 3);
  auto permit = right(i, "sweep", subject, Verb::Move, b.identity, b.authority, b.limits, b.players,
                      b.grants, b.declared_parts);
  if (!permit)
    return denied(L, Verb::Move, subject, permit);
  auto token = b.causes.begin(0, false);
  const auto before = b.transforms.position[subject.slot()];
  auto result = space::translate(b.space, b.identity, b.transforms, subject, translation,
                                 float(1 / i.hz), i.deadline, token);
  mechanical_result(i, b.causes, token, before != b.transforms.position[subject.slot()],
                    std::span(&subject, 1));
  operation(L, {result.status, result.contact ? result.contact->object : Handle{}});
  lua_pushvector(L, result.actual.position.x, result.actual.position.y, result.actual.position.z);
  lua_setfield(L, -2, "position");
  return 1;
}
int transfer_binding(lua_State *L) {
  auto &b = *static_cast<QuantityBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  auto from = handle(L, 2, b.identity), to = handle(L, 3, b.identity);
  auto id = term(L, 4, b.terms);
  double amount = number(L, 5);
  const auto &t = b.terms.rows[id];
  if (!t.conserved || t.kind != react::Kind::Quantity || t.source != react::Source::State ||
      t.combine != react::Combine::None || amount < 0)
    luaL_error(L, "transfer requires conserved state quantity and nonnegative amount");
  auto held = live_held(i, b.identity, b.parents, b.grasped);
  if (!own(i, b.declared_parts, from)) {
    auto permit = right(i, "transfer", from, Verb::Take, b.identity, b.authority, b.limits,
                        b.players, b.grants, b.declared_parts, held);
    if (!permit)
      return denied(L, Verb::Take, from, permit);
  }
  if (!own(i, b.declared_parts, to)) {
    auto permit = right(i, "transfer", to, Verb::Use, b.identity, b.authority, b.limits, b.players,
                        b.grants, b.declared_parts, held);
    if (!permit)
      return denied(L, Verb::Use, to, permit);
  }
  auto current = react::read_state(b.properties, b.processes, from, id, i.step, i.hz);
  amount = std::min(amount, current.present ? std::max(0., current.value.numeric()) : 0.);
  try {
    if (from == to || amount == 0 ||
        !react::transfer_state(b.properties, b.processes, t, from, to, amount, i.step, i.hz,
                               *i.effects))
      amount = 0;
  } catch (const std::invalid_argument &e) {
    luaL_error(L, "transfer failed: %s", e.what());
  }
  lua_pushnumber(L, amount);
  return 1;
}
int position_binding(lua_State *L) {
  auto &b = *static_cast<PositionBinding *>(react::binding_data(L));
  auto h = handle(L, 2, b.identity);
  react::consult_object(react::invocation(L), h);
  auto p = b.transforms.position[h.slot()];
  lua_pushvector(L, p.x, p.y, p.z);
  return 1;
}
int transform_binding(lua_State *L) {
  auto &b = *static_cast<PositionBinding *>(react::binding_data(L));
  auto h = handle(L, 2, b.identity);
  react::consult_object(react::invocation(L), h);
  auto p = b.transforms.position[h.slot()];
  auto q = b.transforms.rotation[h.slot()];
  lua_createtable(L, 0, 2);
  lua_pushvector(L, p.x, p.y, p.z);
  lua_setfield(L, -2, "position");
  lua_createtable(L, 0, 4);
  lua_pushnumber(L, q.x);
  lua_setfield(L, -2, "x");
  lua_pushnumber(L, q.y);
  lua_setfield(L, -2, "y");
  lua_pushnumber(L, q.z);
  lua_setfield(L, -2, "z");
  lua_pushnumber(L, q.w);
  lua_setfield(L, -2, "w");
  lua_setfield(L, -2, "rotation");
  return 1;
}
int pose_batch_binding(lua_State *L) {
  auto &b = *static_cast<PoseBatchBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  luaL_checktype(L, 2, LUA_TTABLE);
  auto count = std::size_t(lua_objlen(L, 2));
  if (count > b.batch_max)
    luaL_error(L, "pose batch exceeds configured limit");
  struct Row {
    Handle object;
    space::Pose pose;
  };
  std::vector<Row> rows;
  rows.reserve(count);
  for (std::size_t index = 1; index <= count; ++index) {
    lua_rawgeti(L, 2, int(index));
    luaL_checktype(L, -1, LUA_TTABLE);
    lua_getfield(L, -1, "object");
    auto h = handle(L, -1, b.identity);
    lua_pop(L, 1);
    auto properties = space::body_motion(b.space, b.identity, h);
    if (!properties || properties->collision_solid ||
        properties->motion != space::Motion::Kinematic || !properties->independent ||
        (b.identity.flags[h.slot()] & Body))
      return denied(L, "independent_non_solid_kinematic", h, Reason::InvalidScope);
    if (!own(i, b.declared_parts, h)) {
      auto permission = perm::check(Holder::script(i.self.script, i.self.object), Verb::Move,
                                    perm::target(b.identity, b.authority, b.limits, h), b.players,
                                    b.grants, i.step);
      if (!permission)
        return denied(L, Verb::Move, h, permission);
    }
    auto target = pose(L, -1);
    lua_getfield(L, -1, "rotation");
    if (lua_isnil(L, -1))
      target.rotation = b.transforms.rotation[h.slot()];
    lua_pop(L, 1);
    // Position is required; pose() permits omission only for attachment identity poses.
    lua_getfield(L, -1, "position");
    target.position = vec(L, -1);
    lua_pop(L, 1);
    rows.push_back({h, target});
    lua_pop(L, 1);
  }
  int changed = 0;
  auto token = b.causes.begin(0, false);
  for (const auto &row : rows)
    if (b.transforms.position[row.object.slot()] != row.pose.position ||
        b.transforms.rotation[row.object.slot()] != row.pose.rotation) {
      space::set_pose(b.space, b.identity, b.transforms, row.object, row.pose, token);
      react::consult_object(i, row.object);
      ++changed;
    }
  mechanical_result(i, b.causes, token, changed != 0);
  lua_pushinteger(L, changed);
  return 1;
}
int near_binding(lua_State *L) {
  auto &b = *static_cast<QueryBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  auto p = vec(L, 2);
  double radius = number(L, 3);
  if (radius < 0 || radius > b.max_radius_m)
    luaL_error(L, "query radius exceeds configured bound");
  std::vector<space::Nearby> rows;
  bool complete = space::near(b.space, b.identity, p, float(radius), {i.self.object, {}, false},
                              i.deadline, rows);
  lua_createtable(L, 0, 2);
  lua_pushboolean(L, complete);
  lua_setfield(L, -2, "complete");
  lua_createtable(L, int(rows.size()), 0);
  int index = 1;
  for (auto row : rows) {
    i.acquired.push_back(row.object);
    react::push_handle(L, row.object);
    lua_rawseti(L, -2, index++);
  }
  lua_setfield(L, -2, "objects");
  return 1;
}
int ray_binding(lua_State *L) {
  auto &b = *static_cast<QueryBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  auto from = vec(L, 2), to = vec(L, 3);
  if ((to - from).length() > b.max_radius_m)
    luaL_error(L, "ray exceeds configured bound");
  auto result =
      space::raycast(b.space, b.identity, from, to, {i.self.object, {}, false}, i.deadline);
  lua_createtable(L, 0, 4);
  lua_pushboolean(L, result.complete);
  lua_setfield(L, -2, "complete");
  if (result.hit) {
    auto &hit = *result.hit;
    i.acquired.push_back(hit.object);
    react::push_handle(L, hit.object);
    lua_setfield(L, -2, "object");
    lua_pushvector(L, hit.position.x, hit.position.y, hit.position.z);
    lua_setfield(L, -2, "position");
    lua_pushnumber(L, hit.fraction);
    lua_setfield(L, -2, "fraction");
  }
  return 1;
}
int sound_binding(lua_State *L) {
  auto &b = *static_cast<SoundBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  if (!b.identity.valid(i.self.object))
    luaL_error(L, "stale sound source");
  auto payload = react::check_event_payload(L, 2);
  double radius = number(L, 3);
  if (radius < 0 || radius > std::numeric_limits<float>::max())
    luaL_error(L, "sound radius must be a nonnegative float");
  auto site = lua_isnoneornil(L, 4) ? Vec3{} : vec(L, 4);
  bool speech = false;
  if (!lua_isnoneornil(L, 5)) {
    luaL_checktype(L, 5, LUA_TTABLE);
    lua_getfield(L, 5, "kind");
    if (!lua_isnil(L, -1)) {
      std::string_view kind = luaL_checkstring(L, -1);
      if (kind != "sound" && kind != "speech")
        luaL_error(L, "sound kind must be sound or speech");
      speech = kind == "speech";
    }
    lua_pop(L, 1);
  }
  auto position = space::transform_point(
      {b.transforms.position[i.self.object.slot()], b.transforms.rotation[i.self.object.slot()]},
      site);
  record::Event event;
  event.step = i.step;
  event.kind = speech ? record::Kind::Speech : record::Kind::System;
  event.actor = speech && i.direct_body ? record::Actor::player(i.direct_body->actor)
                                        : record::Actor::script(i.self.script);
  event.name = speech ? "speech" : "sound";
  event.result = record::Result::Applied;
  event.changed = false;
  event.things = {i.self.object};
  if (i.declaration)
    event.script_author = i.declaration->author;
  event.detail = {{"position", {position.x, position.y, position.z}},
                  {"radius_m", radius},
                  {"data", observe::event_payload_json(payload)},
                  {"agent", false}};
  i.effects->ordered.push_back(std::move(event));
  observe::emit_sound(b.hearing,
                      {i.self.object, position, float(radius), std::move(payload),
                       i.direct_body ? i.direct_body->actor : 0, i.origin},
                      b.listeners);
  lua_pushboolean(L, true);
  return 1;
}
int allow_avatar_binding(lua_State *L) {
  auto &b = *static_cast<AvatarControlBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  if (!immediate(i, "avatar_control", b.identity, b.authority))
    return denied(L, "avatar_control", i.self.object);
  auto selected = handle(L, 2, b.identity);
  if (selected != i.direct_body->selected)
    return denied(L, "selected", selected);
  auto script = luaL_checkinteger(L, 3), rights = luaL_checkinteger(L, 4);
  double seconds = number(L, 5);
  if (script <= 0 || rights <= 0 || (rights & ~3) || seconds <= 0 ||
      seconds * i.hz > double(std::numeric_limits<Step>::max() - i.step))
    luaL_error(L, "invalid avatar consent script/rights/expiry");
  auto *installed = b.installed_scripts.get(selected.slot());
  auto model = b.identity.model[selected.slot()];
  bool model_default =
      model < b.models.rows.size() && b.models.rows[model].script == ScriptId(script);
  if (!model_default &&
      (!installed || std::ranges::find(*installed, ScriptId(script)) == installed->end()))
    return denied(L, "installed_script", selected);
  auto holder = Holder::script(ScriptId(script), selected);
  auto scope = perm::Scope::avatar(i.self.object);
  auto actor = i.direct_body->actor;
  auto result = perm::create_grant(
      b.grants, b.players,
      {actor, holder, scope, perm::Rights(rights), 0, 0, i.step + Step(std::ceil(seconds * i.hz))},
      {scope, actor, 3, 0, false}, {holder, b.authority.owner[selected.slot()], true}, i.step);
  if (!result)
    return denied(L, "avatar_control", selected, result.reason);
  i.effects->changed = true;
  lua_pushinteger(L, int(result.grant));
  return 1;
}
int revoke_avatar_binding(lua_State *L) {
  auto &b = *static_cast<AvatarControlBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  if (!immediate(i, "avatar_control", b.identity, b.authority))
    return denied(L, "avatar_control", i.self.object);
  auto id = luaL_checkinteger(L, 2);
  if (id <= 0 || std::size_t(id) >= b.grants.rows.size())
    luaL_error(L, "unknown grant");
  const auto &g = b.grants.rows[id];
  if (g.issuer != i.direct_body->actor || g.scope != perm::Scope::avatar(i.self.object))
    return denied(L, "own_avatar_grant", i.self.object);
  auto revoked = perm::revoke(b.grants, b.players, GrantId(id));
  i.effects->changed |= !revoked.empty();
  lua_pushboolean(L, !revoked.empty());
  return 1;
}
} // namespace amber::bindings
