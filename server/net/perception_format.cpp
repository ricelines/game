#include "net.hpp"

namespace amber::net {
namespace {
nlohmann::json vector(Vec3 p) {
  return {p.x, p.y, p.z};
}
nlohmann::json atom(const Value &v) {
  switch (v.kind) {
  case Value::Kind::None:
    return nullptr;
  case Value::Kind::Number:
    return v.number;
  case Value::Kind::Integer:
    return v.integer;
  case Value::Kind::Boolean:
    return v.boolean;
  case Value::Kind::Object:
    return to_string(v.object);
  }
  return nullptr;
}
template <class T> nlohmann::json event_atom(const T &v) {
  return std::visit(
      [](const auto &value) -> nlohmann::json {
        using V = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<V, Value>)
          return atom(value);
        else if constexpr (std::is_same_v<V, Vec3>)
          return vector(value);
        else if constexpr (std::is_same_v<V, TermRef>)
          return {{"term", value.id}};
        else if constexpr (std::is_same_v<V, std::string>)
          return value;
        else {
          nlohmann::json result = nlohmann::json::object();
          for (const auto &field : value)
            result[field.name] = event_atom(field.value);
          return result;
        }
      },
      v);
}
nlohmann::json full(const PreparedThing &p, bool far) {
  const auto &s = p.seen;
  const auto &d = p.description;
  nlohmann::json row{
      {"h", to_string(s.object)},
      {"model", d.model},
      {"measured", d.measured},
      {"says",
       {{"name", d.says.name}, {"description", d.says.description}, {"writing", d.says.writing}}},
      {"lit", s.lit},
      {"held", s.held}};
  if (!d.elaborated.empty())
    row["elaborated"] = d.elaborated;
  if (s.reachable)
    row["actions"] = d.actions;
  if (!s.through.empty()) {
    row["through"] = nlohmann::json::array();
    for (auto h : s.through)
      row["through"].push_back(to_string(h));
  }
  if (far) {
    const char *reason = s.salience == observe::Seen::Salience::Large    ? "large"
                         : s.salience == observe::Seen::Salience::Moving ? "moving"
                                                                         : "bright";
    row["why"] = reason;
  }
  return row;
}
} // namespace
nlohmann::json event_payload_json(const EventPayload &payload) {
  return event_atom(payload);
}
nlohmann::json observation(Step now, double hz, const SelfDescription &self,
                           std::span<const PreparedThing> things,
                           std::span<const PreparedThing> far,
                           std::span<const std::string> body_actions,
                           std::span<const std::string> player_actions) {
  nlohmann::json result{{"step", now},
                        {"time_s", now / hz},
                        {"self",
                         {{"h", to_string(self.body)},
                          {"at", vector(self.position)},
                          {"facing", self.facing_degrees},
                          {"holding", self.holding ? nlohmann::json(to_string(self.holding))
                                                   : nlohmann::json(nullptr)},
                          {"intent", self.intent},
                          {"zone", self.zones.empty() ? "" : self.zones.front().second}}},
                        {"things", nlohmann::json::array()},
                        {"far", nlohmann::json::array()},
                        {"heard", nlohmann::json::array()},
                        {"events", nlohmann::json::array()},
                        {"body_actions", body_actions},
                        {"player_actions", player_actions}};
  auto rows = [&](std::span<const PreparedThing> rows, const char *key, bool distant) {
    for (const auto &p : rows) {
      auto row = full(p, distant);
      row["dist_m"] = p.seen.distance_m;
      row["bearing"] = p.seen.bearing_degrees;
      result[key].push_back(std::move(row));
    }
  };
  rows(things, "things", false);
  rows(far, "far", true);
  return result;
}
} // namespace amber::net
