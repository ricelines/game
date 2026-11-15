#include "goods.hpp"
#include "arguments/arguments.hpp"
#include "scene.hpp"

#include <algorithm>
#include <lua.h>
#include <lualib.h>

namespace amber::bindings {
namespace {
int denied(lua_State *L, std::string_view reason, Handle object) {
  lua_createtable(L, 0, 3);
  lua_pushstring(L, "denied");
  lua_setfield(L, -2, "effect");
  lua_pushlstring(L, reason.data(), reason.size());
  lua_setfield(L, -2, "reason");
  react::push_handle(L, object);
  lua_setfield(L, -2, "object");
  return 1;
}
} // namespace
int plot_objects_binding(lua_State *L) {
  const auto &b = *static_cast<const PlotObjectsBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  auto plot = unsigned_arg(L, 2);
  if (plot >= b.plots.rows.size() || b.plots.rows[plot].id != plot)
    luaL_error(L, "unknown plot");
  lua_newtable(L);
  int index = 0;
  for (std::uint32_t slot = 1; slot < b.identity.model.size(); ++slot) {
    if (!b.identity.model[slot] || b.plot[slot] != plot || (b.identity.flags[slot] & (Body | Held)))
      continue;
    const auto *part_of = b.declared_parts.in.get(slot);
    if (part_of && !part_of->empty())
      continue;
    auto object = b.identity.at(slot);
    react::push_handle(L, object);
    lua_rawseti(L, -2, ++index);
    i.acquired.push_back(object);
  }
  return 1;
}
int hold_goods_binding(lua_State *L) {
  auto &b = *static_cast<HoldGoodsBinding *>(react::binding_data(L));
  auto &i = react::invocation(L);
  auto h = react::check_handle(L, 2);
  if (!b.identity.valid(h))
    return denied(L, perm::reason_text(perm::Reason::StaleGoods), h);
  if (h != i.self.object && std::ranges::find(i.acquired, h) == i.acquired.end() &&
      std::ranges::find(i.parts, h) == i.parts.end())
    luaL_error(L, "hold_goods needs a live acquired object");
  if (const auto *parents = b.declared_parts.in.get(h.slot()); parents && !parents->empty())
    return denied(L, perm::reason_text(perm::Reason::InvalidScope), h);
  const auto recipient = player_arg(L, 3);
  std::vector<Handle> descendants;
  if (const auto *children = b.declared_parts.out.get(h.slot()))
    descendants = *children;
  for (std::size_t index = 0; index < descendants.size(); ++index) {
    auto child = descendants[index];
    if (!b.identity.valid(child))
      return denied(L, perm::reason_text(perm::Reason::StaleGoods), child);
    if (const auto *children = b.declared_parts.out.get(child.slot()))
      descendants.insert(descendants.end(), children->begin(), children->end());
  }
  std::vector<perm::Target> parts;
  parts.reserve(descendants.size());
  for (auto child : descendants)
    parts.push_back(perm::target(b.identity, b.authority, b.limits, child));
  auto delivered = b.delivery.present_bodies.find(recipient);
  bool present =
      delivered != b.delivery.present_bodies.end() && b.identity.valid(delivered->second);
  Handle custodian = present ? delivered->second : Handle{};
  auto plan = perm::prepare_store_goods(b.players, b.grants,
                                        perm::Holder::script(i.self.script, i.self.object),
                                        perm::target(b.identity, b.authority, b.limits, h), parts,
                                        recipient, custodian, present, b.mailbox_max, i.step);
  if (!plan)
    return denied(L, perm::reason_text(plan.reason), h);
  const auto &player = b.players.rows[recipient];
  const auto &stored = present ? player.inventory : player.mailbox;
  const bool changed = !(b.flags.get(h.slot()) & Held) ||
                       b.parents.parent[h.slot()] != plan.custodian ||
                       std::ranges::find(stored, h) == stored.end();
  if (changed) {
    auto result = space::custody(b.space, b.identity, h, plan.custodian, b.parents, b.flags,
                                 b.relations, b.relation_changes, descendants);
    if (result.status != space::Status::Ok)
      return denied(L, space::status_text(result.status), h);
    perm::commit_store_goods(b.players, plan);
    auto closure = descendants;
    closure.push_back(h);
    scene_visible(b.scene, closure, false);
  }
  record::Event event;
  event.step = i.step;
  event.kind = record::Kind::System;
  event.actor = record::Actor::script(i.self.script);
  event.name = "hold_goods";
  event.result = changed ? record::Result::Applied : record::Result::NoEffect;
  event.changed = changed;
  event.things = {h};
  if (i.declaration)
    event.script_author = i.declaration->author;
  event.detail = {{"recipient", "p:" + std::to_string(recipient)},
                  {"location", present ? "inventory" : "mailbox"}};
  i.effects->ordered.push_back(std::move(event));
  i.effects->changed |= changed;
  lua_createtable(L, 0, 4);
  lua_pushstring(L, "held");
  lua_setfield(L, -2, "effect");
  react::push_handle(L, h);
  lua_setfield(L, -2, "object");
  auto account = "p:" + std::to_string(recipient);
  lua_pushlstring(L, account.data(), account.size());
  lua_setfield(L, -2, "recipient");
  lua_pushstring(L, present ? "inventory" : "mailbox");
  lua_setfield(L, -2, "location");
  return 1;
}
} // namespace amber::bindings
