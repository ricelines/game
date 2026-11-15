#pragma once

#include <chrono>
#include <filesystem>
#include <span>
#include <string>

namespace amber::elaborate::process {
struct Outcome {
  int exit_code = -1;
  bool timed_out = false, unavailable = false;
  std::string error, diagnostics;
};
// Arguments exclude argv[0]. Nested fixture children inherit the supervisor's
// process group, so the outer worker can cancel the complete job tree.
Outcome run(const std::filesystem::path &executable, std::span<const std::string> arguments,
            std::chrono::steady_clock::time_point deadline, std::size_t diagnostic_bytes,
            bool own_process_group);
} // namespace amber::elaborate::process
