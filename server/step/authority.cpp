#include "step.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
namespace amber::step {
std::uint32_t identifier(const Json &value, std::string_view prefix) {
  const auto &text = value.get_ref<const std::string &>();
  if (!text.starts_with(prefix))
    throw std::invalid_argument("expected " + std::string(prefix) + " identifier");
  std::uint32_t id = 0;
  const auto parsed = std::from_chars(text.data() + prefix.size(), text.data() + text.size(), id);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !id)
    throw std::invalid_argument("invalid complete identifier");
  return id;
}
Handle live_handle(const Json &value, IdentityRead identity) {
  const auto parsed = parse_handle(value.get<std::string>());
  if (!parsed || !identity.valid(*parsed))
    throw std::invalid_argument("stale or invalid full object handle");
  return *parsed;
}
Step duration_steps(double seconds, double hz) {
  const auto value = std::ceil(seconds * hz);
  if (!std::isfinite(value) || value < 0 || value >= double(std::numeric_limits<Step>::max()))
    throw std::invalid_argument("invalid grant duration");
  return static_cast<Step>(value);
}
perm::AccountTarget account(const Json &value, IdentityRead identity, AuthorityRead authority,
                            const perm::Players &players, const perm::Groups &groups) {
  const auto &name = value.get_ref<const std::string &>();
  if (name.starts_with("p:")) {
    const auto id = name == "p:0" ? PlayerId{} : identifier(value, "p:");
    if (id >= players.rows.size())
      throw std::invalid_argument("unknown player account");
    return perm::AccountTarget::player(id);
  }
  if (name.starts_with("group:")) {
    const auto id = identifier(value, "group:");
    if (id >= groups.rows.size() || groups.rows[id].id != id)
      throw std::invalid_argument("unknown group account");
    return {perm::Account::group(id), groups.rows[id].controller};
  }
  const auto object = live_handle(value, identity);
  return {perm::Account::object(object), authority.owner[object.slot()]};
}
perm::ScopeControl scope(const Json &row, IdentityRead identity, AuthorityRead authority,
                         const perm::OwnerLimits &limits, const perm::Players &players,
                         const perm::Plots &plots, const perm::Groups &groups) {
  if (row.size() != 1)
    throw std::invalid_argument("choose one scope");
  if (row.contains("object")) {
    const auto object = live_handle(row.at("object"), identity);
    const auto target = perm::target(identity, authority, limits, plots, object);
    return {perm::Scope::object(object), target.owner,
            (perm::all_rights & ~perm::inherited_rights) |
                (target.owner_limit & perm::inherited_rights),
            target.plot, target.on_lot};
  }
  if (row.contains("owned")) {
    const auto owner = identifier(row.at("owned"), "p:");
    if (owner >= players.rows.size())
      throw std::invalid_argument("unknown owned scope");
    return {perm::Scope::owned(owner), owner, perm::all_rights};
  }
  if (row.contains("owned_on_plot")) {
    const auto &owned = row.at("owned_on_plot");
    const auto owner = identifier(owned.at("owner"), "p:");
    const auto plot = owned.at("plot").get<PlotId>();
    if (owner >= players.rows.size() || !plot || plot >= plots.rows.size() ||
        plots.rows[plot].id != plot)
      throw std::invalid_argument("unknown owned-on-plot scope");
    return {perm::Scope::owned_on_plot(owner, plot), owner, perm::all_rights, plot};
  }
  if (row.contains("account")) {
    const auto target = account(row.at("account"), identity, authority, players, groups);
    return {perm::Scope::account(target.account), target.controller, 1, 0};
  }
  if (row.contains("avatar")) {
    const auto body = live_handle(row.at("avatar"), identity);
    if (!(identity.flags[body.slot()] & Body))
      throw std::invalid_argument("avatar scope needs a Body");
    return {perm::Scope::avatar(body), authority.owner[body.slot()], 3,
            authority.plot[body.slot()]};
  }
  if (row.contains("plot")) {
    const auto id = row.at("plot").get<PlotId>();
    if (!id || id >= plots.rows.size() || plots.rows[id].id != id)
      throw std::invalid_argument("unknown plot scope");
    return {perm::Scope::plot(id), plots.rows[id].owner, perm::Rights{31}, id,
            plots.rows[id].kind == perm::PlotKind::Lot};
  }
  throw std::invalid_argument("unknown grant scope");
}
perm::Rights rights(const Json &row, perm::ScopeKind scope) {
  perm::Rights result = 0;
  for (const auto &value : row) {
    const auto name = value.get<std::string>();
    if (scope == perm::ScopeKind::Account) {
      if (name != "spend")
        throw std::invalid_argument("wrong financial right");
      result |= 1;
    } else if (scope == perm::ScopeKind::Avatar) {
      if (name == "animate")
        result |= 1;
      else if (name == "camera")
        result |= 2;
      else
        throw std::invalid_argument("wrong avatar right");
    } else if (scope == perm::ScopeKind::AllLand && name.starts_with("land_")) {
      result |= rights(Json::array({name.substr(5)}), perm::ScopeKind::Plot) << 8;
    } else if (scope == perm::ScopeKind::Plot) {
      if (name == "build")
        result |= 1;
      else if (name == "enter")
        result |= 2;
      else if (name == "run_scripts")
        result |= 4;
      else if (name == "transfer")
        result |= 8;
      else if (name == "return")
        result |= 16;
      else
        throw std::invalid_argument("wrong plot right");
    } else {
      constexpr std::array names{"use", "take", "move", "edit", "copy", "transfer", "destroy"};
      const auto found = std::find(names.begin(), names.end(), name);
      if (found == names.end())
        throw std::invalid_argument("unknown permission right");
      result |= perm::Rights{1} << std::distance(names.begin(), found);
    }
  }
  return result;
}

} // namespace amber::step
