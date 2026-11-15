#include "recovery.hpp"

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace amber::recovery {
std::filesystem::path latest_snapshot(const std::filesystem::path &data) {
  const auto directory = data / "snapshots";
  if (!std::filesystem::exists(directory))
    return {};
  std::filesystem::path result;
  std::uint64_t newest = 0;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    const auto name = entry.path().filename().string();
    if (!entry.is_regular_file() || !name.starts_with("snap-") || !name.ends_with(".amb.zst"))
      continue;
    const auto number = std::string_view(name).substr(5, name.size() - 13);
    std::uint64_t step = 0;
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), step);
    if (parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size() &&
        (result.empty() || step > newest)) {
      result = entry.path();
      newest = step;
    }
  }
  return result;
}

void rotate_record(const std::filesystem::path &record) {
  if (!std::filesystem::exists(record))
    return;
  for (std::uint64_t part = 1;; ++part) {
    const auto archived = record.parent_path() / (record.stem().stem().string() + ".previous-" +
                                                  std::to_string(part) + ".jsonl.zst");
    if (!std::filesystem::exists(archived)) {
      std::filesystem::rename(record, archived);
      return;
    }
  }
}
} // namespace amber::recovery
