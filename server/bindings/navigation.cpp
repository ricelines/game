#include "navigation.hpp"

namespace amber::bindings {
namespace {
void queue(NavigationDiagnostics &b, std::span<const Handle> objects,
           const InvocationInput &input) {
  if (!input.action_caller || !std::holds_alternative<PlayerId>(*input.action_caller) ||
      !b.identity.valid(input.action_body))
    return;
  const auto author = std::get<PlayerId>(*input.action_caller);
  for (auto object : objects) {
    if (!b.identity.valid(object))
      continue;
    const auto region = space::bounds(b.space, b.identity, object);
    if (!region)
      continue;
    auto &request = b.pending[author];
    request.body = input.action_body;
    request.reference = b.transforms.position[input.action_body.slot()];
    request.regions[object] = *region;
  }
}
} // namespace
void navigation_birth(NavigationDiagnostics &b, std::span<const Handle> objects,
                      const InvocationInput &input, react::DeferredEffects &) {
  queue(b, objects, input);
}
void navigation_rebind(NavigationDiagnostics &b, std::span<const lifecycle::Rebinding> rebindings,
                       const InvocationInput &input, react::DeferredEffects &) {
  std::vector<Handle> objects;
  objects.reserve(rebindings.size());
  for (const auto &row : rebindings)
    objects.push_back(row.object);
  queue(b, objects, input);
}
std::vector<NavigationNotice> publish_navigation(NavigationDiagnostics &b,
                                                 const space::NavigationInstall &installed,
                                                 space::Deadline deadline) {
  std::vector<NavigationNotice> notices;
  for (auto request = b.pending.begin(); request != b.pending.end();) {
    auto &row = request->second;
    row.errors.insert(row.errors.end(), installed.errors.begin(), installed.errors.end());
    for (auto region = row.regions.begin(); region != row.regions.end();) {
      const auto current = b.identity.valid(region->first)
                               ? space::bounds(b.space, b.identity, region->first)
                               : std::nullopt;
      if (!current)
        region = row.regions.erase(region);
      else {
        region->second = *current;
        ++region;
      }
    }
    if (row.regions.empty()) {
      request = b.pending.erase(request);
      continue;
    }
    if (!installed.pending.empty()) {
      ++request;
      continue;
    }
    if (b.identity.valid(row.body))
      row.reference = b.transforms.position[row.body.slot()];
    std::vector<space::Bounds> regions;
    nlohmann::json objects = nlohmann::json::array();
    for (const auto &[object, region] : row.regions) {
      regions.push_back(region);
      objects.push_back(to_string(object));
    }
    const auto report = space::diagnose_navigation(b.space, regions, row.reference, deadline);
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
    request = b.pending.erase(request);
  }
  return notices;
}
} // namespace amber::bindings
