#pragma once

#include "control/control.hpp"
#include "perm/perm.hpp"

namespace amber::bindings::authority {
using Json = nlohmann::json;
struct ScopeRead {
  IdentityRead identity;
  AuthorityRead authority;
  const perm::OwnerLimits &limits;
  const perm::Players &players;
  const perm::Plots &plots;
  const control::Profiles &profiles;
};
std::uint32_t identifier(const Json &, std::string_view prefix);
Handle live_handle(const Json &, IdentityRead);
Step duration_steps(double seconds, double hz);
PlayerId account(const Json &, const ScopeRead &);
perm::ScopeControl scope(const Json &, const ScopeRead &, PlayerId issuer);
perm::Rights rights(const Json &, perm::ScopeKind);
} // namespace amber::bindings::authority
