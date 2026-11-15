#pragma once
#include "catalog/catalog.hpp"
#include "net/net.hpp"
#include <unordered_set>

namespace amber::bindings {
struct ReplicationIndex {
  struct Asset {
    net::AssetHash hash;
    std::uint64_t parameters;
    float footprint_m;
    bool faces_motion;
    std::vector<TermId> replicated;
  };
  struct Row {
    std::string asset;
    net::Cell cell;
    bool process, published;
    std::optional<Step> sampled;
  };
  net::Settings settings;
  std::unordered_map<std::string, Asset> assets;
  std::unordered_map<std::uint64_t, Row> rows;
  std::map<net::Cell, std::unordered_set<std::uint64_t>> cells, process_cells;
  std::map<net::Cell, std::size_t> viewers;
  std::unordered_map<net::SessionId, std::set<net::Cell>> interests;
  std::unordered_set<std::uint64_t> dirty, active_processes, removed;
  explicit ReplicationIndex(net::Settings value) : settings(value) {}
};
// Register again after a model/parameter rebind; no authoritative instance fields are copied.
void register_replication(ReplicationIndex &, Handle, IdentityRead, TransformRead,
                          const catalog::Models &, const catalog::ParameterBlocks &,
                          const catalog::Assets &, const react::ValueRead &);
void moved(ReplicationIndex &, Handle, TransformRead);
void value_changed(ReplicationIndex &, Handle, TermId, const react::Processes &);
void changed(ReplicationIndex &, Handle);
void retire_replication(ReplicationIndex &, Handle);
void view_interest(ReplicationIndex &, net::SessionId, Vec3);
void remove_view_interest(ReplicationIndex &, net::SessionId);
net::Publication publication(ReplicationIndex &, IdentityRead, TransformRead,
                             const react::ValueRead &, Step, double hz);
} // namespace amber::bindings
