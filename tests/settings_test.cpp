#include "settings.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class Fn> void rejects(Fn fn) {
  try {
    fn();
  } catch (const std::invalid_argument &) {
    return;
  }
  throw std::runtime_error("invalid launch profile or allowance was accepted");
}
} // namespace

int main() {
  try {
    const auto ordinary = amber::read_settings(AMBER_CONFIG_PATH);
    const auto production = amber::read_settings(AMBER_CONFIG_PATH, "production");
    const auto development = amber::read_settings(AMBER_CONFIG_PATH, "development");
    require(ordinary.scripts.handler_ms == 5 && production.scripts.handler_ms == 5,
            "default and explicit production retain the 5 ms allowance");
    require(development.scripts.handler_ms == 100,
            "explicit development selects the finite 100 ms watchdog");
    require(development.scripts.script_kb == production.scripts.script_kb &&
                development.scripts.declaration_ms == production.scripts.declaration_ms &&
                development.event_queue_max == production.event_queue_max &&
                development.fixture_limits.timeout_ms == production.fixture_limits.timeout_ms,
            "development preserves the other protective limits");
    rejects([] { (void)amber::read_settings(AMBER_CONFIG_PATH, "developmnt"); });
    std::ifstream source(AMBER_CONFIG_PATH);
    const std::string text{std::istreambuf_iterator<char>(source), {}};
    const auto path = std::filesystem::current_path() / "invalid.toml";
    for (const auto value : {"0", "-1", "inf", "nan"}) {
      auto invalid = text;
      const auto at = invalid.find("HANDLER_MS = 100");
      require(at != std::string::npos, "development allowance exists in test configuration");
      invalid.replace(at, std::string("HANDLER_MS = 100").size(),
                      std::string("HANDLER_MS = ") + value);
      std::ofstream(path) << invalid;
      rejects([&] { (void)amber::read_settings(path, "development"); });
    }
    std::filesystem::remove(path);
    std::cout << "Launch profiles preserve production limits and a finite development watchdog.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
