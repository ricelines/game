#pragma once

#include "bindings/checkpoint/checkpoint.hpp"

namespace amber {
// A row selection shared by the thirteen owner encoders. It owns identifiers
// only; each encoder retains its own exact table references.
struct CheckpointFilter {
  bool local = false;
  std::vector<Handle> selected;
  snap::ObjectSelection objects;
  std::optional<bindings::checkpoint::CatalogSelection> catalog;
  std::optional<perm::SnapshotSelection> authority;
  std::vector<PlotId> plots;
  std::optional<std::span<const Handle>> handles() const {
    return local ? std::optional<std::span<const Handle>>(selected) : std::nullopt;
  }
  std::optional<std::span<const PlotId>> land() const {
    return local ? std::optional<std::span<const PlotId>>(plots) : std::nullopt;
  }
};
} // namespace amber
