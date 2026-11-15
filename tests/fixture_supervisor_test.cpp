#include "elaborate/elaborate.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace amber;
void require(bool value, const char *text) {
  if (!value)
    throw std::runtime_error(text);
}
elaborate::FixtureJob job(const std::filesystem::path &capture, int mode) {
  elaborate::FixtureJob job;
  job.id = 42;
  job.request = 7;
  job.actor = 3;
  job.capture = std::string(64, 'a');
  job.capture_path = capture;
  job.candidate.artifact = std::string(64, 'b');
  job.candidate.registry = std::string(64, 'c');
  job.candidate.script = 9;
  job.candidate.runtime = "fixture-protocol-probe";
  job.candidate.subjects = {{"target", Handle{(std::uint64_t{1} << 32) | 5}}};
  job.candidate.tests = nlohmann::json::array(
      {{{"given", {{"subject", "target"}, {"state", {{"probe.mode", mode}}}}},
        {"when", {{"steps", 0}}},
        {"expect", {{"subject", "target"}, {"term", "probe.counter"}, {"equals", 2}}}}});
  job.limits = {2000, 256, 65536};
  return job;
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const auto directory = std::filesystem::current_path() / "fixture-supervisor-test";
  try {
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory / "captures");
    const auto capture = directory / "captures" / "original";
    std::ofstream(capture) << 1;
    auto original = job(capture, 0);
    original.candidate.tests.push_back(original.candidate.tests.front());
    auto result = fixture::evaluate({argv[1], directory, directory / "runs", original});
    require(result.status == elaborate::FixtureStatus::Completed && result.tests.size() == 2 &&
                result.job == original.id && result.capture == original.capture &&
                result.candidate == original.candidate.artifact,
            "supervisor preserves identity and runs each declared test");
    for (const auto &test : result.tests)
      require(test.candidate.passed && !test.baseline.passed && test.candidate.observed == 2 &&
                  test.baseline.observed == 1 && test.candidate.error.empty() &&
                  test.baseline.error.empty(),
              "fresh candidate and baseline processes reload the same captured input");
    std::size_t outputs = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(directory / "runs"))
      if (entry.path().filename() == "used")
        ++outputs;
    require(outputs == 4, "all test/pass pairs have independent output directories");
    for (int mode : {1, 2, 3, 5}) {
      const auto failed =
          fixture::evaluate({argv[1], directory, directory / "runs", job(capture, mode)});
      require(failed.status == elaborate::FixtureStatus::Error && !failed.error.empty(),
              "child execution/malformed/missing report is never a baseline comparison failure");
    }
    auto slow = job(capture, 4);
    slow.limits.timeout_ms = 50;
    const auto timed = fixture::evaluate({argv[1], directory, directory / "runs", slow});
    require(timed.status == elaborate::FixtureStatus::Error && timed.error == "fixture timed out" &&
                timed.elapsed_ms < 400,
            "supervisor enforces total deadline and reaps child");
    const auto missing = fixture::evaluate(
        {directory / "missing-executable", directory, directory / "runs", job(capture, 0)});
    require(missing.status == elaborate::FixtureStatus::Unavailable,
            "unavailable child remains explicit");
    elaborate::FixtureWorker nested(argv[1], directory / "worker", 1);
    nested.submit(job(capture, 0));
    nested.finish();
    const auto completed = nested.take();
    require(completed.size() == 1 &&
                completed.front().status == elaborate::FixtureStatus::Completed &&
                completed.front().tests.front().candidate.observed == 2 &&
                completed.front().tests.front().baseline.observed == 1,
            "actual worker-to-supervisor-to-child CLI roundtrip");
    std::filesystem::remove_all(directory);
    std::cout << "Fixture supervisor/process protocol checks passed (not gameplay assertions).\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
