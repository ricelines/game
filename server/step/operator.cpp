#include "run_private.hpp"

#include <algorithm>
#include <stdexcept>

namespace amber::step {
namespace {
bool architect_allowance(perm::State &permissions, PlayerId player, bool on,
                         const amber::Settings &settings, const Clock &clock) {
  const auto holder = perm::Holder::player(player);
  const auto scope = perm::Scope::account(perm::Account::player(0));
  const auto found = permissions.grants.by_key.find({holder, scope});
  if (found != permissions.grants.by_key.end())
    return !on && !perm::revoke(permissions.grants, permissions.players, found->second).empty();
  if (!on)
    return false;
  const perm::GrantRequest request{0,
                                   holder,
                                   scope,
                                   1,
                                   settings.architect_allowance,
                                   duration_steps(settings.architect_period_seconds, clock.hz),
                                   clock.step +
                                       duration_steps(settings.architect_expiry_seconds, clock.hz)};
  const auto granted = perm::create_root_grant(permissions.grants, permissions.players, request,
                                               {holder, player, true}, clock.step);
  if (!granted)
    throw std::invalid_argument("architect allowance: " +
                                std::string(perm::reason_text(granted.reason)));
  return true;
}
} // namespace
void admit_operator(const net::Admission &admission, net::State &net, perm::State &permissions,
                    const net::KeyHash &operator_key, const amber::Settings &settings,
                    record::Journal &journal, const Clock &clock) {
  const auto &request = std::get<net::OperatorRequest>(admission.request);
  record::Event event;
  event.step = clock.step;
  event.kind = record::Kind::System;
  event.name = "operator";
  event.detail = {{"op", request.op}, {"player", "p:" + std::to_string(request.player)}};
  if (request.op == "role")
    event.detail.update({{"role", request.role}, {"on", request.on}});
  std::string refusal;
  if (net::hash_key(request.key) != operator_key)
    refusal = "bad_key";
  else if (request.player >= net.profiles.rows.size() || !net.profiles.rows[request.player].id)
    refusal = "bad_input";
  if (!refusal.empty()) {
    event.result = record::Result::Denied;
    event.detail["reason"] = refusal;
    journal.append(std::move(event));
    net.sessions.reject(admission.session, std::move(refusal));
    return;
  }
  auto &profile = net.profiles.rows[request.player];
  nlohmann::json reply = {{"t", "operator"},
                          {"ok", true},
                          {"op", request.op},
                          {"player", "p:" + std::to_string(request.player)}};
  if (request.op == "role") {
    if (request.role == "architect")
      event.changed = architect_allowance(permissions, request.player, request.on, settings, clock);
    auto &roles = profile.roles;
    const auto found = std::ranges::find(roles, request.role);
    event.changed |= (found != roles.end()) != request.on;
    if (request.on && found == roles.end())
      roles.push_back(request.role);
    else if (!request.on && found != roles.end())
      roles.erase(found);
    if (request.role == "architect") {
      const auto architects = settings.land.way_group;
      auto &groups = permissions.players.rows[request.player].groups;
      const auto membership = std::lower_bound(groups.begin(), groups.end(), architects);
      const auto present = membership != groups.end() && *membership == architects;
      event.changed |= present != request.on;
      if (request.on && !present)
        groups.insert(membership, architects);
      else if (!request.on && present)
        groups.erase(membership);
    }
    reply.update({{"role", request.role}, {"on", request.on}});
  } else {
    auto key = net::generate_key();
    auto updated = profile;
    updated.key_hashes = {net::hash_key(key)};
    net::put_profile(net.profiles, std::move(updated));
    reply["key"] = std::move(key);
    event.changed = true;
  }
  event.result = event.changed ? record::Result::Applied : record::Result::NoEffect;
  journal.append(std::move(event));
  net.sessions.send(admission.session, std::move(reply));
}
} // namespace amber::step
