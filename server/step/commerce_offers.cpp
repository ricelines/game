#include "commerce_private.hpp"

namespace amber::step {
std::vector<OfferExpired> advance_offer_expiry(perm::Offers &offers, IdentityRead identity,
                                               PermissionFlags flags, Step now,
                                               std::size_t max_entries) {
  std::vector<OfferExpired> completed;
  for (auto id : perm::advance_offers(offers, identity, flags, now, max_entries)) {
    const auto &row = offers.rows[id];
    OfferExpired expired{id, row.seller, row.goods, {}};
    if (identity.valid(row.goods))
      expired.cleared.push_back(row.goods);
    for (auto part : row.parts)
      if (identity.valid(part))
        expired.cleared.push_back(part);
    completed.push_back(std::move(expired));
  }
  return completed;
}
step::NativeCompletion give(amber::perm::State &perm_state, amber::ObjectState &objects_state,
                            const space::Space &space, const DeliveryTargets &delivery,
                            std::size_t mailbox_max, const step::NativeCall &call) {
  auto &players = perm_state.players;
  auto authority = amber::authority(objects_state.objects);

  auto &limits = perm_state.owner_limits;
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto parents = amber::parents(std::as_const(objects_state.objects));
  auto &parts = objects_state.parts;
  auto &declared_parts = objects_state.declared_parts;

  const auto item = commerce_detail::goods(identity, parts, declared_parts, authority, limits,
                                           perm_state.plots, call.request.target);
  const auto recipient = commerce_detail::player_id(call.arguments.at("recipient"), players);
  const auto body = commerce_detail::present(delivery, identity, recipient);
  commerce_detail::check_custody(space, identity, parents, parts, {item.root.object, body, false});
  const auto given = perm::give(perm_state, objects_state, commerce_detail::actor(call), item.root,
                                recipient, body, bool(body), mailbox_max, call.step, item.parts);
  if (!given)
    return commerce_detail::denied(given.reason, "transfer", to_string(item.root.object),
                                   item.root.owner);
  auto out = commerce_detail::result(true, {{"goods", to_string(item.root.object)},
                                            {"recipient", "p:" + std::to_string(recipient)},
                                            {"location", body ? "inventory" : "mailbox"},
                                            {"revoked", commerce_detail::grants(given.revoked)}});
  out.custody = given.custody;
  out.changed_objects = item.closure;
  commerce_detail::record(out, call, "give", item.closure, out.reply);
  out.permissions_changed = !out.reply.value("revoked", Json::array()).empty();
  return out;
}
step::NativeCompletion offer(amber::perm::State &perm_state, amber::ObjectState &objects_state,
                             const space::Space &space, const step::NativeCall &call) {

  auto flags = amber::permission_flags(objects_state.objects);
  auto &players = perm_state.players;

  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto &limits = perm_state.owner_limits;
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto parents = amber::parents(std::as_const(objects_state.objects));
  auto &parts = objects_state.parts;
  auto &declared_parts = objects_state.declared_parts;

  const auto item = commerce_detail::goods(identity, parts, declared_parts, authority, limits,
                                           perm_state.plots, call.request.target);
  const auto buyer = call.arguments.contains("buyer")
                         ? commerce_detail::player_id(call.arguments.at("buyer"), players)
                         : 0;
  const auto duration =
      call.arguments.contains("expires_s")
          ? step::duration_steps(call.arguments.at("expires_s").get<double>(), call.hz)
          : 0;
  const bool placed =
      space::body_motion(space, identity, item.root.object).has_value() ||
      (space::is_logical_root(space, identity, item.root.object) && !(item.root.flags & Held));
  // Permission operation rejects unplaced goods; avoid asking Space to reveal them.
  if (!placed || (item.root.flags & Held))
    return commerce_detail::denied(perm::Reason::NotPlaced, "placed", to_string(item.root.object),
                                   item.root.owner);
  commerce_detail::check_custody(space, identity, parents, parts, {item.root.object, {}, true});
  const auto offered =
      perm::offer(perm_state, flags, commerce_detail::actor(call), item.root,
                  call.arguments.at("price").get<std::int64_t>(), buyer,
                  duration ? call.step + duration : 0, true, call.step, item.parts);
  if (!offered)
    return commerce_detail::denied(offered.reason, "transfer", to_string(item.root.object),
                                   item.root.owner);
  auto out = commerce_detail::result(true, {{"offer", "offer:" + std::to_string(offered.offer)},
                                            {"goods", to_string(item.root.object)}});
  out.custody = offered.custody;
  out.changed_objects = item.closure;
  commerce_detail::record(out, call, "offer", item.closure,
                          {{"offer", offered.offer}, {"price", call.arguments.at("price")}});
  return out;
}
step::NativeCompletion offers(const perm::Offers &offers, const perm::Players &players,
                              IdentityRead identity, AuthorityRead authority,
                              const step::NativeCall &call) {
  const auto player = commerce_detail::controller(call, identity, authority, players);
  auto rows = Json::array();
  for (const auto &row : offers.rows)
    if (row.id && row.live && !row.trade && (!row.expires_step || row.expires_step > call.step) &&
        (!call.request.target || call.request.target == row.goods) &&
        (!row.buyer || row.buyer == player || row.seller == player))
      rows.push_back({{"id", "offer:" + std::to_string(row.id)},
                      {"goods", to_string(row.goods)},
                      {"seller", "p:" + std::to_string(row.seller)},
                      {"buyer", row.buyer ? Json("p:" + std::to_string(row.buyer)) : Json(nullptr)},
                      {"price", row.price},
                      {"expires_step", row.expires_step}});
  return commerce_detail::result(false, {{"offers", std::move(rows)}});
}
step::NativeCompletion accept(amber::perm::State &perm_state, amber::ObjectState &objects_state,
                              const space::Space &space, const DeliveryTargets &delivery,
                              std::size_t mailbox_max, const step::NativeCall &call) {
  auto &offers = perm_state.offers;

  auto &grants = perm_state.grants;
  auto &players = perm_state.players;
  auto authority = amber::authority(objects_state.objects);

  auto &limits = perm_state.owner_limits;
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto parents = amber::parents(std::as_const(objects_state.objects));
  auto &parts = objects_state.parts;
  auto &declared_parts = objects_state.declared_parts;
  auto &groups = perm_state.groups;

  const auto id = step::identifier(call.arguments.at("offer"), "offer:");
  const auto &row = commerce_detail::offer_row(offers, id);
  const auto item = commerce_detail::goods(identity, parts, declared_parts, authority, limits,
                                           perm_state.plots, row.goods);
  const auto beneficiary =
      call.arguments.contains("beneficiary")
          ? commerce_detail::player_id(call.arguments.at("beneficiary"), players)
          : commerce_detail::player(call);
  const auto payer =
      call.arguments.contains("payer")
          ? step::account(call.arguments.at("payer"), identity, authority, players, groups)
          : commerce_detail::default_account(call, identity, authority, players, groups);
  const auto body = commerce_detail::present(delivery, identity, beneficiary);
  commerce_detail::check_custody(space, identity, parents, parts, {row.goods, body, false});
  const auto bought =
      perm::accept(perm_state, objects_state,
                   {commerce_detail::actor(call), beneficiary, payer, body, bool(body)}, item.root,
                   id, mailbox_max, call.step, item.parts);
  if (!bought) {
    const bool account = bought.reason == perm::Reason::MissingRight ||
                         bought.reason == perm::Reason::Expired ||
                         bought.reason == perm::Reason::Allowance ||
                         bought.reason == perm::Reason::InsufficientBalance;
    return commerce_detail::denied(bought.reason, account ? "spend" : "accept",
                                   account ? perm::account_name(payer.account)
                                           : "offer:" + std::to_string(id),
                                   account ? payer.controller : row.seller);
  }
  auto out = commerce_detail::result(true, {{"goods", to_string(item.root.object)},
                                            {"recipient", "p:" + std::to_string(beneficiary)},
                                            {"location", body ? "inventory" : "mailbox"}});
  out.custody = bought.custody;
  out.changed_objects = item.closure;
  commerce_detail::record(out, call, "accept", item.closure,
                          {{"offer", id},
                           {"price", row.price},
                           {"payer", perm::account_name(payer.account)},
                           {"seller", row.seller},
                           {"cleared", commerce_detail::grants(bought.revoked)}});
  commerce_detail::cleared_notice(out, grants, bought.revoked, item.root.owner, item.root.object,
                                  item.closure);
  out.permissions_changed = !bought.revoked.empty();
  return out;
}
step::NativeCompletion cancel_offer(amber::perm::State &perm_state,
                                    amber::ObjectState &objects_state,
                                    const step::NativeCall &call) {
  auto &offers = perm_state.offers;
  auto flags = amber::permission_flags(objects_state.objects);

  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto &limits = perm_state.owner_limits;
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto &parts = objects_state.parts;
  auto &declared_parts = objects_state.declared_parts;

  const auto id = step::identifier(call.arguments.at("offer"), "offer:");
  const auto &row = commerce_detail::offer_row(offers, id);
  const auto item = commerce_detail::goods(identity, parts, declared_parts, authority, limits,
                                           perm_state.plots, row.goods);
  const auto cancelled = perm::cancel_offer(perm_state, flags, commerce_detail::actor(call),
                                            item.root, id, call.step, item.parts);
  if (!cancelled)
    return commerce_detail::denied(cancelled.reason, "transfer", to_string(item.root.object),
                                   item.root.owner);
  auto out = commerce_detail::result(true, {{"offer", "offer:" + std::to_string(id)}});
  out.changed_objects = item.closure;
  commerce_detail::record(out, call, "cancel_offer", item.closure, out.reply);
  return out;
}
void expire_sale_offers(perm::State &perm_state, IdentityRead ids, PermissionFlags flags,
                        net::SessionViews &session_views, net::Sessions &sessions,
                        record::Journal &journal, Step now, std::size_t limit,
                        std::size_t mailbox_max) {
  for (const auto &expired : advance_offer_expiry(perm_state.offers, ids, flags, now, limit)) {
    net::invalidate(session_views, expired.cleared);
    Json notice{{"kind", "offer_expired"},
                {"offer", "offer:" + std::to_string(expired.offer)},
                {"goods", to_string(expired.goods)},
                {"say", "The sale offer expired; its goods are available again."}};
    notify(perm_state.players, sessions, mailbox_max, expired.seller, notice);
    record::Event event;
    event.step = now;
    event.kind = record::Kind::System;
    event.name = "offer_expired";
    event.changed = true;
    event.result = record::Result::Applied;
    event.things = expired.cleared;
    event.detail = std::move(notice);
    event.detail.erase("kind");
    event.detail["agent"] = false;
    journal.append(std::move(event));
  }
}
} // namespace amber::step
