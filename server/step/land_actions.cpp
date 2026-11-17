#include "commerce_private.hpp"

#include <cmath>

namespace amber::step {
namespace {
std::optional<NativeCompletion> lot(perm::State &state, PlotId id, const NativeCall &call) {
  const auto on = "plot:" + std::to_string(id);
  if (!id || id >= state.plots.rows.size() || state.plots.rows[id].id != id ||
      state.plots.rows[id].kind != perm::PlotKind::Lot)
    return commerce_detail::denied(perm::Reason::InvalidScope, "transfer", on, 0);
  const auto &plot = state.plots.rows[id];
  const auto allowed = perm::check_land(commerce_detail::actor(call), perm::LandVerb::Transfer,
                                        plot, state.players, state.grants, call.step);
  if (!allowed)
    return commerce_detail::denied(allowed.reason, "transfer", on, plot.owner);
  return std::nullopt;
}
void refresh(ObjectState &objects, const perm::Plots &plots, PlotId first, PlotId second,
             NativeCompletion &out) {
  auto ids = identity(std::as_const(objects.objects));
  for (auto plot : {first, second})
    if (const auto found = objects.objects.on_plot.find(plot);
        found != objects.objects.on_plot.end())
      for (auto slot : found->second)
        out.changed_objects.push_back(ids.at(slot));
  perm::update_plots(objects.objects, plots, out.changed_objects, true);
}
} // namespace
NativeCompletion abandon_plot(perm::State &state, ObjectState &objects,
                              const DeliveryTargets &delivery, const space::Space &space,
                              const LandSettings &settings, const NativeCall &call,
                              TermId plot_changed) {
  const auto id = call.arguments.at("plot").get<PlotId>();
  if (auto refusal = lot(state, id, call))
    return *refusal;
  const auto owner = state.plots.rows[id].owner;
  if (!owner)
    return commerce_detail::denied("transfer", "plot:" + std::to_string(id),
                                   "lot is already in the pool",
                                   "this lot already belongs to the pool");
  const auto holders = perm::plot_holders(state.plots.rows[id], state.grants, call.step);
  const auto bounds = state.plots.rows[id].bounds;
  const auto area = (bounds[2] - bounds[0]) * (bounds[3] - bounds[1]);
  auto out = commerce_detail::result(true, {{"plot", id}, {"owner", "p:0"}});
  for (auto object : plot_things(objects, id)) {
    auto returning = call;
    returning.request.target = object;
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
  commerce_detail::plot_changed(
      out, holders, plot_changed, call,
      {"owner", perm::plot_facts(state.plots, id, settings.port_spacing_m), owner, area});
  out.permissions_changed = true;
  return out;
}
NativeCompletion split_plot(perm::State &state, ObjectState &objects, const LandSettings &settings,
                            const NativeCall &call, TermId plot_changed) {
  const auto id = call.arguments.at("plot").get<PlotId>();
  if (auto refusal = lot(state, id, call))
    return *refusal;
  auto &plot = state.plots.rows[id];
  const bool x = call.arguments.contains("x");
  const auto axis = x ? 0 : 1;
  const auto cut = call.arguments.at(x ? "x" : "y").get<float>();
  if (!std::isfinite(cut) || std::floor(cut) != cut || cut <= plot.bounds[axis] ||
      cut >= plot.bounds[axis + 2])
    return commerce_detail::denied("transfer", "plot:" + std::to_string(id), "invalid cut",
                                   "cut at a whole meter strictly inside the lot");
  if (cut - plot.bounds[axis] < settings.lot_min_m ||
      plot.bounds[axis + 2] - cut < settings.lot_min_m)
    return commerce_detail::denied("transfer", "plot:" + std::to_string(id), "piece is too narrow",
                                   "cut at least LOT_MIN_M m from each edge");
  const auto holders = perm::plot_holders(plot, state.grants, call.step);
  const auto owner = plot.owner;
  const auto next = perm::split_plot(state, id, axis, cut, call.step);
  auto out = commerce_detail::result(true, {{"plots", {id, next}}});
  refresh(objects, state.plots, id, next, out);
  commerce_detail::record(out, call, "split_plot", out.changed_objects, out.reply);
  const auto &lower = state.plots.rows[id].bounds;
  const auto &upper = state.plots.rows[next].bounds;
  commerce_detail::plot_changed(
      out, holders, plot_changed, call,
      {"split", perm::plot_facts(state.plots, id, settings.port_spacing_m), owner,
       (lower[2] - lower[0]) * (lower[3] - lower[1]), (upper[2] - upper[0]) * (upper[3] - upper[1]),
       perm::plot_facts(state.plots, next, settings.port_spacing_m)});
  out.permissions_changed = true;
  return out;
}
NativeCompletion join_plots(perm::State &state, ObjectState &objects, const LandSettings &settings,
                            const NativeCall &call, TermId plot_changed) {
  auto a = call.arguments.at("plots").at(0).get<PlotId>();
  auto b = call.arguments.at("plots").at(1).get<PlotId>();
  if (a > b)
    std::swap(a, b);
  if (auto refusal = lot(state, a, call))
    return *refusal;
  if (auto refusal = lot(state, b, call))
    return *refusal;
  auto &left = state.plots.rows[a];
  auto &right = state.plots.rows[b];
  const auto &l = left.bounds;
  const auto &r = right.bounds;
  if (a == b || left.owner != right.owner ||
      !((l[0] == r[0] && l[2] == r[2] && (l[1] == r[3] || l[3] == r[1])) ||
        (l[1] == r[1] && l[3] == r[3] && (l[0] == r[2] || l[2] == r[0]))))
    return commerce_detail::denied(
        "transfer", "plot:" + std::to_string(a), "invalid join",
        "choose two lots of one owner that share a whole edge and form a rectangle");
  auto holders = perm::plot_holders(left, state.grants, call.step);
  const auto other_holders = perm::plot_holders(right, state.grants, call.step);
  holders.insert(holders.end(), other_holders.begin(), other_holders.end());
  const auto owner = left.owner;
  const auto area = (l[2] - l[0]) * (l[3] - l[1]);
  const auto other_area = (r[2] - r[0]) * (r[3] - r[1]);
  const auto old = perm::plot_facts(state.plots, a, settings.port_spacing_m);
  const auto other = perm::plot_facts(state.plots, b, settings.port_spacing_m);
  const auto removed = perm::join_plots(state, a, b);
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
  commerce_detail::plot_changed(out, holders, plot_changed, call,
                                {"join", perm::plot_facts(state.plots, a, settings.port_spacing_m),
                                 owner, area, other_area, other, old});
  out.permissions_changed = true;
  return out;
}
} // namespace amber::step
