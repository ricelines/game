#include "settings.hpp"
#include "step/step.hpp"
#include "view_private.hpp"

#include <charconv>

namespace amber::net::view_detail {
namespace {
std::uint32_t numeric_id(std::string_view value) {
  std::uint32_t id;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), id);
  if (error != std::errc{} || end != value.data() + value.size())
    throw std::invalid_argument("Invalid identifier");
  return id;
}
Json land_rules(const react::State &state) {
  auto rules = Json::array();
  for (const auto &world : state.world_scripts)
    if (world.space == "land") {
      const auto script = "s:" + std::to_string(world.script);
      rules.push_back({{"script", script},
                       {"holder", {{"script", script}, {"on", "world"}}},
                       {"description", world.description},
                       {"writing", world.writing},
                       {"inspect", {{"target", script}}}});
    }
  return rules;
}
} // namespace
PlotId plot_id(std::string_view name, const perm::Plots &plots) {
  const auto id = numeric_id(name.substr(5));
  if (!id || id >= plots.rows.size() || plots.rows[id].id != id)
    throw std::invalid_argument("Unknown plot");
  return id;
}
Json inspect_land(const catalog::State &catalog, const react::State &react_state,
                  const ObjectState &objects, const perm::State &permissions,
                  const step::Clock &clock, const amber::Settings &settings,
                  const Invocation &call) {
  const auto &plots = permissions.plots;
  const auto &fields = call.request.fields;
  const auto name = fields.at("target").get<std::string>();
  const auto ids = identity(objects.objects);
  const bool embodied = ids.valid(call.principal.body);
  const auto poses = transforms(objects.objects);
  const auto position = embodied ? poses.position[call.principal.body.slot()] : Vec3{};
  const auto forward =
      embodied ? rotate(poses.rotation[call.principal.body.slot()], {0, 1, 0}) : Vec3{};
  const auto record = [&](const perm::Plot &plot, const perm::Offer *offer) {
    auto row =
        perm::plot_record(plots, permissions.grants, plot.id, settings.land, clock.step, offer);
    if (embodied) {
      const Vec3 delta{std::clamp(position.x, plot.bounds[0], plot.bounds[2]) - position.x,
                       std::clamp(position.y, plot.bounds[1], plot.bounds[3]) - position.y, 0};
      row["dist_m"] = delta.length();
      row["bearing"] = delta.length() == 0 ? 0
                                           : std::atan2(forward.y * delta.x - forward.x * delta.y,
                                                        forward.x * delta.x + forward.y * delta.y) *
                                                 180 / std::numbers::pi_v<float>;
    }
    const auto target = "plot:" + std::to_string(plot.id);
    row["inspect"] = {{"target", target}};
    row["actions"] = {{"target", target}};
    return row;
  };
  if (name != "land") {
    const auto id = name == "plot" ? embodied ? objects.objects.plot[call.principal.body.slot()] : 0
                                   : plot_id(name, plots);
    if (!id)
      return {{"plot", nullptr}};
    auto result = record(plots.rows.at(id), perm::plot_offer(permissions.offers, id, clock.step,
                                                             call.principal.player));
    result["action_signatures"] = Json::array();
    for (auto action : space_action_ids(catalog, react_state, "land"))
      result["action_signatures"].push_back(
          react::action_signature(react_state.terms, react_state.native_declarations, action));
    result["land"] = {{"target", "land"}};
    return result;
  }
  // Three questions, one call each: what is around me (the default, S5), what
  // could I buy (for_sale), and what does a player hold (owner). Filters apply
  // to all three; every row names the calls that act on it.
  std::optional<PlayerId> owner;
  if (fields.contains("owner")) {
    const auto requested = fields.at("owner").get<std::string>();
    if (requested == "self")
      owner = call.principal.player;
    else if (requested.starts_with("p:"))
      owner = numeric_id(std::string_view(requested).substr(2));
    else
      throw std::invalid_argument("owner must be self or p:N");
  }
  const std::optional<bool> for_sale =
      fields.contains("for_sale") ? std::optional(fields["for_sale"].get<bool>()) : std::nullopt;
  const auto fronts = fields.value("fronts", std::string{});
  const auto max_price = fields.contains("max_price")
                             ? std::optional(fields["max_price"].get<std::int64_t>())
                             : std::nullopt;
  const auto min_area = fields.value("min_area", 0.0);
  const auto cursor = fields.value("cursor", std::uint64_t{});
  const auto limit = fields.value("limit", settings.perception.things_max);
  const auto mode = owner ? "owner" : for_sale.value_or(false) ? "for_sale" : "in_sight";
  const auto sort =
      fields.value("sort", std::string(owner                             ? "id"
                                       : std::string(mode) == "for_sale" ? "price"
                                                                         : "distance"));
  if (owner && sort != "id")
    throw std::invalid_argument("an owner's plots are listed in plot order");
  if (sort != "id" && sort != "price" && sort != "distance" && sort != "area")
    throw std::invalid_argument("sort must be price, distance or area");
  if ((sort == "distance" || std::string(mode) == "in_sight") && !embodied)
    throw std::invalid_argument("listing by distance needs a live body; filter for_sale or owner");
  const auto distance = [&](const perm::Plot &plot) {
    return Vec3{std::clamp(position.x, plot.bounds[0], plot.bounds[2]) - position.x,
                std::clamp(position.y, plot.bounds[1], plot.bounds[3]) - position.y, 0}
        .length();
  };
  const auto area = [](const perm::Plot &plot) {
    return (plot.bounds[2] - plot.bounds[0]) * (plot.bounds[3] - plot.bounds[1]);
  };
  const auto offer_of = [&](PlotId id) {
    return perm::plot_offer(permissions.offers, id, clock.step, call.principal.player);
  };
  const auto keep = [&](const perm::Plot &plot) {
    const auto *offer = offer_of(plot.id);
    return plot.id && (!for_sale || bool(offer) == *for_sale) &&
           (fronts.empty() || perm::plot_facts(plots, plot.id, settings.land).fronts == fronts) &&
           (!max_price || (offer && offer->price <= *max_price)) && area(plot) >= min_area;
  };
  const auto row = [&](const perm::Plot &plot) {
    const auto *offer = offer_of(plot.id);
    auto result = record(plot, offer);
    const auto &b = plot.bounds;
    result["center"] = {(b[0] + b[2]) / 2, (b[1] + b[3]) / 2, 0};
    if (offer && plot.owner != call.principal.player)
      result["accept"] = {
          {"action", "engine.accept"},
          {"args", {{"offer", "offer:" + std::to_string(offer->id)}, {"max_price", offer->price}}}};
    return result;
  };
  Json land = Json::array();
  bool more = false;
  Json next_cursor = nullptr;
  if (owner) {
    // Plot order, continued after the last id returned.
    if (const auto found = plots.by_owner.find(*owner); found != plots.by_owner.end())
      for (auto it = std::ranges::upper_bound(found->second, cursor); it != found->second.end();
           ++it) {
        const auto &plot = plots.rows[*it];
        if (plot.kind != perm::PlotKind::Lot || !keep(plot))
          continue;
        if (land.size() == limit) {
          more = true;
          break;
        }
        land.push_back(row(plot));
        next_cursor = plot.id;
      }
    if (!more)
      next_cursor = nullptr;
  } else {
    // Plots in sight, or lots on offer; then sorted and paged by position.
    std::vector<PlotId> found;
    if (std::string(mode) == "in_sight") {
      const auto sight = settings.perception.sight_m;
      for (auto id : perm::plots_touching(plots, {position.x - sight, position.y - sight,
                                                  position.x + sight, position.y + sight}))
        if (distance(plots.rows[id]) <= sight)
          found.push_back(id);
    } else
      for (const auto &[id, offer] : permissions.offers.by_plot)
        found.push_back(id);
    std::erase_if(found, [&](PlotId id) { return !keep(plots.rows[id]); });
    const auto key = [&](PlotId id) -> double {
      const auto &plot = plots.rows[id];
      return sort == "distance" ? distance(plot)
             : sort == "area"   ? -area(plot)
             : sort == "price"  ? double(offer_of(id) ? offer_of(id)->price : 0)
                                : double(id);
    };
    std::ranges::sort(found, [&](PlotId a, PlotId b) {
      const auto ka = key(a), kb = key(b);
      return ka != kb ? ka < kb : a < b;
    });
    const auto begin = std::min<std::size_t>(cursor, found.size());
    const auto end = std::min<std::size_t>(begin + limit, found.size());
    for (auto index = begin; index < end; ++index)
      land.push_back(row(plots.rows[found[index]]));
    more = end < found.size();
    if (more)
      next_cursor = end;
  }
  return {{"land", std::move(land)},
          {"mode", mode},
          {"sort", sort},
          {"more", more},
          {"next_cursor", next_cursor},
          {"rules", land_rules(react_state)},
          {"actions", {{"target", "land"}}}};
}
} // namespace amber::net::view_detail
