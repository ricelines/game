#pragma once

#include "perm/perm.hpp"
#include "space/space.hpp"

namespace amber::bindings {
struct TravelAccess {
  struct Row {
    PlayerId player;
    std::vector<space::TravelBoundary> forbidden;
  };
  std::unordered_map<std::uint64_t, Row> bodies;
  Step next_expiry = 0;
};
// Called after permission mutations and on restore. Cached boundaries contain
// no authority; ordinary live checks construct them again after any expiry.
void invalidate_travel(TravelAccess &, const perm::Grants &, Step);
std::span<const space::TravelBoundary> travel_boundaries(TravelAccess &, IdentityRead,
                                                         AuthorityRead, Handle body,
                                                         const perm::Plots &, const perm::Players &,
                                                         const perm::Grants &, Step);
} // namespace amber::bindings
