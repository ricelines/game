#include "elaborate/elaborate.hpp"
#include "snap/snap.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace amber;
using namespace amber::elaborate;
namespace {
void require(bool value, const char *text) {
  if (!value)
    throw std::runtime_error(text);
}
FixtureJob job(JobId id, std::string mode) {
  FixtureJob result;
  result.id = id;
  result.request = static_cast<RequestId>(id);
  result.capture = std::string(64, 'a');
  result.capture_path = std::move(mode);
  result.candidate.artifact = std::string(64, 'b');
  result.candidate.registry = std::string(64, 'c');
  result.candidate.runtime = "transport-probe";
  result.candidate.script = 1;
  result.limits = {1000, 32, 65536};
  return result;
}
void worker(const std::filesystem::path &executable, const std::filesystem::path &directory) {
  FixtureWorker pool(executable, directory, 2);
  auto slow = job(1, "slow");
  slow.limits.timeout_ms = 100;
  pool.submit(slow);
  pool.submit(job(2, "normal"));
  pool.submit(job(3, "wrong"));
  pool.submit(job(4, "malformed"));
  std::vector<FixtureResult> results;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (results.size() < 4 && std::chrono::steady_clock::now() < until) {
    auto batch = pool.take();
    for (auto &result : batch)
      results.push_back(std::move(result));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  pool.finish();
  require(results.size() == 4, "all subprocess outcomes returned");
  require(results.front().job != 1, "slow fixture does not block other sessions");
  for (const auto &result : results) {
    require(result.diagnostics.size() <= 32, "diagnostics bounded");
    if (result.job == 1)
      require(result.status == FixtureStatus::Error && result.error == "fixture timed out",
              "timeout terminates process group");
    if (result.job == 2)
      require(result.status == FixtureStatus::Completed && result.tests[0].candidate.passed &&
                  !result.tests[0].baseline.passed,
              "actual subprocess report parsed");
    if (result.job == 3)
      require(result.status == FixtureStatus::Error &&
                  result.error == "fixture result identity mismatch",
              "identity mismatch retains original pending job id");
    if (result.job == 4)
      require(result.status == FixtureStatus::Error && !result.error.empty(),
              "malformed result is not acceptance");
  }
  FixtureWorker missing(directory / "no-such-executable", directory / "missing", 1);
  missing.submit(job(5, "normal"));
  missing.finish();
  const auto unavailable = missing.take();
  require(unavailable.size() == 1 && unavailable[0].status == FixtureStatus::Unavailable,
          "missing real runner explicitly unavailable");
}
void capture(const std::filesystem::path &directory) {
  snap::Buffer bytes;
  snap::Output out{bytes};
  snap::begin(out, {"capture-test", 50, {}});
  for (const auto &tag : snap::tags) {
    const auto marker = snap::begin_section(out, tag);
    snap::end_section(out, marker);
  }
  snap::finish(out, bytes);
  CaptureWriter writer(directory, 3);
  auto hash = writer.submit(bytes);
  writer.finish();
  const auto completed = writer.take();
  require(completed.size() == 1 && completed[0].hash == hash && completed[0].error.empty(),
          "completed capture bytes written asynchronously");
  auto loaded = snap::read_file(completed[0].path);
  require(loaded == bytes, "same actual S10 bytes after zstd roundtrip");
  const std::array<std::string_view, 1> versions{"capture-test"};
  require(snap::parse(loaded, versions).header.step == 50, "capture uses real S10 format");
  CaptureRefs refs;
  complete_capture(refs, completed[0]);
  require(refs.rows.at(hash).status == CaptureStatus::Ready,
          "capture completion publishes reference");
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const auto directory = std::filesystem::current_path() / "elaborate-worker-test";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  worker(argv[1], directory / "jobs");
  capture(directory / "captures");
  std::filesystem::remove_all(directory);
  std::cout << "fixture subprocess transport and S10 capture I/O checks passed (not a gameplay "
               "fixture)\n";
}
