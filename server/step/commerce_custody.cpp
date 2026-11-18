#include "commerce_private.hpp"

#include <set>
#include <unordered_set>

namespace amber::step {
namespace commerce_detail {
void check_custody(const space::Space &space, IdentityRead identity, ParentRead parents,
                   const lifecycle::Parts &parts, const perm::CustodyChange &change) {
  auto descendants = lifecycle::collect(parts, identity, change.goods);
  descendants.erase(descendants.begin());
  const space::VisibleCustody visible{change.goods, change.parent,
                                      change.parent && identity.valid(change.parent) &&
                                          bool(identity.flags[change.parent.slot()] & Body)};
  const auto result =
      change.in_world
          ? space::can_visible_custody(space, identity, parents, visible, descendants)
          : space::can_custody(space, identity, change.goods, change.parent, parents, descendants);
  if (result.status != space::Status::Ok)
    throw std::invalid_argument("custody: " + std::string(space::status_text(result.status)));
}
} // namespace commerce_detail
step::NativeCompletion retire_goods(perm::Offers &offers, perm::Trades &trades,
                                    IdentityRead identity, PermissionFlags flags,
                                    std::span<const Handle> dying) {
  std::unordered_set<std::uint64_t> handles;
  for (auto h : dying)
    handles.insert(h.bits);
  std::vector<Handle> changed;
  for (auto h : dying) {
    auto affected = perm::invalidate_goods_offers(offers, trades, identity, flags, h);
    for (auto object : affected)
      if (!handles.contains(object.bits))
        changed.push_back(object);
  }
  auto out = commerce_detail::result(!changed.empty());
  out.changed_objects = std::move(changed);
  return out;
}

step::NativeCompletion rebind_offers(perm::Offers &offers, perm::Trades &trades,
                                     PermissionFlags flags, IdentityRead identity,
                                     const lifecycle::Parts &parts,
                                     std::span<const Handle> affected, Step now) {
  std::vector<Handle> changed;
  std::set<perm::OfferId> offer_set;
  std::set<std::uint32_t> trade_set;
  for (auto h : affected)
    if (const auto found = offers.by_object.find(h.bits); found != offers.by_object.end())
      for (auto id : found->second) {
        offer_set.insert(id);
        if (const auto trade = offers.rows[id].trade; trade && trades.rows[trade].live)
          trade_set.insert(trade);
      }
  auto offer_ids = Json::array();
  for (auto id : offer_set)
    offer_ids.push_back("offer:" + std::to_string(id));
  auto trade_ids = Json::array();
  for (auto id : trade_set)
    trade_ids.push_back("trade:" + std::to_string(id));
  for (auto h : affected) {
    auto roots = perm::invalidate_goods_offers(offers, trades, identity, flags, h);
    changed.insert(changed.end(), roots.begin(), roots.end());
  }
  if (changed.empty())
    return commerce_detail::result(false);
  std::vector<Handle> things;
  for (auto object : changed) {
    const auto closure = lifecycle::collect(parts, identity, object);
    for (auto h : closure) {
      flags.set(h.slot(), flags.get(h.slot()) & ~Escrow);
      things.push_back(h);
    }
  }
  auto out = commerce_detail::result(true);
  out.changed_objects = things;
  record::Event event;
  event.step = now;
  event.kind = record::Kind::System;
  event.name = "goods_offers_invalidated";
  event.result = record::Result::Applied;
  event.changed = true;
  event.things = std::move(things);
  event.detail = {{"reason", "definition_changed"}, {"offers", offer_ids}, {"trades", trade_ids}};
  out.records.push_back(std::move(event));
  return out;
}
} // namespace amber::step
