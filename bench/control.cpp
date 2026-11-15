#include "control/control.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>

using namespace amber;
using namespace amber::control;
int main() {
  using Clock = std::chrono::steady_clock;
  std::size_t bytes = 0;
  Profiles profiles;
  put_profile(profiles, {1, "Benchmark", {}, {}, all_modes, {hash_key("benchmark-only")}});
  Transport transport{&bytes,
                      [](void *context, net::SessionId, std::string output) {
                        *static_cast<std::size_t *>(context) += output.size();
                      },
                      [](void *, net::SessionId, std::uint16_t, std::string) {},
                      [](void *, net::SessionId, Vec3) {}};
  Sessions sessions(profiles, transport);
  sessions.register_operation(
      {"look", "obs", all_modes, Lane::Query, validate_look,
       [](void *, const Invocation &) { return Reply::result({{"things", Json::array()}}); },
       nullptr});
  for (net::SessionId id = 1; id <= 500; ++id) {
    sessions.accept(net::Connected{id});
    sessions.accept(net::Control{
        id, R"({"t":"hello","version":1,"key":"benchmark-only","mode":"watch","binary":false})"});
  }
  std::vector<double> samples;
  constexpr int repetitions = 101;
  for (int sample = 0; sample < repetitions; ++sample) {
    const auto began = Clock::now();
    for (net::SessionId id = 1; id <= 500; ++id)
      sessions.accept(net::Control{id, R"({"t":"look","id":1,"limit":40})"});
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
    if (sample)
      samples.push_back(elapsed);
  }
  std::sort(samples.begin(), samples.end());
  std::cout << Json{{"kind", "native JSON session adapter, no gameplay or sockets"},
                    {"sessions", 500},
                    {"queries_per_batch", 500},
                    {"samples", samples.size()},
                    {"batch_ms", {{"p50", samples[49]}, {"p95", samples[94]}}},
                    {"serialized_bytes", bytes}}
                   .dump()
            << '\n';
}
