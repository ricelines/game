#include "step.hpp"

#include <algorithm>
#include <lua.h>
#include <lualib.h>

namespace amber::step {
std::string validate_hold_goods(const nlohmann::json &input) {
  return input.is_object() && input.empty() ? "" : "hold_goods takes no inputs";
}
step::NativeCompletion hold_goods(perm::Players &players, const perm::Grants &grants,
                                  const perm::OwnerLimits &limits, IdentityRead identity,
                                  AuthorityRead authority, ParentRead parents, SpaceFlags flags,
                                  const Relation &declared_parts, const DeliveryTargets &delivery,
                                  const space::Space &space, std::size_t mailbox_max,
                                  const step::NativeCall &call) {
  const auto h = call.request.target;
  const auto recipient = authority.owner[h.slot()];
  const auto deny = [&](std::string_view reason) {
    step::NativeCompletion out;
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
  if (const auto *parents = declared_parts.in.get(h.slot()); parents && !parents->empty())
    return deny(perm::reason_text(perm::Reason::InvalidScope));
  std::vector<Handle> descendants;
  if (const auto *children = declared_parts.out.get(h.slot()))
    descendants = *children;
  for (std::size_t index = 0; index < descendants.size(); ++index) {
    const auto child = descendants[index];
    if (!identity.valid(child))
      return deny(perm::reason_text(perm::Reason::StaleGoods));
    if (const auto *children = declared_parts.out.get(child.slot()))
      descendants.insert(descendants.end(), children->begin(), children->end());
  }
  std::vector<perm::Target> parts;
  for (auto child : descendants)
    parts.push_back(perm::target(identity, authority, limits, child));
  const auto delivered = delivery.present_bodies.find(recipient);
  const bool present =
      delivered != delivery.present_bodies.end() && identity.valid(delivered->second);
  const auto custodian = present ? delivered->second : Handle{};
  const auto holder =
      std::holds_alternative<PlayerId>(call.request.caller)
          ? perm::Holder::player(std::get<PlayerId>(call.request.caller))
          : perm::Holder::script(std::get<ScriptHolder>(call.request.caller).script,
                                 std::get<ScriptHolder>(call.request.caller).object);
  if (!recipient || recipient >= players.rows.size())
    return deny(perm::reason_text(perm::Reason::UnknownHolder));
  const auto &player = players.rows[recipient];
  const auto &stored = present ? player.inventory : player.mailbox;
  const bool changed = !(flags.get(h.slot()) & Held) || parents.parent[h.slot()] != custodian ||
                       std::ranges::find(stored, h) == stored.end();
  if (changed) {
    const auto ready = space::can_custody(space, identity, h, custodian, parents, descendants);
    if (ready.status != space::Status::Ok)
      return deny(space::status_text(ready.status));
  }
  const auto stored_goods =
      perm::store_goods(players, grants, holder, perm::target(identity, authority, limits, h),
                        parts, recipient, custodian, present, mailbox_max, call.step);
  if (!stored_goods)
    return deny(perm::reason_text(stored_goods.reason));
  step::NativeCompletion out;
  if (changed) {
    out.custody = stored_goods.custody;
    out.changed_objects = descendants;
    out.changed_objects.push_back(h);
  }
  out.changed = changed;
  out.result = changed ? record::Result::Applied : record::Result::NoEffect;
  out.reply = {{"ok", true},
               {"effect", changed ? "applied" : "none"},
               {"object", to_string(h)},
               {"recipient", "p:" + std::to_string(recipient)},
               {"location", present ? "inventory" : "mailbox"}};
  return out;
}
void declare_hold_goods_action(react::Terms &terms, react::NativeActions &declarations) {
  react::declare_native(
      declarations, terms, "engine.hold_goods",
      "Store the selected item for its existing owner, using Transfer permission.", "transfer",
      false,
      {{"type", "object"},
       {"properties", nlohmann::json::object()},
       {"additionalProperties", false}},
      react::ExchangeAction::HoldGoods);
}
} // namespace amber::step
