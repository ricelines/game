#pragma once

#include "lifecycle.hpp"
#include "snap/snap.hpp"

namespace amber::lifecycle {
void write_parts(snap::Output &, const Parts &, IdentityRead,
                 std::optional<std::span<const Handle>> selected = {});
void read_parts(snap::Input &, Parts &, IdentityRead, Relation &declared_parts);
} // namespace amber::lifecycle
