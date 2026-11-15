#pragma once

#include "react.hpp"

namespace amber::react {
// Empty error means every prior answer and desired value is valid. No writes.
std::string check_exchange(const Properties &, const Processes &, const ScriptState &,
                           const Terms &, const ResourceExchange &, Step, double hz,
                           std::size_t state_capacity_bytes);
// Caller checks the holder's full identity before selecting its scalar-state row.
// A failed preflight leaves all terms, processes and flat state unchanged.
bool apply_exchange(Properties &, Processes &, ScriptState &, const Terms &,
                    const ResourceExchange &, Step, double hz, std::size_t state_capacity_bytes,
                    DeferredEffects &, std::string &error);
} // namespace amber::react
