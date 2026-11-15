#pragma once

#include <filesystem>

namespace amber::recovery {
std::filesystem::path latest_snapshot(const std::filesystem::path &data);
// Preserve the previous run before the streaming writer opens its current file.
void rotate_record(const std::filesystem::path &record);
} // namespace amber::recovery
