#include "step/step.hpp"
#include <csignal>
#include <iostream>

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) {
  stopping = 1;
}
amber::step::RunOptions arguments(int argc, char **argv) {
  amber::step::RunOptions launch;
  for (int i = 1; i < argc; ++i) {
    const std::string name = argv[i];
    if (name == "run")
      continue;
    if (name == "--no-network")
      launch.network = false;
    else if (name == "--unpaced")
      launch.realtime = false;
    else if (name == "--candidate-active")
      launch.candidate_active = true;
    else if (name == "--snapshot-at-end")
      launch.snapshot_at_end = true;
    else {
      if (++i == argc)
        throw std::invalid_argument("missing value for " + name);
      const std::string value = argv[i];
      if (name == "--config")
        launch.config = value;
      else if (name == "--plan")
        launch.plan = value;
      else if (name == "--assets")
        launch.assets = value;
      else if (name == "--data")
        launch.data = value;
      else if (name == "--client")
        launch.client = value;
      else if (name == "--report")
        launch.report = value;
      else if (name == "--restore")
        launch.restore = value;
      else if (name == "--fixture-case")
        launch.fixture_case = value;
      else if (name == "--candidate")
        launch.candidate = value;
      else if (name == "--candidate-registry")
        launch.candidate_registry = value;
      else if (name == "--candidate-author")
        launch.candidate_author = std::stoul(value);
      else if (name == "--catalog")
        launch.catalog = value;
      else if (name == "--steps")
        launch.steps = std::stoull(value);
      else if (name == "--port") {
        const auto port = std::stoul(value);
        if (port > 65535)
          throw std::invalid_argument("port exceeds 65535");
        launch.port = port;
      } else
        throw std::invalid_argument("unknown argument: " + name);
    }
  }
  return launch;
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc > 1 && std::string_view(argv[1]) == "fixture")
      return amber::fixture::command(argc, argv, std::filesystem::canonical("/proc/self/exe"));
    const auto launch = arguments(argc, argv);
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    return amber::step::run(launch, stopping);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
