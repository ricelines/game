#pragma once

#include "bindings/authority/descriptors.hpp"
#include "commerce.hpp"

#include <algorithm>
#include <cmath>

namespace amber::bindings::commerce::detail {
inline perm::Holder actor(const drive::NativeCall &call) {
  if (const auto *player = std::get_if<PlayerId>(&call.request.caller))
    return perm::Holder::player(*player);
  const auto script = std::get<ScriptHolder>(call.request.caller);
  return perm::Holder::script(script.script, script.object);
}
inline PlayerId player(const drive::NativeCall &call) {
  if (const auto *player = std::get_if<PlayerId>(&call.request.caller); player && *player)
    return *player;
  throw std::invalid_argument("this action requires a player caller");
}
inline PlayerId player_id(const Json &input, const perm::Players &players) {
  const auto id = authority::identifier(input, "p:");
  if (id >= players.rows.size())
    throw std::invalid_argument("unknown player wallet");
  return id;
}
inline PlayerId default_wallet(const drive::NativeCall &call, IdentityRead identity,
                               AuthorityRead authority, const perm::Players &players) {
  const auto holder = actor(call);
  if (holder.kind == perm::HolderKind::Player)
    return player_id("p:" + std::to_string(holder.id), players);
  if (!identity.valid(holder.installed_on))
    throw std::invalid_argument("script installation is no longer alive");
  const auto owner = authority.owner[holder.installed_on.slot()];
  return player_id("p:" + std::to_string(owner), players);
}
inline Handle present(const DeliveryTargets &delivery, IdentityRead identity, PlayerId player) {
  const auto found = delivery.present_bodies.find(player);
  return found != delivery.present_bodies.end() && identity.valid(found->second) ? found->second
                                                                                 : Handle{};
}
inline drive::NativeCompletion result(bool changed, Json extra = Json::object()) {
  drive::NativeCompletion out;
  out.changed = changed;
  out.result = changed ? record::Result::Applied : record::Result::NoEffect;
  out.reply = {{"ok", true}, {"effect", changed ? "applied" : "none"}};
  out.reply.update(extra);
  return out;
}
inline drive::NativeCompletion denied(perm::Reason why) {
  drive::NativeCompletion out;
  out.result = record::Result::Denied;
  out.reply = {{"ok", false}, {"denied", {{"why", perm::reason_text(why)}}}};
  return out;
}
inline void record(drive::NativeCompletion &out, const drive::NativeCall &call, std::string name,
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
struct Goods {
  perm::Target root;
  std::vector<Handle> closure;
  std::vector<perm::Target> parts;
};
inline Goods goods(const Custody &custody, AuthorityRead authority, const perm::OwnerLimits &limits,
                   Handle h) {
  if (!custody.identity.valid(h))
    throw std::invalid_argument("stale goods handle");
  if (const auto *parents = custody.declared_parts.in.get(h.slot()); parents && !parents->empty())
    throw std::invalid_argument("exchange the declared assembly root, not an internal part");
  Goods out{perm::target(custody.identity, authority, limits, h),
            lifecycle::collect(custody.parts, custody.identity, h),
            {}};
  for (auto part : std::span{out.closure}.subspan(1))
    out.parts.push_back(perm::target(custody.identity, authority, limits, part));
  return out;
}
void preflight(const Custody &, const perm::CustodyChange &);
void apply(Custody &, std::span<const perm::CustodyChange>, react::DeferredEffects &);
inline Json handles(std::span<const Handle> objects) {
  auto rows = Json::array();
  for (auto h : objects)
    rows.push_back(to_string(h));
  return rows;
}
inline Json grants(std::span<const GrantId> ids) {
  auto rows = Json::array();
  for (auto id : ids)
    rows.push_back("grant:" + std::to_string(id));
  return rows;
}
inline const perm::Offer &offer_row(const perm::Offers &offers, perm::OfferId id) {
  if (!id || id >= offers.rows.size() || !offers.rows[id].live || offers.rows[id].trade)
    throw std::invalid_argument("ordinary offer is gone");
  return offers.rows[id];
}
inline const perm::Trade &trade_row(const perm::Trades &trades, perm::TradeId id, PlayerId player) {
  if (!id || id >= trades.rows.size() || !trades.rows[id].live)
    throw std::invalid_argument("trade is gone");
  const auto &row = trades.rows[id];
  if (row.players[0] != player && row.players[1] != player)
    throw std::invalid_argument("caller is not a trade participant");
  return row;
}
} // namespace amber::bindings::commerce::detail
