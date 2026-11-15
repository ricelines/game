#include "elaborate.hpp"
#include "private.hpp"

#include <picosha2.h>
#include <zstd.h>

#include <array>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace amber::elaborate {
namespace {
void write_file(const std::filesystem::path &path, const void *data, std::size_t bytes) {
  auto temporary = path;
  temporary += ".tmp";
  std::ofstream file(temporary, std::ios::binary);
  file.exceptions(std::ios::failbit | std::ios::badbit);
  file.write(static_cast<const char *>(data), static_cast<std::streamsize>(bytes));
  file.close();
  std::filesystem::rename(temporary, path);
}
TestRun test_run(const nlohmann::json &value) {
  return {value.at("passed").get<bool>(), value.value("observed", nlohmann::json{}),
          value.value("error", std::string{})};
}
nlohmann::json test_run_json(const TestRun &run) {
  return {{"passed", run.passed}, {"observed", run.observed}, {"error", run.error}};
}
FixtureResult run_process(const std::filesystem::path &executable,
                          const std::filesystem::path &directory, const FixtureJob &job) {
  const auto started = std::chrono::steady_clock::now();
  FixtureResult result;
  result.job = job.id;
  result.request = job.request;
  result.capture = job.capture;
  result.candidate = job.candidate.artifact;
  result.runtime = job.candidate.runtime;
  try {
    std::filesystem::create_directories(directory);
    const auto prefix = directory / std::to_string(job.id);
    const auto input_path = std::filesystem::path(prefix.string() + ".job.json");
    const auto result_path = std::filesystem::path(prefix.string() + ".result.json");
    const auto input = job_json(job).dump();
    write_file(input_path, input.data(), input.size());
    std::filesystem::remove(result_path);
    const std::array<std::string, 6> arguments{"fixture",  job.capture_path.string(),
                                               "--job",    input_path.string(),
                                               "--result", result_path.string()};
    const auto child = process::run(executable, arguments,
                                    started + std::chrono::milliseconds(job.limits.timeout_ms),
                                    job.limits.diagnostic_bytes, true);
    result.diagnostics = child.diagnostics;
    if (child.unavailable)
      result.status = FixtureStatus::Unavailable;
    if (!child.error.empty())
      throw std::runtime_error(child.error);
    if (child.exit_code)
      throw std::runtime_error("fixture process failed");
    if (!std::filesystem::is_regular_file(result_path) ||
        std::filesystem::file_size(result_path) > job.limits.result_bytes)
      throw std::runtime_error("fixture result missing or too large");
    std::ifstream file(result_path);
    const auto data = nlohmann::json::parse(file);
    auto reported = result_from_json(data);
    if (reported.job != job.id || reported.request != job.request ||
        reported.capture != job.capture || reported.candidate != job.candidate.artifact ||
        reported.runtime != job.candidate.runtime)
      throw std::runtime_error("fixture result identity mismatch");
    reported.diagnostics = std::move(result.diagnostics);
    result = std::move(reported);
  } catch (const std::exception &error) {
    result.error = error.what();
  }
  result.elapsed_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  return result;
}
} // namespace
nlohmann::json job_json(const FixtureJob &job) {
  nlohmann::json subjects = nlohmann::json::object();
  for (const auto &[name, handle] : job.candidate.subjects)
    subjects[name] = handle.bits;
  return {{"version", 1},
          {"job", job.id},
          {"request", job.request},
          {"actor", job.actor},
          {"capture", job.capture},
          {"candidate", job.candidate.artifact},
          {"script", job.candidate.script},
          {"registry", job.candidate.registry},
          {"runtime", job.candidate.runtime},
          {"subjects", std::move(subjects)},
          {"tests", job.candidate.tests},
          {"limits",
           {{"timeout_ms", job.limits.timeout_ms},
            {"diagnostic_bytes", job.limits.diagnostic_bytes},
            {"result_bytes", job.limits.result_bytes}}}};
}
FixtureResult result_from_json(const nlohmann::json &data) {
  if (data.at("version") != 1)
    throw std::invalid_argument("unsupported fixture result version");
  FixtureResult result;
  result.job = data.at("job").get<JobId>();
  result.request = data.at("request").get<RequestId>();
  result.capture = data.at("capture").get<Hash>();
  result.candidate = data.at("candidate").get<Hash>();
  result.runtime = data.at("runtime").get<std::string>();
  const auto status = data.at("status").get<std::string>();
  if (status == "completed")
    result.status = FixtureStatus::Completed;
  else if (status == "error")
    result.status = FixtureStatus::Error;
  else if (status == "unavailable")
    result.status = FixtureStatus::Unavailable;
  else
    throw std::invalid_argument("unknown fixture result status");
  result.error = data.value("error", std::string{});
  result.elapsed_ms = data.value("elapsed_ms", 0.0);
  if (data.contains("tests"))
    for (const auto &test : data["tests"])
      result.tests.push_back({test_run(test.at("candidate")), test_run(test.at("baseline"))});
  return result;
}
nlohmann::json result_json(const FixtureResult &result) {
  nlohmann::json tests = nlohmann::json::array();
  for (const auto &test : result.tests)
    tests.push_back(
        {{"candidate", test_run_json(test.candidate)}, {"baseline", test_run_json(test.baseline)}});
  return {{"version", 1},
          {"job", result.job},
          {"request", result.request},
          {"capture", result.capture},
          {"candidate", result.candidate},
          {"runtime", result.runtime},
          {"status", result.status == FixtureStatus::Completed     ? "completed"
                     : result.status == FixtureStatus::Unavailable ? "unavailable"
                                                                   : "error"},
          {"tests", std::move(tests)},
          {"error", result.error},
          {"elapsed_ms", result.elapsed_ms}};
}
struct FixtureWorker::Impl {
  std::filesystem::path executable, directory;
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<FixtureJob> pending;
  std::vector<FixtureResult> completed;
  bool stopping = false;
  std::vector<std::thread> threads;
  Impl(std::filesystem::path exe, std::filesystem::path dir, std::size_t concurrency)
      : executable(std::filesystem::absolute(exe)), directory(std::filesystem::absolute(dir)) {
    if (!concurrency)
      throw std::invalid_argument("fixture concurrency must be positive");
    for (std::size_t i = 0; i < concurrency; ++i)
      threads.emplace_back([this] {
        for (;;) {
          FixtureJob job;
          {
            std::unique_lock lock(mutex);
            ready.wait(lock, [&] { return stopping || !pending.empty(); });
            if (pending.empty())
              return;
            job = std::move(pending.front());
            pending.pop_front();
          }
          auto result = run_process(executable, directory, job);
          std::lock_guard lock(mutex);
          completed.push_back(std::move(result));
        }
      });
  }
  void finish() {
    {
      std::lock_guard lock(mutex);
      stopping = true;
    }
    ready.notify_all();
    for (auto &thread : threads)
      if (thread.joinable())
        thread.join();
  }
};
FixtureWorker::FixtureWorker(std::filesystem::path executable, std::filesystem::path directory,
                             std::size_t concurrency)
    : impl_(std::make_unique<Impl>(std::move(executable), std::move(directory), concurrency)) {}
FixtureWorker::~FixtureWorker() {
  finish();
}
void FixtureWorker::submit(FixtureJob job) {
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping)
      throw std::logic_error("fixture worker already finished");
    impl_->pending.push_back(std::move(job));
  }
  impl_->ready.notify_one();
}
std::vector<FixtureResult> FixtureWorker::take() {
  std::lock_guard lock(impl_->mutex);
  auto results = std::move(impl_->completed);
  impl_->completed.clear();
  return results;
}
void FixtureWorker::finish() {
  impl_->finish();
}
struct CaptureWriter::Impl {
  std::filesystem::path directory;
  int level;
  struct Job {
    Hash hash;
    std::vector<std::uint8_t> bytes;
  };
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<Job> pending;
  std::vector<CaptureWrite> completed;
  bool stopping = false;
  std::thread thread;
  Impl(std::filesystem::path dir, int compression)
      : directory(std::move(dir)), level(compression), thread([this] {
          for (;;) {
            Job job;
            {
              std::unique_lock lock(mutex);
              ready.wait(lock, [&] { return stopping || !pending.empty(); });
              if (pending.empty())
                return;
              job = std::move(pending.front());
              pending.pop_front();
            }
            CaptureWrite result{job.hash, directory / (job.hash + ".amb.zst"), {}};
            try {
              std::filesystem::create_directories(directory);
              std::vector<std::uint8_t> compressed(ZSTD_compressBound(job.bytes.size()));
              const auto size = ZSTD_compress(compressed.data(), compressed.size(),
                                              job.bytes.data(), job.bytes.size(), level);
              if (ZSTD_isError(size))
                throw std::runtime_error(ZSTD_getErrorName(size));
              write_file(result.path, compressed.data(), size);
            } catch (const std::exception &error) {
              result.error = error.what();
            }
            std::lock_guard lock(mutex);
            completed.push_back(std::move(result));
          }
        }) {}
  void finish() {
    {
      std::lock_guard lock(mutex);
      stopping = true;
    }
    ready.notify_one();
    if (thread.joinable())
      thread.join();
  }
};
CaptureWriter::CaptureWriter(std::filesystem::path directory, int level)
    : impl_(std::make_unique<Impl>(std::filesystem::absolute(directory), level)) {}
CaptureWriter::~CaptureWriter() {
  finish();
}
Hash CaptureWriter::submit(std::vector<std::uint8_t> bytes) {
  auto hash = picosha2::hash256_hex_string(bytes.begin(), bytes.end());
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping)
      throw std::logic_error("capture writer already finished");
    impl_->pending.push_back({hash, std::move(bytes)});
  }
  impl_->ready.notify_one();
  return hash;
}
std::vector<CaptureWrite> CaptureWriter::take() {
  std::lock_guard lock(impl_->mutex);
  auto results = std::move(impl_->completed);
  impl_->completed.clear();
  return results;
}
void CaptureWriter::finish() {
  impl_->finish();
}
} // namespace amber::elaborate
