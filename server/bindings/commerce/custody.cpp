#include "bindings/scene.hpp"
#include "private.hpp"

#include <unordered_set>

namespace amber::bindings::commerce {
namespace detail {
namespace {
space::VisibleCustody visible(const Custody &binding, const perm::CustodyChange &change) {
  return {change.goods, change.parent,
          change.parent && binding.identity.valid(change.parent) &&
              bool(binding.identity.flags[change.parent.slot()] & Body)};
}
std::vector<Handle> descendants(const Custody &binding, Handle root) {
  auto objects = lifecycle::collect(binding.parts, binding.identity, root);
  objects.erase(objects.begin());
  return objects;
}
} // namespace
void preflight(const Custody &binding, const perm::CustodyChange &change) {
  const auto parts = descendants(binding, change.goods);
  const auto check =
      change.in_world ? space::can_visible_custody(binding.space, binding.identity, binding.parents,
                                                   visible(binding, change), parts)
                      : space::can_custody(binding.space, binding.identity, change.goods,
                                           change.parent, binding.parents, parts);
  if (check.status != space::Status::Ok)
    throw std::invalid_argument("custody: " + std::string(space::status_text(check.status)));
}
void apply(Custody &binding, std::span<const perm::CustodyChange> changes,
           react::DeferredEffects &effects) {
  for (const auto &change : changes) {
    const auto parts = descendants(binding, change.goods);
    std::vector<space::RelationChange> relations;
    const auto applied =
        change.in_world
            ? space::visible_custody(binding.space, binding.identity, binding.transforms,
                                     binding.parents, binding.flags, binding.physical,
                                     visible(binding, change), relations, parts)
            : space::custody(binding.space, binding.identity, change.goods, change.parent,
                             binding.parents, binding.flags, binding.physical, relations, parts);
    if (applied.status != space::Status::Ok)
      throw std::logic_error("preflighted custody failed: " +
                             std::string(space::status_text(applied.status)));
    for (const auto &row : relations) {
      const auto term = binding.relation_terms.at(static_cast<std::size_t>(row.kind));
      if (term) {
        effects.ordered.emplace_back(step::WatchWork{{row.from, term}, {}});
        effects.ordered.emplace_back(step::WatchWork{{row.to, term}, {}});
      }
    }
    std::vector<Handle> closure{change.goods};
    closure.insert(closure.end(), parts.begin(), parts.end());
    scene_visible(binding.scene, closure, change.in_world);
  }
}
} // namespace detail
namespace {
std::vector<perm::CustodyChange> released(const perm::Offers &offers,
                                          const perm::Trades &all_trades,
                                          std::span<const Handle> affected,
                                          std::span<const Handle> excluded = {}) {
  std::unordered_set<std::uint64_t> handles;
  for (auto h : affected)
    handles.insert(h.bits);
  const auto omitted = [&](Handle h) { return std::ranges::find(excluded, h) != excluded.end(); };
  std::unordered_set<perm::TradeId> trades;
  std::vector<perm::CustodyChange> out;
  for (const auto &offer : offers.rows) {
    if (!offer.live || (!handles.contains(offer.goods.bits) &&
                        std::none_of(offer.parts.begin(), offer.parts.end(),
                                     [&](Handle h) { return handles.contains(h.bits); })))
      continue;
    if (offer.trade)
      trades.insert(offer.trade);
    else if (!omitted(offer.goods))
      out.push_back({offer.goods, {}, true});
  }
  for (auto id : trades)
    for (const auto &side : all_trades.rows.at(id).sides)
      if (side.offer) {
        const auto &offer = offers.rows.at(side.offer);
        if (offer.live && offer.goods && !omitted(offer.goods))
          out.push_back(side.prior_custody);
      }
  return out;
}
} // namespace
bool can_retire_goods(const RetirementBinding &binding, std::span<const Handle> dying,
                      std::string &error) {
  try {
    for (const auto &change : released(binding.offers, binding.trades, dying, dying))
      detail::preflight(binding.custody, change);
    return true;
  } catch (const std::exception &problem) {
    error = problem.what();
    return false;
  }
}
void retire_goods(RetirementBinding &binding, std::span<const Handle> dying,
                  react::DeferredEffects &effects) {
  std::unordered_set<std::uint64_t> handles;
  for (auto h : dying)
    handles.insert(h.bits);
  std::vector<perm::CustodyChange> changes;
  for (auto h : dying) {
    auto released = perm::invalidate_goods_offers(binding.offers, binding.trades,
                                                  binding.custody.identity, binding.flags, h);
    for (auto change : released)
      if (!handles.contains(change.goods.bits))
        changes.push_back(change);
  }
  detail::apply(binding.custody, changes, effects);
  effects.changed |= !changes.empty();
}
bool preflight_rebound_offers(const void *local, std::span<const Handle> affected,
                              std::span<const Handle> removed,
                              std::vector<space::VisibleCustody> &forecast, std::string &error) {
  const auto &binding = *static_cast<const RebindOffersBinding *>(local);
  std::vector<space::VisibleCustody> pending;
  try {
    for (const auto &change : released(binding.offers, binding.trades, affected)) {
      detail::preflight(binding.custody, change);
      if (!change.in_world && change.parent &&
          std::ranges::find(removed, change.parent) != removed.end())
        throw std::invalid_argument("definition removes an inventory custodian");
      if (change.in_world)
        pending.push_back(detail::visible(binding.custody, change));
    }
    forecast = std::move(pending);
    return true;
  } catch (const std::exception &problem) {
    error = problem.what();
    return false;
  }
}
void rebind_offers(void *local, std::span<const Handle> affected, const InvocationInput &invocation,
                   react::DeferredEffects &effects) {
  auto &binding = *static_cast<RebindOffersBinding *>(local);
  const auto changes = released(binding.offers, binding.trades, affected);
  if (changes.empty())
    return;
  // Run before native extraction. The structural forecast already checked new
  // geometry and aggregate capacity for the restored physical grasps.
  for (const auto &change : changes)
    detail::preflight(binding.custody, change);
  auto offers = Json::array();
  auto trades = Json::array();
  for (const auto &row : binding.offers.rows)
    if (row.live &&
        std::ranges::any_of(changes, [&](const auto &change) { return change.goods == row.goods; }))
      offers.push_back("offer:" + std::to_string(row.id));
  for (const auto &row : binding.trades.rows)
    if (row.live && std::ranges::any_of(row.sides, [&](const auto &side) {
          return side.offer && std::ranges::any_of(changes, [&](const auto &change) {
                   return change.goods == binding.offers.rows[side.offer].goods;
                 });
        }))
      trades.push_back("trade:" + std::to_string(row.id));
  for (auto h : affected)
    perm::invalidate_goods_offers(binding.offers, binding.trades, binding.custody.identity,
                                  binding.flags, h);
  std::vector<Handle> things;
  for (const auto &change : changes) {
    const auto closure =
        lifecycle::collect(binding.custody.parts, binding.custody.identity, change.goods);
    for (auto h : closure) {
      binding.flags.set(h.slot(), binding.flags.get(h.slot()) & ~Escrow);
      things.push_back(h);
    }
  }
  detail::apply(binding.custody, changes, effects);
  record::Event event;
  event.step = invocation.step;
  event.kind = record::Kind::System;
  event.name = "goods_offers_invalidated";
  event.result = record::Result::Applied;
  event.changed = true;
  event.things = std::move(things);
  event.detail = {{"reason", "definition_changed"}, {"offers", offers}, {"trades", trades}};
  effects.ordered.emplace_back(std::move(event));
  effects.changed = true;
}
} // namespace amber::bindings::commerce
