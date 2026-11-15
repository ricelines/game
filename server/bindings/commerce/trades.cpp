#include "private.hpp"

namespace amber::bindings::commerce {
drive::NativeCompletion trade_open(void *local, const drive::NativeCall &call) {
  auto &b = *static_cast<TradeOpenBinding *>(local);
  const auto who = detail::player(call);
  const auto other = detail::player_id(call.arguments.at("other"), b.players);
  if (!detail::present(b.delivery, b.identity, who) ||
      !detail::present(b.delivery, b.identity, other))
    return detail::denied(perm::Reason::NotPresent, "presence", "p:" + std::to_string(other));
  const auto opened = perm::open_trade(b.trades, b.players, who, other);
  if (!opened)
    return detail::denied(opened.reason, "trade", "p:" + std::to_string(other));
  return detail::result(true,
                        {{"trade", "trade:" + std::to_string(opened.trade)},
                         {"players", {"p:" + std::to_string(who), "p:" + std::to_string(other)}}});
}
drive::NativeCompletion trade_change(void *local, const drive::NativeCall &call) {
  auto &b = *static_cast<TradeChangeBinding *>(local);
  const auto who = detail::player(call);
  const auto id = authority::identifier(call.arguments.at("trade"), "trade:");
  const auto &trade = detail::trade_row(b.trades, id, who);
  const auto &before = trade.sides[who == trade.players[0] ? 0 : 1];
  const auto old_goods = before.offer && b.offers.rows[before.offer].live
                             ? b.offers.rows[before.offer].goods
                             : Handle{};
  std::optional<detail::Goods> item;
  perm::CustodyChange prior;
  if (call.arguments.contains("goods") && !call.arguments.at("goods").is_null()) {
    const auto h = authority::live_handle(call.arguments.at("goods"), b.custody.identity);
    item = detail::goods(b.custody, b.authority, b.limits, h);
    const bool in_world = space::body_motion(b.custody.space, b.custody.identity, h).has_value() ||
                          (space::is_logical_root(b.custody.space, b.custody.identity, h) &&
                           !(item->root.flags & Held));
    prior = {h, b.custody.parents.parent[h.slot()], in_world};
    if (h == old_goods)
      prior = before.prior_custody;
    detail::preflight(b.custody, {h, {}, prior.in_world});
  }
  if (old_goods && (!item || item->root.object != old_goods))
    detail::preflight(b.custody, before.prior_custody);
  const auto changed = perm::change_trade(
      b.trades, b.offers, b.flags, b.players, b.grants, id, who, item ? &item->root : nullptr,
      call.arguments.at("money").get<std::int64_t>(), prior, call.step,
      item ? std::span<const perm::Target>{item->parts} : std::span<const perm::Target>{});
  if (!changed)
    return detail::denied(changed.reason, "transfer",
                          item ? to_string(item->root.object) : "trade:" + std::to_string(id), who);
  auto out = detail::result(
      true, {{"trade", "trade:" + std::to_string(id)}, {"confirmations_reset", true}});
  detail::apply(b.custody, changed.custody, out.effects);
  std::vector<Handle> affected;
  if (old_goods)
    affected.push_back(old_goods);
  if (item)
    affected.insert(affected.end(), item->closure.begin(), item->closure.end());
  detail::record(out, call, "trade_change", std::move(affected), out.reply);
  return out;
}
drive::NativeCompletion trade_confirm(void *local, const drive::NativeCall &call) {
  auto &b = *static_cast<TradeConfirmBinding *>(local);
  const auto who = detail::player(call);
  const auto id = authority::identifier(call.arguments.at("trade"), "trade:");
  const auto &trade = detail::trade_row(b.trades, id, who);
  const auto own_side = who == trade.players[0] ? 0 : 1;
  const bool previously = trade.sides[own_side].confirmed;
  std::array<Handle, 2> bodies;
  std::array<perm::Target, 2> targets{};
  std::array<std::vector<perm::Target>, 2> parts;
  std::array<std::span<const perm::Target>, 2> spans;
  std::vector<Handle> affected;
  for (std::size_t side = 0; side < 2; ++side) {
    bodies[side] = detail::present(b.delivery, b.custody.identity, trade.players[side]);
    if (!bodies[side])
      return detail::denied(perm::Reason::NotPresent, "presence",
                            "p:" + std::to_string(trade.players[side]));
  }
  for (std::size_t side = 0; side < 2; ++side) {
    const auto offer_id = trade.sides[side].offer;
    if (offer_id && b.offers.rows.at(offer_id).goods) {
      const auto item =
          detail::goods(b.custody, b.authority, b.limits, b.offers.rows[offer_id].goods);
      targets[side] = item.root;
      parts[side] = item.parts;
      affected.insert(affected.end(), item.closure.begin(), item.closure.end());
      detail::preflight(b.custody, {item.root.object, bodies[1 - side], false});
    }
    spans[side] = parts[side];
  }
  const auto confirmed =
      perm::confirm_trade(b.trades, b.offers, b.ledger, b.grants, b.players, b.authority, b.flags,
                          b.limits, id, who, targets, bodies, call.step, spans);
  if (!confirmed)
    return detail::denied(confirmed.reason, "confirm", "trade:" + std::to_string(id), who);
  auto out = detail::result(confirmed.completed || !previously,
                            {{"trade", "trade:" + std::to_string(id)},
                             {"completed", confirmed.completed},
                             {"revoked", detail::grants(confirmed.revoked)}});
  detail::apply(b.custody, confirmed.custody, out.effects);
  detail::record(out, call, confirmed.completed ? "trade_complete" : "trade_confirm",
                 std::move(affected), out.reply);
  return out;
}
drive::NativeCompletion trade_cancel(void *local, const drive::NativeCall &call) {
  auto &b = *static_cast<TradeCancelBinding *>(local);
  const auto who = detail::player(call);
  const auto id = authority::identifier(call.arguments.at("trade"), "trade:");
  const auto &trade = detail::trade_row(b.trades, id, who);
  for (const auto &side : trade.sides)
    if (side.offer && b.offers.rows.at(side.offer).live && b.offers.rows[side.offer].goods)
      detail::preflight(b.custody, side.prior_custody);
  const auto cancelled = perm::cancel_trade(b.trades, b.offers, b.flags, id, who);
  if (!cancelled)
    return detail::denied(cancelled.reason, "cancel", "trade:" + std::to_string(id), who);
  auto out = detail::result(true, {{"trade", "trade:" + std::to_string(id)}});
  detail::apply(b.custody, cancelled.custody, out.effects);
  std::vector<Handle> affected;
  for (const auto &change : cancelled.custody)
    affected.push_back(change.goods);
  detail::record(out, call, "trade_cancel", std::move(affected), out.reply);
  return out;
}
} // namespace amber::bindings::commerce
