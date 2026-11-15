#pragma once

#include "perm.hpp"
#include "snap/snap.hpp"

namespace amber::perm {
struct SnapshotSelection {
  snap::ObjectSelection objects{std::span<const Handle>{}};
  std::unordered_set<PlayerId> players;
  std::unordered_set<GroupId> groups;
  std::unordered_set<PlotId> plots;
  std::unordered_set<GrantId> grants;
  std::unordered_set<OfferId> offers;
  std::unordered_set<TradeId> trades;
};
SnapshotSelection capture_authority(AuthorityRead, std::span<const Handle>,
                                    std::span<const PlayerId> actors, const Players &,
                                    const Plots &, const Groups &, const Grants &, const Offers &,
                                    const Trades &);
void write_players(snap::Output &, const Players &, const Ledger &, const Plots &, const Groups &,
                   const SnapshotSelection * = nullptr);
void read_players(snap::Input &, Players &, Ledger &, Plots &, Groups &);
void write_grants(snap::Output &, const Grants &, const Offers &, const Trades &,
                  const OwnerLimits &, const SnapshotSelection * = nullptr);
void read_grants(snap::Input &, Grants &, Offers &, Trades &, OwnerLimits &,
                 std::size_t object_slots);
} // namespace amber::perm
