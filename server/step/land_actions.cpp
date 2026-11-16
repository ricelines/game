#include "commerce_private.hpp"

#include <cmath>

namespace amber::step {
namespace {
perm::Plot &lot(perm::State &state, PlotId id, const NativeCall &call) {
  if (!id || id >= state.plots.rows.size() || state.plots.rows[id].id != id ||
      state.plots.rows[id].kind != perm::PlotKind::Lot)
    throw std::invalid_argument("unknown lot");
  auto &plot = state.plots.rows[id];
  const auto allowed = perm::check_land(commerce_detail::actor(call), perm::LandVerb::Transfer,
                                        plot, state.players, state.grants, call.step);
  if (!allowed)
    throw std::invalid_argument(std::string(perm::reason_text(allowed.reason)));
  return plot;
}
void close_offers(perm::Offers &offers, PlotId id) {
  for (auto &offer : offers.rows)
    if (offer.live && offer.plot == id) {
      offers.expiring.erase({offer.expires_step, offer.id});
      offer.live = false;
    }
}
bool on_plot(perm::Scope scope, PlotId id) {
  return (scope.kind == perm::ScopeKind::Plot || scope.kind == perm::ScopeKind::OwnedOnPlot) &&
         scope.subject == id;
}
void refresh(ObjectState &objects, const perm::Plots &plots, PlotId first, PlotId second,
             NativeCompletion &out) {
  auto ids = identity(std::as_const(objects.objects));
  for (std::uint32_t slot = 1; slot < ids.model.size(); ++slot)
    if (ids.model[slot] &&
        (objects.objects.plot[slot] == first || objects.objects.plot[slot] == second))
      out.changed_objects.push_back(ids.at(slot));
  perm::update_plots(ids, transforms(objects.objects), plot_membership(objects.objects), plots,
                     out.changed_objects, true);
}
} // namespace
NativeCompletion abandon_plot(perm::State &state, ObjectState &objects,
                              const DeliveryTargets &delivery, const space::Space &space,
                              const NativeCall &call) {
  const auto id = call.arguments.at("plot").get<PlotId>();
  const auto owner = lot(state, id, call).owner;
  if (!owner)
    throw std::invalid_argument("lot is already in the pool");
  auto out = commerce_detail::result(true, {{"plot", id}, {"owner", "p:0"}});
  const auto ids = identity(std::as_const(objects.objects));
  for (std::uint32_t slot = 1; slot < ids.model.size(); ++slot) {
    if (!ids.model[slot] || objects.objects.plot[slot] != id || (ids.flags[slot] & (Body | Held)))
      continue;
    const auto *part_of = objects.declared_parts.in.get(slot);
    if (part_of && !part_of->empty())
      continue;
    auto returning = call;
    returning.request.target = ids.at(slot);
    const auto returned = return_goods(state, objects, delivery, space, returning, true);
    out.custody.insert(out.custody.end(), returned.custody.begin(), returned.custody.end());
    out.changed_objects.insert(out.changed_objects.end(), returned.changed_objects.begin(),
                               returned.changed_objects.end());
  }
  const auto revoked = perm::change_plot_owner(state, id, 0);
  if (!revoked.empty())
    out.notices.emplace_back(owner, Json{{"kind", "changed"},
                                         {"on", "plot:" + std::to_string(id)},
                                         {"cleared", commerce_detail::grants(revoked)}});
  commerce_detail::record(out, call, "plot_owner", {},
                          {{"plot", id}, {"old_owner", owner}, {"owner", 0}});
  out.records.back().kind = record::Kind::System;
  commerce_detail::record(out, call, "abandon_plot", {}, out.reply);
  out.permissions_changed = true;
  return out;
}
NativeCompletion split_plot(perm::State &state, ObjectState &objects, const NativeCall &call) {
  const auto id = call.arguments.at("plot").get<PlotId>();
  auto &plot = lot(state, id, call);
  const bool x = call.arguments.contains("x");
  const auto axis = x ? 0 : 1;
  const auto cut = call.arguments.at(x ? "x" : "y").get<float>();
  if (!std::isfinite(cut) || std::floor(cut) != cut || cut <= plot.bounds[axis] ||
      cut >= plot.bounds[axis + 2])
    throw std::invalid_argument("cut must be a whole metre strictly inside the lot");
  auto upper = plot;
  upper.id = PlotId(state.plots.rows.size());
  const auto next = upper.id;
  plot.bounds[axis + 2] = cut;
  upper.bounds[axis] = cut;
  state.plots.rows.push_back(std::move(upper));
  const auto count = state.grants.rows.size();
  std::map<GrantId, GrantId> copies;
  for (std::size_t i = 1; i < count; ++i) {
    auto grant = state.grants.rows[i];
    if (!grant.live || (grant.expires_step && grant.expires_step <= call.step) ||
        !on_plot(grant.scope, id))
      continue;
    grant.id = GrantId(state.grants.rows.size());
    copies.emplace(GrantId(i), grant.id);
    grant.scope.subject = next;
    if (copies.contains(grant.parent))
      grant.parent = copies.at(grant.parent);
    state.players.rows[grant.issuer].grants_given.push_back(grant.id);
    if (grant.holder.kind == perm::HolderKind::Player)
      state.players.rows[grant.holder.id].grants_held.push_back(grant.id);
    state.grants.rows.push_back(grant);
  }
  perm::rebuild_grant_lookup(state.grants);
  close_offers(state.offers, id);
  auto out = commerce_detail::result(true, {{"plots", {id, next}}});
  refresh(objects, state.plots, id, next, out);
  commerce_detail::record(out, call, "split_plot", out.changed_objects, out.reply);
  out.permissions_changed = true;
  return out;
}
NativeCompletion join_plots(perm::State &state, ObjectState &objects, const NativeCall &call) {
  auto a = call.arguments.at("plots").at(0).get<PlotId>();
  auto b = call.arguments.at("plots").at(1).get<PlotId>();
  if (a > b)
    std::swap(a, b);
  auto &left = lot(state, a, call);
  auto &right = lot(state, b, call);
  const auto &l = left.bounds;
  const auto &r = right.bounds;
  if (a == b || left.owner != right.owner ||
      !((l[0] == r[0] && l[2] == r[2] && (l[1] == r[3] || l[3] == r[1])) ||
        (l[1] == r[1] && l[3] == r[3] && (l[0] == r[2] || l[2] == r[0]))))
    throw std::invalid_argument(
        "lots must have the same owner and form one rectangle along a whole edge");
  left.bounds = {std::min(l[0], r[0]), std::min(l[1], r[1]), std::max(l[2], r[2]),
                 std::max(l[3], r[3])};
  right.id = 0;
  std::vector<GrantId> ids;
  for (const auto &grant : state.grants.rows)
    if (grant.live && on_plot(grant.scope, b))
      ids.push_back(grant.id);
  const auto removed = perm::revoke_many(state.grants, state.players, ids);
  close_offers(state.offers, a);
  close_offers(state.offers, b);
  auto out = commerce_detail::result(true, {{"plot", a}});
  std::map<PlayerId, std::vector<GrantId>> notices;
  for (auto id : removed)
    notices[state.grants.rows[id].issuer].push_back(id);
  for (const auto &[issuer, grants] : notices)
    out.notices.emplace_back(issuer, Json{{"kind", "changed"},
                                          {"on", "plot:" + std::to_string(b)},
                                          {"cleared", commerce_detail::grants(grants)}});
  refresh(objects, state.plots, a, b, out);
  commerce_detail::record(out, call, "join_plots", out.changed_objects,
                          {{"plots", {a, b}}, {"plot", a}});
  out.permissions_changed = true;
  return out;
}
} // namespace amber::step
