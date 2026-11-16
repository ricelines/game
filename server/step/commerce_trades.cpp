#include "commerce_private.hpp"

namespace amber::step {
step::NativeCompletion trade_open(perm::Trades &trades, const perm::Players &players,
                                  IdentityRead identity, const DeliveryTargets &delivery,
                                  const step::NativeCall &call) {
  const auto who = commerce_detail::player(call);
  const auto other = commerce_detail::player_id(call.arguments.at("other"), players);
  if (!commerce_detail::present(delivery, identity, who) ||
      !commerce_detail::present(delivery, identity, other))
    return commerce_detail::denied(perm::Reason::NotPresent, "presence",
                                   "p:" + std::to_string(other));
  const auto opened = perm::open_trade(trades, players, who, other);
  if (!opened)
    return commerce_detail::denied(opened.reason, "trade", "p:" + std::to_string(other));
  return commerce_detail::result(
      true, {{"trade", "trade:" + std::to_string(opened.trade)},
             {"players", {"p:" + std::to_string(who), "p:" + std::to_string(other)}}});
}
step::NativeCompletion trade_change(amber::perm::State &perm_state,
                                    amber::ObjectState &objects_state, const space::Space &space,
                                    const step::NativeCall &call) {
  auto &trades = perm_state.trades;
  auto &offers = perm_state.offers;
  auto flags = amber::permission_flags(objects_state.objects);

  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto &limits = perm_state.owner_limits;
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto parents = amber::parents(std::as_const(objects_state.objects));
  auto &parts = objects_state.parts;
  auto &declared_parts = objects_state.declared_parts;

  const auto who = commerce_detail::player(call);
  const auto id = step::identifier(call.arguments.at("trade"), "trade:");
  const auto &trade = commerce_detail::trade_row(trades, id, who);
  const auto &before = trade.sides[who == trade.players[0] ? 0 : 1];
  const auto old_goods =
      before.offer && offers.rows[before.offer].live ? offers.rows[before.offer].goods : Handle{};
  std::optional<commerce_detail::Goods> item;
  bool in_world = false;
  if (call.arguments.contains("goods") && !call.arguments.at("goods").is_null()) {
    const auto h = step::live_handle(call.arguments.at("goods"), identity);
    item = commerce_detail::goods(identity, parts, declared_parts, authority, limits,
                                  perm_state.plots, h);
    in_world = space::body_motion(space, identity, h).has_value() ||
               (space::is_logical_root(space, identity, h) && !(item->root.flags & Held));
    if (h != old_goods)
      commerce_detail::check_custody(space, identity, parents, parts, {h, {}, in_world});
  }
  const auto changed = perm::change_trade(
      perm_state, flags, id, who, item ? &item->root : nullptr,
      call.arguments.at("money").get<std::int64_t>(), in_world, call.step,
      item ? std::span<const perm::Target>{item->parts} : std::span<const perm::Target>{});
  if (!changed)
    return commerce_detail::denied(
        changed.reason, "transfer",
        item ? to_string(item->root.object) : "trade:" + std::to_string(id), who);
  auto out = commerce_detail::result(
      true, {{"trade", "trade:" + std::to_string(id)}, {"confirmations_reset", true}});
  out.custody = changed.custody;
  std::vector<Handle> affected;
  if (old_goods)
    affected.push_back(old_goods);
  if (item)
    affected.insert(affected.end(), item->closure.begin(), item->closure.end());
  out.changed_objects = affected;
  commerce_detail::record(out, call, "trade_change", std::move(affected), out.reply);
  return out;
}
step::NativeCompletion trade_confirm(amber::perm::State &perm_state,
                                     amber::ObjectState &objects_state, const space::Space &space,
                                     const DeliveryTargets &delivery,
                                     const step::NativeCall &call) {
  auto &trades = perm_state.trades;
  auto &offers = perm_state.offers;

  auto &grants = perm_state.grants;

  auto authority = amber::authority(objects_state.objects);

  auto &limits = perm_state.owner_limits;
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto parents = amber::parents(std::as_const(objects_state.objects));
  auto &parts = objects_state.parts;
  auto &declared_parts = objects_state.declared_parts;

  const auto who = commerce_detail::player(call);
  const auto id = step::identifier(call.arguments.at("trade"), "trade:");
  const auto &trade = commerce_detail::trade_row(trades, id, who);
  const auto own_side = who == trade.players[0] ? 0 : 1;
  const bool previously = trade.sides[own_side].confirmed;
  std::array<Handle, 2> bodies;
  std::array<perm::Target, 2> targets{};
  std::array<std::vector<perm::Target>, 2> part_targets;
  std::array<std::span<const perm::Target>, 2> spans;
  std::vector<Handle> affected;
  for (std::size_t side = 0; side < 2; ++side) {
    bodies[side] = commerce_detail::present(delivery, identity, trade.players[side]);
    if (!bodies[side])
      return commerce_detail::denied(perm::Reason::NotPresent, "presence",
                                     "p:" + std::to_string(trade.players[side]));
  }
  for (std::size_t side = 0; side < 2; ++side) {
    const auto offer_id = trade.sides[side].offer;
    if (offer_id && offers.rows.at(offer_id).goods) {
      const auto item = commerce_detail::goods(identity, parts, declared_parts, authority, limits,
                                               perm_state.plots, offers.rows[offer_id].goods);
      targets[side] = item.root;
      part_targets[side] = item.parts;
      affected.insert(affected.end(), item.closure.begin(), item.closure.end());
      commerce_detail::check_custody(space, identity, parents, parts,
                                     {item.root.object, bodies[1 - side], false});
    }
    spans[side] = part_targets[side];
  }
  const auto confirmed =
      perm::confirm_trade(perm_state, objects_state, id, who, targets, bodies, call.step, spans);
  if (!confirmed)
    return commerce_detail::denied(confirmed.reason, "confirm", "trade:" + std::to_string(id), who);
  auto out = commerce_detail::result(
      confirmed.completed || !previously,
      {{"trade", "trade:" + std::to_string(id)}, {"completed", confirmed.completed}});
  out.custody = confirmed.custody;
  out.changed_objects = affected;
  commerce_detail::record(out, call, confirmed.completed ? "trade_complete" : "trade_confirm",
                          std::move(affected),
                          {{"trade", id},
                           {"completed", confirmed.completed},
                           {"cleared", commerce_detail::grants(confirmed.revoked)}});
  if (confirmed.completed)
    for (const auto &target : targets)
      if (target.object) {
        const auto objects = lifecycle::collect(parts, identity, target.object);
        commerce_detail::cleared_notice(out, grants, confirmed.revoked, target.owner, target.object,
                                        objects);
      }
  out.permissions_changed = !confirmed.revoked.empty();
  return out;
}
step::NativeCompletion trade_cancel(perm::Trades &trades, perm::Offers &offers,
                                    PermissionFlags flags, IdentityRead identity,
                                    const lifecycle::Parts &parts, const step::NativeCall &call) {
  const auto who = commerce_detail::player(call);
  const auto id = step::identifier(call.arguments.at("trade"), "trade:");
  const auto &trade = commerce_detail::trade_row(trades, id, who);
  std::vector<Handle> affected;
  for (const auto &side : trade.sides)
    if (side.offer && offers.rows.at(side.offer).live && offers.rows[side.offer].goods) {
      const auto closure = lifecycle::collect(parts, identity, offers.rows[side.offer].goods);
      affected.insert(affected.end(), closure.begin(), closure.end());
    }
  const auto cancelled = perm::cancel_trade(trades, offers, flags, id, who);
  if (!cancelled)
    return commerce_detail::denied(cancelled.reason, "cancel", "trade:" + std::to_string(id), who);
  auto out = commerce_detail::result(true, {{"trade", "trade:" + std::to_string(id)}});
  out.changed_objects = affected;
  commerce_detail::record(out, call, "trade_cancel", std::move(affected), out.reply);
  return out;
}
} // namespace amber::step
