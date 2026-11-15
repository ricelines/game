#include "fixture.hpp"
#include "elaborate/process.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace amber::fixture {
namespace {
using Json = nlohmann::json;
Json read_json(const std::filesystem::path &path) {
  std::ifstream file(path);
  file.exceptions(std::ios::badbit);
  if (!file)
    throw std::runtime_error("cannot read fixture file: " + path.string());
  return Json::parse(file);
}
void write_json(const std::filesystem::path &path, const Json &value) {
  auto temporary = path;
  temporary += ".tmp";
  std::ofstream file(temporary, std::ios::binary);
  file.exceptions(std::ios::failbit | std::ios::badbit);
  file << value.dump();
  file.close();
  std::filesystem::rename(temporary, path);
}
elaborate::FixtureJob read_job(const std::filesystem::path &path,
                               const std::filesystem::path &capture) {
  const auto value = read_json(path);
  if (value.at("version") != 1)
    throw std::invalid_argument("unsupported fixture job version");
  elaborate::FixtureJob job;
  job.id = value.at("job").get<elaborate::JobId>();
  job.request = value.at("request").get<elaborate::RequestId>();
  job.actor = value.at("actor").get<PlayerId>();
  job.capture = value.at("capture").get<std::string>();
  job.capture_path = capture;
  job.candidate.artifact = value.at("candidate").get<std::string>();
  job.candidate.registry = value.at("registry").get<std::string>();
  job.candidate.script = value.at("script").get<ScriptId>();
  job.candidate.runtime = value.at("runtime").get<std::string>();
  job.candidate.tests = value.at("tests");
  for (const auto &[name, bits] : value.at("subjects").items())
    job.candidate.subjects.emplace(name, Handle{bits.get<std::uint64_t>()});
  const auto &limits = value.at("limits");
  job.limits = {limits.at("timeout_ms").get<std::uint64_t>(),
                limits.at("diagnostic_bytes").get<std::size_t>(),
                limits.at("result_bytes").get<std::size_t>()};
  return job;
}
elaborate::TestRun report(const std::filesystem::path &path, std::size_t max_bytes) {
  if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > max_bytes)
    throw std::runtime_error("fixture child report missing or too large");
  const auto value = read_json(path);
  if (value.at("version") != 1)
    throw std::runtime_error("unsupported fixture child report version");
  return {value.at("passed").get<bool>(), value.at("observed"),
          value.at("error").get<std::string>()};
}
} // namespace

elaborate::FixtureResult evaluate(const Run &run) {
  using Clock = std::chrono::steady_clock;
  const auto started = Clock::now();
  const auto &job = run.job;
  elaborate::FixtureResult result;
  result.job = job.id;
  result.request = job.request;
  result.capture = job.capture;
  result.candidate = job.candidate.artifact;
  result.runtime = job.candidate.runtime;
  try {
    elaborate::validate_tests(job.candidate.tests);
    if (!job.limits.timeout_ms || !job.limits.result_bytes)
      throw std::invalid_argument("fixture requires positive time and report limits");
    const auto deadline = started + std::chrono::milliseconds(job.limits.timeout_ms);
    std::filesystem::create_directories(run.work_directory);
    const auto directory =
        run.work_directory / (std::to_string(job.id) + "." + std::to_string(getpid()) + "." +
                              std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 started.time_since_epoch())
                                                 .count()));
    std::filesystem::create_directory(directory);
    Json subjects = Json::object();
    for (const auto &[name, handle] : job.candidate.subjects)
      subjects[name] = handle.bits;
    const auto catalog =
        run.catalog.empty() ? job.capture_path.parent_path().parent_path() : run.catalog;
    for (std::size_t i = 0; i < job.candidate.tests.size(); ++i) {
      const auto case_path = directory / (std::to_string(i) + ".case.json");
      write_json(case_path,
                 {{"version", 1}, {"subjects", subjects}, {"test", job.candidate.tests[i]}});
      elaborate::TestResult test;
      auto execute = [&](bool active) {
        const auto output = directory / (std::to_string(i) + (active ? ".candidate" : ".baseline"));
        std::filesystem::create_directory(output);
        const auto output_report = output / "report.json";
        std::vector<std::string> arguments{"run",
                                           "--restore",
                                           job.capture_path.string(),
                                           "--catalog",
                                           catalog.string(),
                                           "--data",
                                           output.string(),
                                           "--no-network",
                                           "--unpaced",
                                           "--fixture-case",
                                           case_path.string(),
                                           "--candidate",
                                           job.candidate.artifact,
                                           "--candidate-registry",
                                           job.candidate.registry,
                                           "--candidate-author",
                                           std::to_string(job.actor),
                                           "--report",
                                           output_report.string()};
        if (active)
          arguments.push_back("--candidate-active");
        const auto child =
            elaborate::process::run(run.executable, arguments, deadline,
                                    job.limits.diagnostic_bytes - result.diagnostics.size(), false);
        result.diagnostics += child.diagnostics;
        if (child.unavailable)
          result.status = elaborate::FixtureStatus::Unavailable;
        if (!child.error.empty())
          return elaborate::TestRun{false, {}, child.error};
        if (child.exit_code)
          return elaborate::TestRun{false, {}, "fixture simulation process failed"};
        return report(output_report, job.limits.result_bytes);
      };
      test.candidate = execute(true);
      if (!test.candidate.error.empty()) {
        test.baseline.error = "not executed after candidate execution error";
        result.tests.push_back(std::move(test));
        throw std::runtime_error(result.tests.back().candidate.error);
      }
      test.baseline = execute(false);
      result.tests.push_back(std::move(test));
      if (!result.tests.back().baseline.error.empty())
        throw std::runtime_error(result.tests.back().baseline.error);
    }
    result.status = elaborate::FixtureStatus::Completed;
  } catch (const std::exception &error) {
    result.error = error.what();
  }
  result.elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
  return result;
}

int command(int argc, char **argv, const std::filesystem::path &executable) {
  try {
    if (argc < 3 || std::string_view(argv[1]) != "fixture")
      throw std::invalid_argument("fixture requires a capture path");
    const std::filesystem::path capture = argv[2];
    std::filesystem::path job_path, result_path, catalog;
    for (int i = 3; i < argc; i += 2) {
      if (i + 1 == argc)
        throw std::invalid_argument("missing fixture option value");
      const std::string_view name = argv[i];
      if (name == "--job")
        job_path = argv[i + 1];
      else if (name == "--result")
        result_path = argv[i + 1];
      else if (name == "--catalog")
        catalog = argv[i + 1];
      else
        throw std::invalid_argument("unknown fixture option: " + std::string(name));
    }
    if (job_path.empty() || result_path.empty())
      throw std::invalid_argument("fixture requires --job and --result");
    const auto job = read_job(job_path, capture);
    auto result = evaluate({executable, catalog, job_path.parent_path() / "cases", job});
    if (elaborate::result_json(result).dump().size() > job.limits.result_bytes) {
      result.status = elaborate::FixtureStatus::Error;
      result.tests.clear();
      result.error = "fixture combined report too large";
    }
    write_json(result_path, elaborate::result_json(result));
    std::cerr << result.diagnostics;
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
}
} // namespace amber::fixture
