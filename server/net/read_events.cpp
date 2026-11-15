#include "view_private.hpp"

namespace amber::net {
namespace {
Json summary(const Json &record) {
  Json result = Json::object();
  for (auto field = record.begin(); field != record.end(); ++field) {
    const auto &name = field.key();
    if (name != "observation" && name != "measured" && name != "source" && name != "bytecode" &&
        name != "manifest")
      result[name] = field.value();
  }
  return result;
}
} // namespace
void append_records(RecordFeed &feed, std::span<const Json> records, IdentityRead identity,
                    TransformRead transforms) {
  for (const auto &record : records) {
    RecordFeed::Entry entry{record, {}};
    if (record.contains("things"))
      for (const auto &name : record.at("things")) {
        const auto handle = parse_handle(name.get<std::string>());
        if (handle && identity.valid(*handle))
          entry.positions.push_back(transforms.position[handle->slot()]);
      }
    feed.rows.push_back(std::move(entry));
    while (feed.rows.size() > feed.capacity)
      feed.rows.pop_front();
  }
}
net::Reply read_events(const RecordFeed &feed, const SessionViews &sessions, float radius_m,
                       std::size_t default_limit, const net::Invocation &call) {
  const auto since = call.request.fields.value("since", std::uint64_t{});
  const auto root = call.request.fields.value("root", std::uint64_t{});
  const auto thing = call.request.fields.value("thing", std::string{});
  const auto limit = call.request.fields.value("limit", default_limit);
  const bool detailed = call.request.fields.value("detail", true);
  const bool privileged = call.principal.mode == net::Mode::Debug ||
                          std::find(call.principal.roles.begin(), call.principal.roles.end(),
                                    "architect") != call.principal.roles.end();
  const auto player = "p:" + std::to_string(call.principal.player);
  Vec3 eye;
  if (const auto session = sessions.rows.find(call.session); session != sessions.rows.end())
    eye = session->second.eye;
  std::vector<const Json *> selected;
  selected.reserve(std::min(limit, feed.rows.size()));
  std::size_t matched = 0;
  for (const auto &entry : feed.rows) {
    const auto &record = entry.record;
    if (record.value("id", std::uint64_t{}) <= since)
      continue;
    if (root && record.value("root", std::uint64_t{}) != root)
      continue;
    if (!thing.empty()) {
      const auto &things = record.at("things");
      if (std::find(things.begin(), things.end(), Json(thing)) == things.end())
        continue;
    }
    bool visible = privileged || record.value("actor", std::string{}) == player;
    if (!visible)
      for (const auto at : entry.positions) {
        const auto delta = at - eye;
        if (delta.x * delta.x + delta.y * delta.y + delta.z * delta.z <= radius_m * radius_m) {
          visible = true;
          break;
        }
      }
    if (!visible)
      continue;
    ++matched;
    if (selected.size() < limit)
      selected.push_back(&record);
    else
      selected[(matched - 1) % limit] = &record;
  }
  Json events = Json::array();
  const auto first = matched > limit ? matched % limit : 0;
  for (std::size_t i = 0; i < selected.size(); ++i) {
    const auto &record = *selected[(first + i) % selected.size()];
    events.push_back(detailed ? record : summary(record));
  }
  const auto oldest =
      feed.rows.empty() ? std::uint64_t{} : feed.rows.front().record.value("id", std::uint64_t{});
  const auto newest =
      feed.rows.empty() ? std::uint64_t{} : feed.rows.back().record.value("id", std::uint64_t{});
  return net::Reply::result({{"events", events},
                             {"events_omitted", matched - events.size()},
                             {"oldest_available", oldest},
                             {"newest", newest},
                             {"truncated", since && oldest > since + 1}});
}
net::Reply read_debug(const space::Space &space, IdentityRead identity, Step now,
                      float default_radius_m, Json stats, const net::Invocation &call) {
  const auto kind = call.request.fields.at("kind").get<std::string>();
  if (kind == "stats") {
    Json result = std::move(stats);
    result["step"] = now;
    result["navigation_revision"] = space::navigation_revision(space);
    std::size_t live = 0;
    for (std::size_t slot = 1; slot < identity.model.size(); ++slot)
      if (identity.model[slot])
        ++live;
    result["objects"] = live;
    return net::Reply::result(std::move(result));
  }
  if (kind != "nav" && kind != "collision" && kind != "paths" && kind != "volumes" && kind != "all")
    return view_detail::error("Unknown debug kind");
  Vec3 center;
  if (call.request.fields.contains("around")) {
    const auto &at = call.request.fields["around"];
    center = {at[0].get<float>(), at[1].get<float>(), at[2].get<float>()};
  }
  const auto radius = call.request.fields.value("radius", default_radius_m);
  const Vec3 extent{radius, radius, radius};
  const space::DebugSelection selection{{center - extent, center + extent},
                                        kind == "collision" || kind == "all",
                                        kind == "nav" || kind == "all",
                                        kind == "paths" || kind == "all",
                                        kind == "volumes" || kind == "all"};
  std::vector<space::DebugLine> lines;
  space::debug_lines(space, selection, lines);
  Json segments = Json::array();
  for (const auto &line : lines)
    segments.push_back({view_detail::vector(line.from), view_detail::vector(line.to)});
  return net::Reply::result({{"segments", segments}, {"kind", kind}});
}
} // namespace amber::net
