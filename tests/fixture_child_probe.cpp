// Exercises the supervisor/process protocol only; this is not a game simulator.
#include "fixture.hpp"

#include <fstream>
#include <map>
#include <thread>

int main(int argc, char **argv) {
  if (argc > 1 && std::string_view(argv[1]) == "fixture")
    return amber::fixture::command(argc, argv, std::filesystem::absolute(argv[0]));
  if (argc < 2 || std::string_view(argv[1]) != "run")
    return 2;
  std::map<std::string, std::string> options;
  bool active = false, no_network = false, unpaced = false;
  for (int i = 2; i < argc; ++i) {
    const std::string name = argv[i];
    if (name == "--candidate-active")
      active = true;
    else if (name == "--no-network")
      no_network = true;
    else if (name == "--unpaced")
      unpaced = true;
    else if (++i < argc)
      options.emplace(name, argv[i]);
    else
      return 3;
  }
  if (!no_network || !unpaced || !options.contains("--catalog") ||
      !options.contains("--candidate-registry") || !options.contains("--candidate-author"))
    return 4;
  std::ifstream input(options.at("--fixture-case"));
  const auto data = nlohmann::json::parse(input);
  const auto mode = data.at("test").at("given").at("state").at("probe.mode").get<int>();
  const auto directory = std::filesystem::path(options.at("--data"));
  if (std::filesystem::exists(directory / "used"))
    return 5;
  std::ofstream(directory / "used") << "fresh process";
  std::ifstream capture(options.at("--restore"));
  int captured = 0;
  capture >> captured;
  if (mode == 3)
    return 0;
  if (mode == 4)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  if (mode == 5)
    return 6;
  nlohmann::json report{{"version", mode == 2 ? 9 : 1},
                        {"passed", active},
                        {"observed", captured + (active ? 1 : 0)},
                        {"error", mode == 1 ? "real child execution error" : ""}};
  std::ofstream(options.at("--report")) << report.dump();
  return 0;
}
