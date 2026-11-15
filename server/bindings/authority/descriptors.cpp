#include "descriptors.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
namespace amber::bindings::authority {
namespace {
bool role(const control::Profiles &profiles, PlayerId player, std::string_view wanted) {
  return player < profiles.rows.size() && profiles.rows[player].id == player &&
         std::find(profiles.rows[player].roles.begin(), profiles.rows[player].roles.end(),
                   wanted) != profiles.rows[player].roles.end();
}
} // namespace
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
perm::AccountTarget account(const Json &value, const AccountRead &binding) {
  const auto &name = value.get_ref<const std::string &>();
  if (name.starts_with("p:")) {
    const auto id = identifier(value, "p:");
    if (id >= binding.players.rows.size())
      throw std::invalid_argument("unknown player account");
    return perm::AccountTarget::player(id);
  }
  if (name.starts_with("group:")) {
    const auto id = identifier(value, "group:");
    if (id >= binding.groups.rows.size() || binding.groups.rows[id].id != id)
      throw std::invalid_argument("unknown group account");
    return {perm::Account::group(id), binding.groups.rows[id].controller};
  }
  const auto object = live_handle(value, binding.identity);
  return {perm::Account::object(object), binding.authority.owner[object.slot()]};
}
perm::ScopeControl scope(const Json &row, const ScopeRead &binding, PlayerId issuer) {
  if (row.size() != 1)
    throw std::invalid_argument("choose one scope");
  if (row.contains("object")) {
    const auto object = live_handle(row.at("object"), binding.identity);
    const auto target = perm::target(binding.identity, binding.authority, binding.limits, object);
    return {perm::Scope::object(object), target.owner,
            (perm::all_rights & ~perm::inherited_rights) |
                (target.owner_limit & perm::inherited_rights),
            target.plot, false};
  }
  if (row.contains("owned_on_plot")) {
    const auto &owned = row.at("owned_on_plot");
    const auto owner = identifier(owned.at("owner"), "p:");
    const auto plot = owned.at("plot").get<PlotId>();
    if (owner >= binding.players.rows.size() || !plot || plot >= binding.plots.rows.size() ||
        binding.plots.rows[plot].id != plot)
      throw std::invalid_argument("unknown owned-on-plot scope");
    return {perm::Scope::owned_on_plot(owner, plot), owner, perm::all_rights, plot, false};
  }
  if (row.contains("account")) {
    const auto target = account(
        row.at("account"), {binding.identity, binding.authority, binding.players, binding.groups});
    return {perm::Scope::account(target.account), target.controller, 1, 0, false};
  }
  if (row.contains("avatar")) {
    const auto body = live_handle(row.at("avatar"), binding.identity);
    if (!(binding.identity.flags[body.slot()] & Body))
      throw std::invalid_argument("avatar scope needs a Body");
    return {perm::Scope::avatar(body), binding.authority.owner[body.slot()], 3,
            binding.authority.plot[body.slot()], false};
  }
  if (row.contains("plot")) {
    const auto id = row.at("plot").get<PlotId>();
    if (!id || id >= binding.plots.rows.size() || binding.plots.rows[id].id != id)
      throw std::invalid_argument("unknown plot scope");
    return {perm::Scope::plot(id), binding.plots.rows[id].owner,
            perm::Rights{7} | perm::bit(perm::Verb::Transfer), id, false};
  }
  if (row.contains("issuance") && row.at("issuance") == true)
    return {perm::Scope::issuance(), 0, 1, 0, role(binding.profiles, issuer, "architect")};
  throw std::invalid_argument("unknown grant scope");
}
perm::Rights rights(const Json &row, perm::ScopeKind scope) {
  perm::Rights result = 0;
  for (const auto &value : row) {
    const auto name = value.get<std::string>();
    if (scope == perm::ScopeKind::Account || scope == perm::ScopeKind::Issuance) {
      if ((scope == perm::ScopeKind::Account && name != "spend") ||
          (scope == perm::ScopeKind::Issuance && name != "issue"))
        throw std::invalid_argument("wrong financial right");
      result |= 1;
    } else if (scope == perm::ScopeKind::Avatar) {
      if (name == "animate")
        result |= 1;
      else if (name == "camera")
        result |= 2;
      else
        throw std::invalid_argument("wrong avatar right");
    } else if (scope == perm::ScopeKind::Plot) {
      if (name == "build")
        result |= 1;
      else if (name == "enter")
        result |= 2;
      else if (name == "run_scripts")
        result |= 4;
      else if (name == "transfer")
        result |= perm::bit(perm::Verb::Transfer);
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

} // namespace amber::bindings::authority
