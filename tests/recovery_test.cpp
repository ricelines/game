#include "recovery.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
} // namespace
int main() {
  const auto data =
      std::filesystem::current_path() / (".amber-recovery-test-" + std::to_string(getpid()));
  try {
    require(amber::recovery::latest_snapshot(data).empty(), "fresh run has no checkpoint");
    std::filesystem::create_directories(data / "snapshots");
    for (const auto name : {"snap-0.amb.zst", "snap-900.amb.zst", "snap-1200.amb.zst",
                            "snap-1300.amb.zst.tmp", "snap-invalid.amb.zst"})
      std::ofstream(data / "snapshots" / name) << name;
    require(amber::recovery::latest_snapshot(data).filename() == "snap-1200.amb.zst",
            "recovery selects greatest completed step, ignoring temporary files");
    const auto record = data / "events.jsonl.zst";
    std::ofstream(record) << "first";
    amber::recovery::rotate_record(record);
    std::ofstream(record) << "second";
    amber::recovery::rotate_record(record);
    std::string first, second;
    std::ifstream(data / "events.previous-1.jsonl.zst") >> first;
    std::ifstream(data / "events.previous-2.jsonl.zst") >> second;
    require(first == "first" && second == "second" && !std::filesystem::exists(record),
            "restart preserves every earlier record instead of truncating it");
    std::filesystem::remove_all(data);
  } catch (const std::exception &error) {
    std::filesystem::remove_all(data);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
