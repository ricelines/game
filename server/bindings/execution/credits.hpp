#pragma once

#include "catalog/catalog.hpp"
#include "execution.hpp"

namespace amber::bindings {
struct MetadataCredits {
  IdentityRead identity;
  AttributionRead attribution;
  const catalog::Models &models;
  const catalog::Scripts &scripts;
  std::vector<PlayerId> seed_authors;
};
std::vector<record::Credit> resolve_credits(const void *, std::span<const Handle>, ScriptId,
                                            double hz);
} // namespace amber::bindings
