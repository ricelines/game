#include "net/net.hpp"

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

#include <csignal>
#include <iostream>
#include <thread>

using namespace amber;
namespace nt = amber::net;
namespace {
volatile std::sig_atomic_t running = 1;
void stop(int) {
  running = 0;
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 4) {
    std::cerr << "usage: net_probe PORT CLIENT_ROOT ASSETS_ROOT\n";
    return 2;
  }
  auto config = toml::parse_file(AMBER_CONFIG_PATH);
  auto network = config["network"];
  auto i = [](auto v) { return v.template value<std::int64_t>().value(); };
  nt::Server server({"127.0.0.1",
                     std::uint16_t(std::stoi(argv[1])),
                     argv[2],
                     argv[3],
                     {float(network["CELL_M"].value<double>().value()),
                      int(i(network["ENTER_CELLS"])), int(i(network["LEAVE_CELLS"])),
                      std::size_t(i(network["SEND_SOFT"])), std::size_t(i(network["SEND_HARD"]))},
                     std::uint32_t(i(network["CONTROL_MAX"]))});
  auto error = server.start();
  if (!error.empty()) {
    std::cerr << error << "\n";
    return 1;
  }
  std::signal(SIGTERM, stop);
  std::signal(SIGINT, stop);
  std::cout << "ready\n" << std::flush;
  std::vector<nt::Input> inputs;
  std::uint32_t step = 0;
  auto next = std::chrono::steady_clock::now();
  auto deadline = next + std::chrono::seconds(60);
  while (running && std::chrono::steady_clock::now() < deadline) {
    server.drain(inputs);
    for (const auto &input : inputs) {
      if (const auto *control = std::get_if<nt::Control>(&input)) {
        auto message = nlohmann::json::parse(control->text, nullptr, false);
        if (message.is_discarded())
          continue;
        if (message.value("t", "") == "hello") {
          if (message.value("key", "") != "net-probe-fixture") {
            server.close(control->session, 1008, "bad_key");
            continue;
          }
          server.send(control->session, nlohmann::json{{"t", "welcome"},
                                                       {"session", control->session},
                                                       {"step", step},
                                                       {"step_hz", 20},
                                                       {"cell_m", 32},
                                                       {"assets", "/assets/"}}
                                            .dump());
          if (message.value("binary", false))
            server.view(control->session, {});
        } else if (message.value("t", "") == "ping") {
          message["t"] = "pong";
          server.send(control->session, message.dump());
        } else if (message.value("t", "") == "flood") {
          server.send(control->session, std::string(12 * 1024 * 1024, 'x'));
        }
      } else if (const auto *fault = std::get_if<nt::Fault>(&input)) {
        std::cerr << "transport fault: " << fault->reason << "\n";
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
      step += 2;
      nt::Publication publication{step, {}, {}};
      for (int n = 1; n <= 300; ++n) {
        nt::Entity row{};
        row.object = Handle::make(n, 1);
        row.position = {float(n % 20) + float(step % 200) * .01f, float(n / 20), 1};
        row.footprint_m = .1f;
        row.faces_motion = true;
        row.states = {{97, float(n) * .01f}};
        publication.upserts.push_back(std::move(row));
      }
      server.publish(std::move(publication));
      next += std::chrono::milliseconds(100);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  server.stop();
}
