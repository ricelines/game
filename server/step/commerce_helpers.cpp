#include "commerce_private.hpp"

#include <algorithm>
#include <cmath>

namespace amber::step::commerce_detail {
perm::Holder actor(const step::NativeCall &call) {
  if (const auto *player = std::get_if<PlayerId>(&call.request.caller))
    return perm::Holder::player(*player);
  const auto script = std::get<ScriptHolder>(call.request.caller);
  return perm::Holder::script(script.script, script.object);
}
PlayerId player(const step::NativeCall &call) {
  if (const auto *player = std::get_if<PlayerId>(&call.request.caller); player && *player)
    return *player;
  throw std::invalid_argument("this action requires a player caller");
}
PlayerId player_id(const Json &input, const perm::Players &players) {
  const auto id = step::identifier(input, "p:");
  if (id >= players.rows.size())
    throw std::invalid_argument("unknown player wallet");
  return id;
}
PlayerId controller(const step::NativeCall &call, IdentityRead identity, AuthorityRead authority,
                    const perm::Players &players) {
  const auto holder = actor(call);
  if (holder.kind == perm::HolderKind::Player)
    return player_id("p:" + std::to_string(holder.id), players);
  if (!identity.valid(holder.installed_on))
    throw std::invalid_argument("script installation is no longer alive");
  const auto owner = authority.owner[holder.installed_on.slot()];
  return player_id("p:" + std::to_string(owner), players);
}
perm::AccountTarget default_account(const step::NativeCall &call, IdentityRead identity,
                                    AuthorityRead authority, const perm::Players &players,
                                    const perm::Groups &groups) {
  const auto holder = actor(call);
  if (holder.kind == perm::HolderKind::Player)
    return step::account("p:" + std::to_string(holder.id), identity, authority, players, groups);
  return step::account(to_string(holder.installed_on), identity, authority, players, groups);
}
Handle present(const DeliveryTargets &delivery, IdentityRead identity, PlayerId player) {
  const auto found = delivery.present_bodies.find(player);
  return found != delivery.present_bodies.end() && identity.valid(found->second) ? found->second
                                                                                 : Handle{};
}
step::NativeCompletion result(bool changed, Json extra) {
  step::NativeCompletion out;
  out.changed = changed;
  out.result = changed ? record::Result::Applied : record::Result::NoEffect;
  out.reply = {{"ok", true}, {"effect", changed ? "applied" : "none"}};
  out.reply.update(extra);
  return out;
}
step::NativeCompletion denied(std::string_view need, Json on, std::string_view why,
                              std::string_view fix) {
  step::NativeCompletion out;
  out.result = record::Result::Denied;
  out.reply = {{"ok", false},
               {"denied", {{"need", need}, {"on", std::move(on)}, {"why", why}, {"fix", fix}}}};
  return out;
}
step::NativeCompletion denied(perm::Reason why, std::string need, std::string on,
                              PlayerId controller) {
  std::string fix;
  switch (why) {
  case perm::Reason::MissingRight:
  case perm::Reason::Expired:
    fix = "ask p:" + std::to_string(controller) + " for " + need + " on " + on;
    break;
  case perm::Reason::Allowance:
    fix = "ask the grant issuer to increase the spending allowance";
    break;
  case perm::Reason::InsufficientBalance:
    fix = "add credits to the paying account or lower the amount";
    break;
  case perm::Reason::MailboxFull:
    fix = "ask the recipient to collect items from their mailbox";
    break;
  case perm::Reason::NotPlaced:
    fix = "place the goods in the world before offering them";
    break;
  case perm::Reason::InEscrow:
    fix = "cancel the goods' active offer or trade first";
    break;
  case perm::Reason::NotPresent:
    fix = "both participants must connect before trading";
    break;
  case perm::Reason::NotParticipant:
  case perm::Reason::WrongBuyer:
    fix = "ask the participants to change or create an offer for you";
    break;
  case perm::Reason::OfferGone:
  case perm::Reason::StaleGoods:
    fix = "choose currently available goods or an active offer";
    break;
  case perm::Reason::NotGoods:
    fix = "choose an object that is not a player body";
    break;
  case perm::Reason::InheritedLimit:
  case perm::Reason::NoCopy:
    fix = "choose goods whose next-owner permissions allow this exchange";
    break;
  case perm::Reason::PriceChanged:
    fix = "inspect the offer and accept again with a max_price of at least its price";
    break;
  case perm::Reason::NotController:
    fix = "use goods from your inventory or ask their owner to place them";
    break;
  default:
    fix = "correct the supplied " + need + " for " + on;
    break;
  }
  step::NativeCompletion out;
  out.result = record::Result::Denied;
  out.reply = {{"ok", false},
               {"denied",
                {{"need", std::move(need)},
                 {"on", std::move(on)},
                 {"why", perm::reason_text(why)},
                 {"fix", std::move(fix)}}}};
  return out;
}
void record(step::NativeCompletion &out, const step::NativeCall &call, std::string name,
            std::vector<Handle> objects, Json facts) {
  record::Event event;
  event.step = call.step;
  event.kind = record::Kind::System;
  const auto holder = actor(call);
  event.actor = holder.kind == perm::HolderKind::Player ? record::Actor::player(holder.id)
                                                        : record::Actor::script(holder.id);
  event.name = std::move(name);
  event.changed = out.changed;
  event.result = out.result;
  event.things = std::move(objects);
  event.detail = std::move(facts);
  out.records.push_back(std::move(event));
}
Goods goods(IdentityRead identity, const lifecycle::Parts &parts, const Relation &declared_parts,
            AuthorityRead authority, const perm::OwnerLimits &limits, const perm::Plots &plots,
            Handle h) {
  if (!identity.valid(h))
    throw std::invalid_argument("stale goods handle");
  if (const auto *parents = declared_parts.in.get(h.slot()); parents && !parents->empty())
    throw std::invalid_argument("exchange the declared assembly root, not an internal part");
  Goods out{perm::target(identity, authority, limits, plots, h),
            lifecycle::collect(parts, identity, h),
            {}};
  for (auto part : std::span{out.closure}.subspan(1))
    out.parts.push_back(perm::target(identity, authority, limits, plots, part));
  return out;
}
Json handles(std::span<const Handle> objects) {
  auto rows = Json::array();
  for (auto h : objects)
    rows.push_back(to_string(h));
  return rows;
}
Json grants(std::span<const GrantId> ids) {
  auto rows = Json::array();
  for (auto id : ids)
    rows.push_back("g:" + std::to_string(id));
  return rows;
}
void cleared_notice(step::NativeCompletion &out, const perm::Grants &rows,
                    std::span<const GrantId> revoked, PlayerId owner, Handle root,
                    std::span<const Handle> objects) {
  const auto belongs = [&](const perm::Grant &grant) {
    const bool scope = grant.scope.kind == perm::ScopeKind::Object ||
                       grant.scope.kind == perm::ScopeKind::Avatar ||
                       (grant.scope.kind == perm::ScopeKind::Account &&
                        grant.scope.account_kind == perm::AccountKind::Object);
    return (scope && std::ranges::find(objects, Handle{grant.scope.subject}) != objects.end()) ||
           (grant.holder.kind == perm::HolderKind::Script &&
            std::ranges::find(objects, grant.holder.installed_on) != objects.end());
  };
  std::vector<GrantId> cleared;
  for (const auto id : revoked) {
    const auto &grant = rows.rows[id];
    if (belongs(grant) || (grant.parent && belongs(rows.rows[grant.parent])))
      cleared.push_back(id);
  }
  if (!cleared.empty())
    out.notices.emplace_back(
        owner, Json{{"kind", "changed"}, {"cleared", grants(cleared)}, {"on", to_string(root)}});
}
const perm::Offer &offer_row(const perm::Offers &offers, perm::OfferId id) {
  if (!id || id >= offers.rows.size() || !offers.rows[id].live || offers.rows[id].trade)
    throw std::invalid_argument("ordinary offer is gone");
  return offers.rows[id];
}
const perm::Trade &trade_row(const perm::Trades &trades, perm::TradeId id, PlayerId player) {
  if (!id || id >= trades.rows.size() || !trades.rows[id].live)
    throw std::invalid_argument("trade is gone");
  const auto &row = trades.rows[id];
  if (row.players[0] != player && row.players[1] != player)
    throw std::invalid_argument("caller is not a trade participant");
  return row;
}
} // namespace amber::step::commerce_detail
