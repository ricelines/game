#include "commerce_private.hpp"

namespace amber::step {
NativeCompletion return_goods(perm::State &permissions, ObjectState &objects,
                              const DeliveryTargets &delivery, const space::Space &space,
                              const NativeCall &call, bool forced) {
  const auto ids = identity(std::as_const(objects.objects));
  const auto auth = authority(std::as_const(objects.objects));
  const auto parents = amber::parents(std::as_const(objects.objects));
  const auto item =
      commerce_detail::goods(ids, objects.parts, objects.declared_parts, auth,
                             permissions.owner_limits, permissions.plots, call.request.target);
  auto body = commerce_detail::present(delivery, ids, item.root.owner);
  if (space::can_custody(space, ids, item.root.object, body, parents,
                         std::span(item.closure).subspan(1))
          .status != space::Status::Ok)
    body = {};
  std::vector<perm::OfferId> live_offers;
  for (const auto &offer : permissions.offers.rows)
    if (offer.live)
      live_offers.push_back(offer.id);
  const auto returned = perm::return_goods(permissions,
                                           {commerce_detail::actor(call), item.root, item.parts,
                                            item.root.plot, body, bool(body), forced},
                                           call.step);
  if (!returned) {
    const auto on = "plot:" + std::to_string(item.root.plot);
    if (returned.reason == perm::Reason::NotController)
      return commerce_detail::denied("return", on, "you own this", "pick it up instead");
    if (returned.reason == perm::Reason::MissingRight &&
        perm::check_scope(perm::Holder::player(item.root.owner), perm::Scope::plot(item.root.plot),
                          1, permissions.grants, call.step))
      return commerce_detail::denied("return", on, "its owner holds build on this plot",
                                     "ask p:" + std::to_string(item.root.owner) +
                                         " to remove it, or revoke their build grant");
    return commerce_detail::denied(returned.reason, "return", on,
                                   item.root.plot < permissions.plots.rows.size()
                                       ? permissions.plots.rows[item.root.plot].owner
                                       : 0);
  }
  auto flags = permission_flags(objects.objects);
  std::vector<Handle> cleared;
  // Returning one side cancels a trade as a whole, releasing the other side's escrow.
  for (auto id : live_offers)
    if (const auto &offer = permissions.offers.rows[id]; !offer.live) {
      if (ids.valid(offer.goods)) {
        flags.set(offer.goods.slot(), flags.get(offer.goods.slot()) & ~Escrow);
        cleared.push_back(offer.goods);
      }
      for (auto part : offer.parts)
        if (ids.valid(part)) {
          flags.set(part.slot(), flags.get(part.slot()) & ~Escrow);
          cleared.push_back(part);
        }
    }
  auto out = commerce_detail::result(true, {{"object", to_string(item.root.object)},
                                            {"recipient", "p:" + std::to_string(item.root.owner)},
                                            {"location", body ? "inventory" : "mailbox"}});
  out.custody = returned.custody;
  out.changed_objects = item.closure;
  out.changed_objects.insert(out.changed_objects.end(), cleared.begin(), cleared.end());
  commerce_detail::record(out, call, "return", item.closure, out.reply);
  return out;
}
} // namespace amber::step
