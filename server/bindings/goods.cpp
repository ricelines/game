#include "goods.hpp"
#include "arguments/arguments.hpp"
#include "scene.hpp"

#include <algorithm>
#include <lua.h>
#include <lualib.h>

namespace amber::bindings {
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
namespace {
std::string hold_inputs(const nlohmann::json &input) {
  return input.is_object() && input.empty() ? "" : "hold_goods takes no inputs";
}
drive::NativeCompletion hold_goods(void *context, const drive::NativeCall &call) {
  auto &b = *static_cast<HoldGoodsBinding *>(context);
  const auto h = call.request.target;
  const auto recipient = b.authority.owner[h.slot()];
  const auto deny = [&](std::string_view reason) {
    drive::NativeCompletion out;
    out.result = record::Result::Denied;
    out.reply = {{"ok", false},
                 {"denied",
                  {{"need", "transfer"},
                   {"on", to_string(h)},
                   {"why", reason},
                   {"fix", "ask p:" + std::to_string(recipient) +
                               " for Transfer on the item and its declared parts"}}}};
    return out;
  };
  if (const auto *parents = b.declared_parts.in.get(h.slot()); parents && !parents->empty())
    return deny(perm::reason_text(perm::Reason::InvalidScope));
  std::vector<Handle> descendants;
  if (const auto *children = b.declared_parts.out.get(h.slot()))
    descendants = *children;
  for (std::size_t index = 0; index < descendants.size(); ++index) {
    const auto child = descendants[index];
    if (!b.identity.valid(child))
      return deny(perm::reason_text(perm::Reason::StaleGoods));
    if (const auto *children = b.declared_parts.out.get(child.slot()))
      descendants.insert(descendants.end(), children->begin(), children->end());
  }
  std::vector<perm::Target> parts;
  for (auto child : descendants)
    parts.push_back(perm::target(b.identity, b.authority, b.limits, child));
  const auto delivered = b.delivery.present_bodies.find(recipient);
  const bool present =
      delivered != b.delivery.present_bodies.end() && b.identity.valid(delivered->second);
  const auto custodian = present ? delivered->second : Handle{};
  const auto holder =
      std::holds_alternative<PlayerId>(call.request.caller)
          ? perm::Holder::player(std::get<PlayerId>(call.request.caller))
          : perm::Holder::script(std::get<ScriptHolder>(call.request.caller).script,
                                 std::get<ScriptHolder>(call.request.caller).object);
  if (!recipient || recipient >= b.players.rows.size())
    return deny(perm::reason_text(perm::Reason::UnknownHolder));
  const auto &player = b.players.rows[recipient];
  const auto &stored = present ? player.inventory : player.mailbox;
  const bool changed = !(b.flags.get(h.slot()) & Held) || b.parents.parent[h.slot()] != custodian ||
                       std::ranges::find(stored, h) == stored.end();
  if (changed) {
    const auto ready =
        space::can_custody(b.space, b.identity, h, custodian, b.parents, descendants);
    if (ready.status != space::Status::Ok)
      return deny(space::status_text(ready.status));
  }
  const auto stored_goods = perm::store_goods(
      b.players, b.grants, holder, perm::target(b.identity, b.authority, b.limits, h), parts,
      recipient, custodian, present, b.mailbox_max, call.step);
  if (!stored_goods)
    return deny(perm::reason_text(stored_goods.reason));
  if (changed) {
    // The readonly custody check passed; only player lists changed since then.
    [[maybe_unused]] const auto result =
        space::custody(b.space, b.identity, h, custodian, b.parents, b.flags, b.relations,
                       b.relation_changes, descendants);
    assert(result.status == space::Status::Ok);
    auto closure = descendants;
    closure.push_back(h);
    perm::update_plots(b.identity, b.transforms, b.plot_write, b.plots, closure, false);
    scene_visible(b.scene, closure, false);
  }
  drive::NativeCompletion out;
  out.changed = changed;
  out.result = changed ? record::Result::Applied : record::Result::NoEffect;
  out.reply = {{"ok", true},
               {"effect", changed ? "applied" : "none"},
               {"object", to_string(h)},
               {"recipient", "p:" + std::to_string(recipient)},
               {"location", present ? "inventory" : "mailbox"}};
  return out;
}
} // namespace
void register_hold_goods_action(react::Terms &terms, command::NativeActions &declarations,
                                drive::NativeOperations &operations, HoldGoodsBinding &binding) {
  const auto id = command::declare_native(
      declarations, terms, "engine.hold_goods",
      "Store the selected item for its existing owner, using Transfer permission.", "transfer",
      false,
      {{"type", "object"},
       {"properties", nlohmann::json::object()},
       {"additionalProperties", false}});
  drive::register_native(operations, id, {hold_inputs, hold_goods, &binding});
}
} // namespace amber::bindings
