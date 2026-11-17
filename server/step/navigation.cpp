#include "step/step.hpp"

namespace amber::step {
void queue_navigation(NavigationRequests &pending, const space::State &space_state,
                      IdentityRead identity, TransformRead transforms,
                      std::span<const Handle> objects, PlayerId author, Handle body) {
  if (!identity.valid(body))
    return;
  for (auto object : objects) {
    if (!identity.valid(object))
      continue;
    const auto region = space::bounds(space_state, identity, object);
    if (!region)
      continue;
    auto &request = pending.rows[author];
    request.body = body;
    request.reference = transforms.position[body.slot()];
    request.regions[object] = *region;
  }
}
std::vector<NavigationNotice> publish_navigation(NavigationRequests &pending,
                                                 const space::State &space_state,
                                                 IdentityRead identity, TransformRead transforms,
                                                 const space::NavigationInstall &installed) {
  std::vector<NavigationNotice> notices;
  for (auto request = pending.rows.begin(); request != pending.rows.end();) {
    auto &row = request->second;
    row.errors.insert(row.errors.end(), installed.errors.begin(), installed.errors.end());
    for (auto region = row.regions.begin(); region != row.regions.end();) {
      const auto current = identity.valid(region->first)
                               ? space::bounds(space_state, identity, region->first)
                               : std::nullopt;
      if (!current)
        region = row.regions.erase(region);
      else {
        region->second = *current;
        ++region;
      }
    }
    if (row.regions.empty()) {
      request = pending.rows.erase(request);
      continue;
    }
    if (!installed.pending.empty()) {
      ++request;
      continue;
    }
    if (identity.valid(row.body))
      row.reference = transforms.position[row.body.slot()];
    std::vector<space::Bounds> regions;
    nlohmann::json objects = nlohmann::json::array();
    for (const auto &[object, region] : row.regions) {
      regions.push_back(region);
      objects.push_back(to_string(object));
    }
    const auto report = space::diagnose_navigation(space_state.space, regions, row.reference);
    if (!report.complete) {
      ++request;
      continue;
    }
    nlohmann::json areas = nlohmann::json::array();
    bool unreachable = false;
    for (const auto &area : report.areas) {
      unreachable |= report.reference_navigable && !area.reachable;
      areas.push_back({{"point", {area.point.x, area.point.y, area.point.z}},
                       {"component", area.connected_component},
                       {"reachable", area.reachable}});
    }
    const auto status = !row.errors.empty()           ? "rebuild_failed"
                        : !report.reference_navigable ? "no_reference_surface"
                        : unreachable                 ? "unreachable"
                        : report.areas.empty()        ? "no_walkable_area"
                                                      : "connected";
    const auto text = !row.errors.empty()           ? "Navigation tile rebuilding failed."
                      : !report.reference_navigable ? "Your body is outside the navigation surface."
                      : unreachable          ? "Built geometry has disconnected walkable areas."
                      : report.areas.empty() ? "Built geometry adds no walkable area."
                                             : "Built walkable areas are connected to your body.";
    notices.push_back({request->first,
                       {{"kind", "navigation"},
                        {"status", status},
                        {"text", text},
                        {"objects", std::move(objects)},
                        {"revision", report.revision},
                        {"complete", true},
                        {"reference_navigable", report.reference_navigable},
                        {"areas", std::move(areas)},
                        {"errors", row.errors}}});
    request = pending.rows.erase(request);
  }
  return notices;
}
} // namespace amber::step
