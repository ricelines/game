#include "catalog/catalog.hpp"
#include "step.hpp"
#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace amber::step {
namespace {
struct NavigationBinding {
  step::Intents &intents;
  std::vector<step::IntentEnding> &endings;
  IdentityRead identity;
  const perm::Players &players;
  const perm::Grants &grants;
  const perm::Plots &plots;
};
struct SweepBinding {
  space::Space &space;
  IdentityRead identity;
  TransformWrite transforms;
  const Relation &declared_parts;
  MechanicalCauses &causes;
  Objects &objects;
  const perm::Plots &plots;
};
struct PartPoseBinding {
  space::Space &space;
  IdentityRead identity;
  TransformWrite transforms;
  const Relation &declared_parts;
  MechanicalCauses &causes;
  Objects &objects;
  const perm::Plots &plots;
  std::size_t batch_max;
};
struct QueryBinding {
  const space::Space &space;
  IdentityRead identity;
  TransformRead transforms;
};
struct SoundBinding {
  observe::Hearing &hearing;
  IdentityRead identity;
  TransformRead transforms;
  const std::vector<observe::Listener> &listeners;
  const catalog::InstanceTexts &instance_texts;
  const catalog::ModelTexts &model_texts;
};
using Json = nlohmann::json;
using perm::Holder;
using perm::Reason;
using perm::Verb;
using react::Invocation;
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
bool acquired(const Invocation &i, Handle h) {
  return h == i.self.object || std::ranges::find(i.parts, h) != i.parts.end() ||
         std::ranges::find(i.acquired, h) != i.acquired.end();
}
Handle handle(lua_State *L, int index, IdentityRead id) {
  auto h = react::check_handle(L, index);
  if (!id.valid(h))
    luaL_error(L, "stale object handle");
  if (!acquired(react::invocation(L), h))
    luaL_error(L, "object handle was not acquired");
  return h;
}
std::string permission_fix(std::string_view need, PlayerId owner = 0) {
  if (need == "reach")
    return "move closer";
  if (need == "acyclic_attachment")
    return "select a parent outside this assembly";
  if (need == "unattached")
    return "release or detach the item first";
  if (need == "self_or_part")
    return "select this script's own object or a declared part";
  return "ask " + (owner ? "p:" + std::to_string(owner) : std::string("the owner")) + " for " +
         std::string(need);
}
int denied(lua_State *L, const char *need, Handle on, Reason why = Reason::MissingRight) {
  lua_createtable(L, 0, 2);
  lua_pushboolean(L, false);
  lua_setfield(L, -2, "ok");
  lua_createtable(L, 0, 4);
  lua_pushstring(L, need);
  lua_setfield(L, -2, "need");
  react::push_handle(L, on);
  lua_setfield(L, -2, "on");
  auto text = perm::reason_text(why);
  lua_pushlstring(L, text.data(), text.size());
  lua_setfield(L, -2, "why");
  const auto fix = permission_fix(need);
  lua_pushlstring(L, fix.data(), fix.size());
  lua_setfield(L, -2, "fix");
  lua_setfield(L, -2, "denied");
  return 1;
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
bool within_reach(space::Space &world, IdentityRead identity, TransformRead transforms,
                  float reach_m, Handle body, Handle target) {
  auto bounds = space::bounds(world, identity, target);
  if (!bounds)
    return false;
  auto p = transforms.position[body.slot()];
  Vec3 closest{std::clamp(p.x, bounds->min.x, bounds->max.x),
               std::clamp(p.y, bounds->min.y, bounds->max.y),
               std::clamp(p.z, bounds->min.z, bounds->max.z)};
  return (p - closest).length() <= reach_m;
}
std::vector<Handle> closure(const Relation &parts, Handle root) {
  std::vector<Handle> result{root};
  for (std::size_t index = 0; index < result.size(); ++index)
    if (const auto *children = parts.out.get(result[index].slot()))
      result.insert(result.end(), children->begin(), children->end());
  return result;
}
Vec3 input_vector(const Json &value) {
  if (!value.is_array() || value.size() != 3)
    throw std::invalid_argument("expected vector [x,y,z]");
  Vec3 out{value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>()};
  if (!std::isfinite(out.x) || !std::isfinite(out.y) || !std::isfinite(out.z))
    throw std::invalid_argument("expected finite vector");
  return out;
}
space::Pose input_pose(const Json &value) {
  space::Pose out{};
  if (value.contains("position"))
    out.position = input_vector(value.at("position"));
  if (value.contains("rotation")) {
    const auto &q = value.at("rotation");
    if (!q.is_array() || q.size() != 4)
      throw std::invalid_argument("expected quaternion [x,y,z,w]");
    out.rotation = {q.at(0), q.at(1), q.at(2), q.at(3)};
    const auto norm = out.rotation.x * out.rotation.x + out.rotation.y * out.rotation.y +
                      out.rotation.z * out.rotation.z + out.rotation.w * out.rotation.w;
    if (!std::isfinite(norm) || std::abs(norm - 1.f) > 0.001f)
      throw std::invalid_argument("expected finite unit quaternion");
  }
  return out;
}
Handle input_handle(const Json &value, IdentityRead identity) {
  const auto h = parse_handle(value.get<std::string>());
  if (!h || !identity.valid(*h))
    throw std::invalid_argument("stale object handle");
  return *h;
}
perm::Holder actor(const step::NativeCall &call) {
  if (const auto *player = std::get_if<PlayerId>(&call.request.caller))
    return Holder::player(*player);
  const auto script = std::get<ScriptHolder>(call.request.caller);
  return Holder::script(script.script, script.object);
}
Handle actor_body(const step::NativeCall &call) {
  if (const auto *script = std::get_if<ScriptHolder>(&call.request.caller))
    return script->object;
  return call.body;
}
step::NativeCompletion native_denied(std::string_view need, Handle target, Reason why,
                                     PlayerId owner = 0) {
  step::NativeCompletion out;
  out.result = record::Result::Denied;
  out.reply = {{"ok", false},
               {"denied",
                {{"need", need},
                 {"on", to_string(target)},
                 {"why", perm::reason_text(why)},
                 {"fix", permission_fix(need, owner)}}}};
  return out;
}
step::NativeCompletion native_result(space::OperationResult result, Handle subject, bool changed) {
  step::NativeCompletion out;
  const bool ok = result.status == space::Status::Ok;
  out.changed = ok && changed;
  out.result =
      ok ? (changed ? record::Result::Applied : record::Result::NoEffect) : record::Result::Denied;
  out.reply = {{"ok", ok},
               {"effect", out.changed ? "applied" : "none"},
               {"reason", space::status_text(result.status)}};
  if (!ok) {
    const auto need = result.status == space::Status::AlreadyParented   ? "unattached"
                      : result.status == space::Status::AttachmentCycle ? "acyclic_attachment"
                      : result.status == space::Status::Obstructed      ? "clearance"
                      : result.status == space::Status::NotAttached     ? "attached_object"
                                                                        : "physical_object";
    const auto fix = result.status == space::Status::Obstructed    ? "move the obstruction first"
                     : result.status == space::Status::NotAttached ? "select an attached object"
                     : result.status == space::Status::MissingGeometry ||
                             result.status == space::Status::MissingBody ||
                             result.status == space::Status::UnsupportedMotion
                         ? "select a visible object with a compatible physical shape"
                     : result.status == space::Status::Capacity
                         ? "remove unused physical objects before retrying"
                         : "select a live object";
    out.reply["denied"] = {{"need", need},
                           {"on", to_string(subject)},
                           {"why", space::status_text(result.status)},
                           {"fix", result.status == space::Status::AlreadyParented ||
                                           result.status == space::Status::AttachmentCycle
                                       ? permission_fix(need)
                                       : fix}};
  }
  if (result.obstacle)
    out.reply["obstacle"] = to_string(result.obstacle);
  return out;
}
} // namespace
step::NativeCompletion grasp_action(amber::ObjectState &objects_state,
                                    amber::space::State &space_state,
                                    const PhysicalRelationTerms &relation_terms, float reach_m,
                                    MechanicalCauses &causes, const perm::Plots &plots,
                                    const step::NativeCall &call) {
  auto &world = space_state.space;

  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto transforms = amber::transforms(objects_state.objects);
  auto parents = amber::parents(objects_state.objects);

  auto &relations = space_state.physical_relations;
  auto &declared_parts = objects_state.declared_parts;

  const auto target = call.request.target, body = actor_body(call);
  const auto local = input_pose(call.arguments.value("local_pose", Json::object()));
  if (!identity.valid(body) || !within_reach(world, identity, transforms, reach_m, body, target) ||
      local.position.length() > reach_m)
    return native_denied("reach", target, Reason::InvalidScope);
  if (relations.attached_to.out.get(target.slot()))
    return native_denied("unattached", target, Reason::InvalidScope);
  for (auto parent = body; parent; parent = parents.parent[parent.slot()])
    if (parent == target)
      return native_denied("acyclic_attachment", target, Reason::InvalidScope);
  auto token = causes.intern(call.origin);
  auto objects = closure(declared_parts, target);
  if (objects.size() > 1 || space::is_logical_root(world, identity, target)) {
    const auto assembled =
        space::assemble(world, identity, transforms, target, std::span(objects).subspan(1),
                        space::Motion::Kinematic, token);
    if (assembled.status != space::Status::Ok)
      return native_result(assembled, target, false);
  }
  const auto changes_begin = space_state.relation_changes.size();
  auto result = space::attach(objects_state, space_state, {target, body, local, true}, token);
  auto out = native_result(result, target, true);
  relation_effects(out.effects, relation_terms,
                   std::span(space_state.relation_changes).subspan(changes_begin));
  space_state.relation_changes.resize(changes_begin);
  if (out.changed) {
    perm::update_plots(objects_state.objects, plots, objects, true);
    out.changed_objects = objects;
  }
  return out;
}
step::NativeCompletion attach_action(amber::ObjectState &objects_state,
                                     amber::space::State &space_state,
                                     const PhysicalRelationTerms &relation_terms,
                                     const amber::perm::State &perm_state, float reach_m,
                                     MechanicalCauses &causes, const step::NativeCall &call) {
  auto &world = space_state.space;

  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto transforms = amber::transforms(objects_state.objects);

  auto &declared_parts = objects_state.declared_parts;
  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto &limits = perm_state.owner_limits;
  auto &players = perm_state.players;
  auto &grants = perm_state.grants;
  auto &plots = perm_state.plots;

  const auto subject = call.request.target;
  const auto parent = input_handle(call.arguments.at("parent"), identity);
  const auto permit = perm::check(
      actor(call), Verb::Use, perm::target(identity, authority, limits, perm_state.plots, parent),
      players, grants, call.step);
  if (!permit)
    return native_denied("use", parent, permit.reason, authority.owner[parent.slot()]);
  const auto body = actor_body(call);
  if (!identity.valid(body) || !within_reach(world, identity, transforms, reach_m, body, parent))
    return native_denied("reach", parent, Reason::InvalidScope);
  auto token = causes.intern(call.origin);
  const auto changes_begin = space_state.relation_changes.size();
  auto result = space::attach(
      objects_state, space_state,
      {subject, parent, input_pose(call.arguments.value("local_pose", Json::object())), false},
      token);
  auto out = native_result(result, subject, true);
  relation_effects(out.effects, relation_terms,
                   std::span(space_state.relation_changes).subspan(changes_begin));
  space_state.relation_changes.resize(changes_begin);
  if (out.changed) {
    const auto objects = closure(declared_parts, subject);
    perm::update_plots(objects_state.objects, plots, objects, true);
    out.changed_objects = objects;
  }
  return out;
}
step::NativeCompletion release_action(amber::ObjectState &objects_state,
                                      amber::space::State &space_state,
                                      const PhysicalRelationTerms &relation_terms,
                                      const amber::perm::State &perm_state,
                                      MechanicalCauses &causes, const step::NativeCall &call) {

  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto parents = amber::parents(objects_state.objects);

  auto &declared_parts = objects_state.declared_parts;
  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto &limits = perm_state.owner_limits;
  auto &players = perm_state.players;
  auto &grants = perm_state.grants;
  auto &plots = perm_state.plots;

  const auto target = call.request.target;
  const auto holder = actor(call);
  const bool held_here =
      (identity.flags[target.slot()] & Held) && parents.parent[target.slot()] == actor_body(call);
  const bool own_part = holder.kind == perm::HolderKind::Script &&
                        part_of(declared_parts, holder.installed_on, target);
  if (!held_here && !own_part) {
    const auto permit =
        (identity.flags[target.slot()] & Body)
            ? perm::check_scope(holder, perm::Scope::avatar(target), 1, grants, call.step)
            : perm::check(holder, Verb::Move,
                          perm::target(identity, authority, limits, perm_state.plots, target),
                          players, grants, call.step);
    if (!permit)
      return native_denied("move", target, permit.reason, authority.owner[target.slot()]);
  }
  const auto linear =
      call.arguments.contains("linear") ? input_vector(call.arguments.at("linear")) : Vec3{};
  const auto angular =
      call.arguments.contains("angular") ? input_vector(call.arguments.at("angular")) : Vec3{};
  auto token = causes.intern(call.origin);
  const auto changes_begin = space_state.relation_changes.size();
  const auto motion =
      identity.flags[target.slot()] & Body ? space::Motion::Character : space::Motion::Dynamic;
  auto result = space::detach(objects_state, space_state, {target, motion, linear, angular}, token);
  auto out = native_result(result, target, true);
  relation_effects(out.effects, relation_terms,
                   std::span(space_state.relation_changes).subspan(changes_begin));
  space_state.relation_changes.resize(changes_begin);
  if (out.changed) {
    const auto objects = closure(declared_parts, target);
    perm::update_plots(objects_state.objects, plots, objects, true);
    out.changed_objects = objects;
  }
  return out;
}
step::NativeCompletion impulse_action(space::Space &world, IdentityRead identity,
                                      MechanicalCauses &causes, const step::NativeCall &call) {
  const auto target = call.request.target;
  const auto momentum = input_vector(call.arguments.at("momentum"));
  const auto point = input_vector(call.arguments.at("point"));
  auto token = causes.intern(call.origin, space::motion_cause(world, identity, target));
  auto out = native_result(space::impulse(world, identity, target, momentum, point, token), target,
                           momentum != Vec3{});
  if (out.changed)
    out.changed_objects.push_back(target);
  return out;
}
step::NativeCompletion transfer_action(amber::react::State &react_state,
                                       const amber::ObjectState &objects_state,
                                       const amber::perm::State &perm_state,
                                       const step::NativeCall &call) {
  auto &properties = react_state.properties;
  auto &processes = react_state.processes;
  auto &terms = react_state.terms;
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto &limits = perm_state.owner_limits;
  auto &players = perm_state.players;
  auto &grants = perm_state.grants;

  const auto source = call.request.target;
  const auto destination = input_handle(call.arguments.at("to"), identity);
  const auto permit =
      perm::check(actor(call), Verb::Use,
                  perm::target(identity, authority, limits, perm_state.plots, destination), players,
                  grants, call.step);
  if (!permit)
    return native_denied("use", destination, permit.reason, authority.owner[destination.slot()]);
  const auto term = terms.by_name.at(call.arguments.at("term").get<std::string>());
  const auto &quantity = terms.rows[term];
  auto amount = call.arguments.at("amount").get<double>();
  if (!quantity.conserved || quantity.kind != react::Kind::Quantity ||
      quantity.source != react::Source::State || quantity.combine != react::Combine::None ||
      !std::isfinite(amount) || amount < 0)
    throw std::invalid_argument("transfer requires a conserved state quantity and finite amount");
  const auto current = react::read_state(properties, processes, source, term, call.step, call.hz);
  const auto received =
      react::read_state(properties, processes, destination, term, call.step, call.hz);
  const auto *source_process =
      term < processes.size() ? processes[term].get(source.slot()) : nullptr;
  const auto *destination_process =
      term < processes.size() ? processes[term].get(destination.slot()) : nullptr;
  const auto floor = source_process ? source_process->lo : 0.;
  amount = std::min(amount, current.present ? std::max(0., current.value.numeric() - floor) : 0.);
  double ceiling =
      destination_process ? destination_process->hi : std::numeric_limits<double>::infinity();
  if (call.arguments.contains("to_max")) {
    const auto bound = call.arguments.at("to_max").get<double>();
    if (!std::isfinite(bound))
      throw std::invalid_argument(
          "destination bound must be finite and use the transfer term's units");
    ceiling = std::min(ceiling, bound);
  }
  amount =
      std::min(amount, std::max(0., ceiling - (received.present ? received.value.numeric() : 0.)));
  step::NativeCompletion out;
  out.changed = react::transfer_state(react_state, quantity, source, destination, amount, call.step,
                                      call.hz, out.effects);
  out.result = out.changed ? record::Result::Applied : record::Result::NoEffect;
  out.reply = {{"ok", true},
               {"effect", out.changed ? "applied" : "none"},
               {"amount", out.changed ? amount : 0.}};
  if (out.changed)
    out.changed_objects = {source, destination};
  return out;
}
namespace {
void mechanical_result(Invocation &i, bool changed, std::span<const Handle> objects = {}) {
  if (!changed)
    return;
  for (auto h : objects)
    react::consult_object(i, h);
  i.effects->changed = true;
}
} // namespace
namespace {
int navigate_binding(lua_State *L) {
  auto &b = *static_cast<NavigationBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  auto body = i.self.object;

  step::MoveGoal goal = lua_type(L, 2) == LUA_TVECTOR ? step::MoveGoal(vec(L, 2))
                                                      : step::MoveGoal(handle(L, 2, b.identity));
  if (auto *point = std::get_if<Vec3>(&goal)) {
    for (const auto &plot : b.plots.rows)
      if (plot.id && point->x >= plot.bounds[0] && point->y >= plot.bounds[1] &&
          point->x <= plot.bounds[2] && point->y <= plot.bounds[3]) {
        auto holder = Holder::script(i.self.script, i.self.object);
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
    b.endings.back().intent.origin = i.origin;
  }
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
    b.endings.back().intent.origin = i.origin;
  }
  lua_pushboolean(L, true);
  return 1;
}
int move_part_binding(lua_State *L) {
  auto &b = *static_cast<PartPoseBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  std::size_t moved = 0;
  space::CauseToken token = 0;
  const auto move = [&](Handle part, space::Pose target) {
    if (part == i.self.object || !part_of(b.declared_parts, i.self.object, part))
      luaL_error(L, "move_part requires a declared part of this object");
    const auto motion = space::body_motion(b.space, b.identity, part);
    if ((b.identity.flags[part.slot()] & Body) || !motion || !motion->independent ||
        motion->motion != space::Motion::Kinematic)
      luaL_error(L, "move_part requires an independent kinematic part");
    if (b.transforms.position[part.slot()] == target.position &&
        b.transforms.rotation[part.slot()] == target.rotation)
      return;
    if (!token)
      token = b.causes.intern(i.origin);
    const auto result = space::set_pose(b.space, b.identity, b.transforms, part, target, token);
    if (!moved)
      mechanical_result(i, result.status == space::Status::Ok);
    react::consult_object(i, part);
    if (result.status != space::Status::Ok)
      luaL_error(L, "move_part: %s", space::status_text(result.status));
    if (b.declared_parts.out.get(part.slot())) {
      const auto objects = closure(b.declared_parts, part);
      perm::update_plots(b.objects, b.plots, objects, true);
    } else
      perm::update_plots(b.objects, b.plots, std::span(&part, 1), true);
    ++moved;
  };
  if (lua_istable(L, 2)) {
    const auto count = std::size_t(lua_objlen(L, 2));
    if (count > b.batch_max)
      luaL_error(L, "move_part batch exceeds configured limit");
    for (std::size_t row = 1; row <= count; ++row) {
      lua_rawgeti(L, 2, int(row));
      luaL_checktype(L, -1, LUA_TTABLE);
      lua_getfield(L, -1, "object");
      const auto part = handle(L, -1, b.identity);
      lua_pop(L, 1);
      auto target = pose(L, -1);
      lua_getfield(L, -1, "rotation");
      if (lua_isnil(L, -1))
        target.rotation = b.transforms.rotation[part.slot()];
      lua_pop(L, 1);
      move(part, target);
      lua_pop(L, 1);
    }
  } else {
    const auto part = handle(L, 2, b.identity);
    lua_newtable(L);
    lua_pushvalue(L, 3);
    lua_setfield(L, -2, "position");
    lua_pushvalue(L, 4);
    lua_setfield(L, -2, "rotation");
    move(part, pose(L, -1));
    lua_pop(L, 1);
  }
  lua_pushinteger(L, int(moved));
  return 1;
}
int sweep_binding(lua_State *L) {
  auto &b = *static_cast<SweepBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  auto subject = handle(L, 2, b.identity);
  auto translation = vec(L, 3);
  if (!own(i, b.declared_parts, subject))
    return denied(L, "self_or_part", subject);
  auto token = b.causes.intern(i.origin);
  const auto before = b.transforms.position[subject.slot()];
  auto result = space::translate(b.space, b.identity, b.transforms, subject, translation,
                                 float(1 / i.hz), token);
  mechanical_result(i, before != b.transforms.position[subject.slot()], std::span(&subject, 1));
  const auto objects = closure(b.declared_parts, subject);
  perm::update_plots(b.objects, b.plots, objects, true);
  operation(L, {result.status, result.contact ? result.contact->object : Handle{}});
  lua_pushvector(L, result.actual.position.x, result.actual.position.y, result.actual.position.z);
  lua_setfield(L, -2, "position");
  return 1;
}
int near_binding(lua_State *L) {
  auto &b = *static_cast<QueryBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  lua_getfield(L, 1, "handle");
  const auto self = handle(L, -1, b.identity);
  lua_pop(L, 1);
  if (self != i.self.object)
    luaL_error(L, "near needs this handler's self");
  const auto p = b.transforms.position[self.slot()];
  double radius = number(L, 2);
  if (radius < 0 || radius > std::numeric_limits<float>::max())
    luaL_error(L, "query radius must be a nonnegative finite float");
  std::vector<space::Nearby> rows;
  space::near(b.space, b.identity, p, float(radius), {i.self.object, {}, false, {}, i.deadline},
              rows);
  if (i.deadline.passed())
    luaL_error(L, "handler deadline exceeded");
  lua_createtable(L, 0, 1);
  lua_createtable(L, int(rows.size()), 0);
  int index = 1;
  for (auto row : rows) {
    if (i.deadline.passed())
      luaL_error(L, "handler deadline exceeded");
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
  auto from = vec(L, 1), to = vec(L, 2);
  auto result =
      space::raycast(b.space, b.identity, from, to, {i.self.object, {}, false, {}, i.deadline});
  if (i.deadline.passed())
    luaL_error(L, "handler deadline exceeded");
  lua_createtable(L, 0, 3);
  if (result) {
    auto &hit = *result;
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
  const auto *speaker = speech && i.action_caller && i.action_body == i.self.object
                            ? std::get_if<PlayerId>(&*i.action_caller)
                            : nullptr;
  event.actor = speaker ? record::Actor::player(*speaker) : record::Actor::script(i.self.script);
  event.name = speech ? "speech" : "sound";
  event.result = record::Result::Applied;
  event.changed = false;
  event.things = {i.self.object};
  if (i.declaration)
    event.script_author = i.declaration->author;
  event.detail = {{"position", {position.x, position.y, position.z}},
                  {"radius_m", radius},
                  {"data", net::event_payload_json(payload)},
                  {"agent", false}};
  i.effects->ordered.push_back(std::move(event));
  std::string name;
  if (const auto found = b.instance_texts.rows.find(i.self.object.bits);
      found != b.instance_texts.rows.end())
    name = found->second.name;
  if (name.empty())
    if (const auto found = b.model_texts.rows.find(b.identity.model[i.self.object.slot()]);
        found != b.model_texts.rows.end())
      name = found->second.name;
  observe::emit_sound(b.hearing,
                      {i.self.object, position, float(radius), std::move(payload),
                       speaker ? *speaker : 0, i.origin, std::move(name), speech},
                      b.listeners);
  lua_pushboolean(L, true);
  return 1;
}
} // namespace
void bind_physical(react::Vm &vm, amber::ObjectState &objects_state, amber::step::State &step_state,
                   const amber::perm::State &perm_state, std::size_t part_batch_max,
                   amber::space::State &space_state, const amber::catalog::State &catalog_state) {
  auto &space = space_state.space;

  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto transforms = amber::transforms(objects_state.objects);
  auto &declared_parts = objects_state.declared_parts;
  auto &causes = step_state.mechanical_causes;
  auto &plots = perm_state.plots;
  auto &intents = step_state.intents;
  auto &endings = step_state.intent_endings;
  auto &players = perm_state.players;
  auto &grants = perm_state.grants;
  auto &hearing = space_state.hearing;
  auto &listeners = space_state.listeners;
  auto &instance_texts = catalog_state.instance_text;
  auto &model_texts = catalog_state.model_text;

  const auto navigation = std::make_shared<NavigationBinding>(
      NavigationBinding{intents, endings, identity, players, grants, plots});
  const auto sweep = std::make_shared<SweepBinding>(SweepBinding{
      space, identity, transforms, declared_parts, causes, objects_state.objects, plots});
  const auto parts = std::make_shared<PartPoseBinding>(
      PartPoseBinding{space, identity, transforms, declared_parts, causes, objects_state.objects,
                      plots, part_batch_max});
  const auto query = std::make_shared<QueryBinding>(QueryBinding{space, identity, transforms});
  const auto sound = std::make_shared<SoundBinding>(
      SoundBinding{hearing, identity, transforms, listeners, instance_texts, model_texts});
  react::register_binding(vm, "navigate", navigate_binding, navigation);
  react::register_binding(vm, "cancel_motion", cancel_motion_binding, navigation);
  react::register_binding(vm, "sweep", sweep_binding, sweep);
  react::register_binding(vm, "move_part", move_part_binding, parts);
  react::register_binding(vm, "world.near", near_binding, query);
  react::register_binding(vm, "world.raycast", ray_binding, query);
  react::register_binding(vm, "sound", sound_binding, sound);
}
void declare_physical_actions(react::Terms &terms, react::NativeActions &declarations) {
  const Json vector = {
      {"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}};
  const Json rotation = {
      {"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 4}, {"maxItems", 4}};
  const Json local_pose = {{"type", "object"},
                           {"properties", {{"position", vector}, {"rotation", rotation}}},
                           {"additionalProperties", false}};
  const Json handle = {{"type", "string"}};
  const auto add = [&](std::string name, std::string description, std::string right, bool reach,
                       Json properties, Json required, react::NativeOperation operation) {
    const auto id = catalog::declare_native(declarations, terms, std::move(name),
                                            std::move(description), std::move(right), reach,
                                            {{"type", "object"},
                                             {"properties", std::move(properties)},
                                             {"required", std::move(required)},
                                             {"additionalProperties", false}},
                                            operation);
    (void)id;
  };
  add("engine.grasp", "Attach the selected item to the caller's body or script installation.",
      "take", true, {{"local_pose", local_pose}}, Json::array(), react::MechanicAction::Grasp);
  add("engine.release", "Detach the selected object with optional linear and angular velocity.", "",
      false, {{"linear", vector}, {"angular", vector}}, Json::array(),
      react::MechanicAction::Release);
  add("engine.attach", "Attach the selected object to a parent at its local pose.", "move", false,
      {{"parent", handle}, {"local_pose", local_pose}}, {"parent"}, react::MechanicAction::Attach);
  add("engine.impulse", "Apply momentum at a world point on the selected object.", "move", true,
      {{"momentum", vector}, {"point", vector}}, {"momentum", "point"},
      react::MechanicAction::Impulse);
  add("engine.transfer", "Move a conserved state quantity from the selected source to a receiver.",
      "move", false,
      {{"to", handle},
       {"term", {{"type", "string"}}},
       {"amount", {{"type", "number"}, {"minimum", 0}}},
       {"to_max", {{"type", "number"}}}},
      {"to", "term", "amount"}, react::MechanicAction::Transfer);
}
} // namespace amber::step
