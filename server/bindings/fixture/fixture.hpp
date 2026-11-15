#pragma once

#include "bindings/controls/controls.hpp"
#include "elaborate/elaborate.hpp"

namespace amber::bindings::fixture {
struct Case {
  std::map<std::string, Handle> subjects;
  nlohmann::json test;
};
Case read_case(const std::filesystem::path &);
struct Setup {
  react::Properties &properties;
  react::Processes &processes;
  react::SharedColumns &shared;
  react::GivesTo &gives;
  const react::ValueRead &values;
};
void apply_given(Setup &, const Case &, Step, double hz, react::DeferredEffects &);
// Actions use the captured body and its current player's ordinary authority.
ActionOutcome perform_when(ActionTurns &, const Case &);
std::uint64_t steps(const Case &);
elaborate::TestRun observe(const react::ValueRead &, const Case &, Step, double hz);
} // namespace amber::bindings::fixture
