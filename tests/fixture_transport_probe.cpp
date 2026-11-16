// A subprocess protocol probe only. This does not simulate or validate a game world.
#include "elaborate/elaborate.hpp"

#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace amber::elaborate;
int main(int argc, char **argv) {
  if (argc != 11 || std::string_view(argv[1]) != "fixture")
    return 2;
  std::ifstream input(argv[4]);
  auto job = nlohmann::json::parse(input);
  const auto mode = std::filesystem::path(argv[2]).filename().string();
  if (mode == "blocked") {
    std::ofstream(std::string(argv[2]) + ".started") << "ready";
    std::ifstream gate(argv[2]);
    char released;
    if (!gate.get(released))
      return 3;
  }
  if (mode == "timeout")
    for (;;)
      pause();
  FixtureResult result;
  result.job = job.at("job").get<JobId>();
  result.request = job.at("request").get<RequestId>();
  result.capture = job.at("capture").get<Hash>();
  result.candidate = job.at("candidate").get<Hash>();
  result.runtime = job.at("runtime").get<std::string>();
  result.status = FixtureStatus::Completed;
  result.tests = {{{true, 1, {}}, {false, 0, {}}}};
  auto report = result_json(result);
  if (mode == "wrong")
    report["job"] = 999999;
  if (mode == "malformed")
    report["version"] = 2;
  std::cout << std::string(20000, 'x');
  std::ofstream output(argv[6]);
  output << report.dump();
  return 0;
}
