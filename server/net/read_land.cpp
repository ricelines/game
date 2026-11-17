#include "settings.hpp"
#include "step/step.hpp"
#include "view_private.hpp"

#include <charconv>
#include <unordered_map>

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
    auto row = perm::plot_record(plots, permissions.grants, plot.id, settings.land.port_spacing_m,
                                 clock.step, offer);
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
  std::unordered_map<PlotId, const perm::Offer *> offered;
  for (const auto &offer : permissions.offers.rows)
    if (offer.live && offer.plot && (!offer.expires_step || offer.expires_step > clock.step) &&
        (!offer.buyer || offer.buyer == call.principal.player))
      offered.try_emplace(offer.plot, &offer);
  const auto cursor = fields.value("cursor", std::uint64_t{});
  const auto limit = fields.value("limit", settings.perception.things_max);
  Json land = Json::array();
  bool more = false;
  PlotId last = 0;
  const auto append = [&](const perm::Plot &plot) {
    if (!plot.id || plot.kind != perm::PlotKind::Lot)
      return true;
    const auto found = offered.find(plot.id);
    const auto *offer = found == offered.end() ? nullptr : found->second;
    if (for_sale && bool(offer) != *for_sale)
      return true;
    if (land.size() == limit) {
      more = true;
      return false;
    }
    land.push_back(record(plot, offer));
    last = plot.id;
    return true;
  };
  if (owner) {
    if (const auto found = plots.by_owner.find(*owner); found != plots.by_owner.end())
      for (auto it = std::ranges::upper_bound(found->second, cursor); it != found->second.end();
           ++it)
        if (!append(plots.rows[*it]))
          break;
  } else
    for (auto index = cursor < plots.rows.size() ? std::size_t(cursor) + 1 : plots.rows.size();
         index < plots.rows.size(); ++index)
      if (!append(plots.rows[index]))
        break;
  return {{"land", std::move(land)},
          {"more", more},
          {"next_cursor", more ? Json(last) : Json(nullptr)},
          {"rules", land_rules(react_state)},
          {"actions", {{"target", "land"}}}};
}
} // namespace amber::net::view_detail
