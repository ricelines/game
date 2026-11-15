#pragma once
#include "control.hpp"
#include "snap/codec.hpp"

namespace amber::control {
void write_profiles(snap::Output &, const Profiles &,
                    std::optional<std::span<const PlayerId>> selected = std::nullopt);
void read_profiles(snap::Input &, Profiles &);
} // namespace amber::control
