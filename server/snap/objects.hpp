#pragma once

#include "snap.hpp"

namespace amber::snap {
// nullopt is the entire world; an empty selection is an empty capture.
void write_objects(Output &, const Objects &,
                   std::optional<std::span<const Handle>> selection = std::nullopt);
void read_objects(Input &, Objects &);
} // namespace amber::snap
