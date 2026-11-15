#include "private.hpp"

namespace amber::bindings::commerce {
std::vector<OfferExpired> advance_offer_expiry(OfferExpiryBinding &b, Step now,
                                               std::size_t max_entries) {
  std::vector<OfferExpired> completed;
  for (auto id : perm::advance_offers(b.offers, b.identity, b.flags, now, max_entries)) {
    const auto &row = b.offers.rows[id];
    OfferExpired expired{id, row.seller, row.goods, {}};
    if (b.identity.valid(row.goods))
      expired.cleared.push_back(row.goods);
    for (auto part : row.parts)
      if (b.identity.valid(part))
        expired.cleared.push_back(part);
    completed.push_back(std::move(expired));
  }
  return completed;
}
drive::NativeCompletion give(void *local, const drive::NativeCall &call) {
  auto &b = *static_cast<GiveBinding *>(local);
  const auto item = detail::goods(b.custody, b.authority, b.limits, call.request.target);
  const auto recipient = detail::player_id(call.arguments.at("recipient"), b.players);
  const auto body = detail::present(b.delivery, b.custody.identity, recipient);
  detail::preflight(b.custody, {item.root.object, body, false});
  const auto given =
      perm::give(b.players, b.grants, b.authority, b.flags, b.limits, detail::actor(call),
                 item.root, recipient, body, bool(body), b.mailbox_max, call.step, item.parts);
  if (!given)
    return detail::denied(given.reason, "transfer", to_string(item.root.object), item.root.owner);
  auto out = detail::result(true, {{"goods", to_string(item.root.object)},
                                   {"recipient", "p:" + std::to_string(recipient)},
                                   {"location", body ? "inventory" : "mailbox"},
                                   {"revoked", detail::grants(given.revoked)}});
  detail::apply(b.custody, given.custody, out.effects);
  detail::record(out, call, "give", item.closure, out.reply);
  return out;
}
drive::NativeCompletion offer(void *local, const drive::NativeCall &call) {
  auto &b = *static_cast<OfferBinding *>(local);
  const auto item = detail::goods(b.custody, b.authority, b.limits, call.request.target);
  const auto buyer = call.arguments.contains("buyer")
                         ? detail::player_id(call.arguments.at("buyer"), b.players)
                         : 0;
  const auto duration =
      call.arguments.contains("expires_s")
          ? authority::duration_steps(call.arguments.at("expires_s").get<double>(), call.hz)
          : 0;
  const bool placed =
      space::body_motion(b.custody.space, b.custody.identity, item.root.object).has_value() ||
      (space::is_logical_root(b.custody.space, b.custody.identity, item.root.object) &&
       !(item.root.flags & Held));
  // Permission operation rejects unplaced goods; avoid asking Space to reveal them.
  if (!placed || (item.root.flags & Held))
    return detail::denied(perm::Reason::NotPlaced, "placed", to_string(item.root.object),
                          item.root.owner);
  detail::preflight(b.custody, {item.root.object, {}, true});
  const auto offered =
      perm::offer(b.offers, b.flags, detail::actor(call), item.root, b.players, b.grants,
                  call.arguments.at("price").get<std::int64_t>(), buyer,
                  duration ? call.step + duration : 0, true, call.step, item.parts);
  if (!offered)
    return detail::denied(offered.reason, "transfer", to_string(item.root.object), item.root.owner);
  auto out = detail::result(true, {{"offer", "offer:" + std::to_string(offered.offer)},
                                   {"goods", to_string(item.root.object)}});
  detail::apply(b.custody, offered.custody, out.effects);
  detail::record(out, call, "offer", item.closure,
                 {{"offer", offered.offer}, {"price", call.arguments.at("price")}});
  return out;
}
drive::NativeCompletion offers(void *local, const drive::NativeCall &call) {
  const auto &b = *static_cast<const OffersBinding *>(local);
  const auto player = detail::controller(call, b.identity, b.authority, b.players);
  auto rows = Json::array();
  for (const auto &row : b.offers.rows)
    if (row.id && row.live && !row.trade && (!row.expires_step || row.expires_step > call.step) &&
        (!call.request.target || call.request.target == row.goods) &&
        (!row.buyer || row.buyer == player || row.seller == player))
      rows.push_back({{"id", "offer:" + std::to_string(row.id)},
                      {"goods", to_string(row.goods)},
                      {"seller", "p:" + std::to_string(row.seller)},
                      {"buyer", row.buyer ? Json("p:" + std::to_string(row.buyer)) : Json(nullptr)},
                      {"price", row.price},
                      {"expires_step", row.expires_step}});
  return detail::result(false, {{"offers", std::move(rows)}});
}
drive::NativeCompletion accept(void *local, const drive::NativeCall &call) {
  auto &b = *static_cast<AcceptBinding *>(local);
  const auto id = authority::identifier(call.arguments.at("offer"), "offer:");
  const auto &row = detail::offer_row(b.offers, id);
  const auto item = detail::goods(b.custody, b.authority, b.limits, row.goods);
  const auto beneficiary = call.arguments.contains("beneficiary")
                               ? detail::player_id(call.arguments.at("beneficiary"), b.players)
                               : detail::player(call);
  const authority::AccountRead accounts{b.custody.identity, b.authority, b.players, b.groups};
  const auto payer = call.arguments.contains("payer")
                         ? authority::account(call.arguments.at("payer"), accounts)
                         : detail::default_account(call, accounts);
  const auto body = detail::present(b.delivery, b.custody.identity, beneficiary);
  detail::preflight(b.custody, {row.goods, body, false});
  const auto bought =
      perm::accept(b.offers, b.ledger, b.grants, b.players, b.authority, b.flags, b.limits,
                   {detail::actor(call), beneficiary, payer, body, bool(body)}, item.root, id,
                   b.mailbox_max, call.step, item.parts);
  if (!bought) {
    const bool account = bought.reason == perm::Reason::MissingRight ||
                         bought.reason == perm::Reason::Expired ||
                         bought.reason == perm::Reason::Allowance ||
                         bought.reason == perm::Reason::InsufficientBalance;
    return detail::denied(bought.reason, account ? "spend" : "accept",
                          account ? perm::account_name(payer.account)
                                  : "offer:" + std::to_string(id),
                          account ? payer.controller : row.seller);
  }
  auto out = detail::result(true, {{"goods", to_string(item.root.object)},
                                   {"recipient", "p:" + std::to_string(beneficiary)},
                                   {"location", body ? "inventory" : "mailbox"},
                                   {"revoked", detail::grants(bought.revoked)}});
  detail::apply(b.custody, bought.custody, out.effects);
  detail::record(out, call, "accept", item.closure,
                 {{"offer", id},
                  {"price", row.price},
                  {"payer", perm::account_name(payer.account)},
                  {"seller", row.seller}});
  return out;
}
drive::NativeCompletion cancel_offer(void *local, const drive::NativeCall &call) {
  auto &b = *static_cast<OfferBinding *>(local);
  const auto id = authority::identifier(call.arguments.at("offer"), "offer:");
  const auto &row = detail::offer_row(b.offers, id);
  const auto item = detail::goods(b.custody, b.authority, b.limits, row.goods);
  detail::preflight(b.custody, {row.goods, {}, true});
  const auto cancelled = perm::cancel_offer(b.offers, b.flags, detail::actor(call), item.root,
                                            b.players, b.grants, id, call.step, item.parts);
  if (!cancelled)
    return detail::denied(cancelled.reason, "transfer", to_string(item.root.object),
                          item.root.owner);
  auto out = detail::result(true, {{"offer", "offer:" + std::to_string(id)}});
  detail::apply(b.custody, cancelled.custody, out.effects);
  detail::record(out, call, "cancel_offer", item.closure, out.reply);
  return out;
}
} // namespace amber::bindings::commerce
