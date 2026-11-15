#pragma once

#include "elaborate/elaborate.hpp"

namespace amber::fixture {
struct Run {
  std::filesystem::path executable, catalog, work_directory;
  elaborate::FixtureJob job;
};
// Composition only: every TestRun is reported by a fresh real simulation child.
elaborate::FixtureResult evaluate(const Run &);
int command(int argc, char **argv, const std::filesystem::path &executable);
} // namespace amber::fixture
