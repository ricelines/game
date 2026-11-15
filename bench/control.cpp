#include "net/net.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>

using namespace amber;
using namespace amber::net;
int main() {
  using Clock = std::chrono::steady_clock;
  std::size_t bytes = 0;
  Profiles profiles;
  put_profile(profiles, {1, "Benchmark", {}, {}, all_modes, {hash_key("benchmark-only")}});
  Sessions sessions(profiles);
  auto complete = [&] {
    std::vector<Invocation> ready;
    std::vector<SessionNotice> notices;
    std::vector<SessionOutput> outputs;
    sessions.drain(ready, notices, outputs);
    for (const auto &notice : notices)
      if (const auto *login = std::get_if<Authenticated>(&notice))
        sessions.welcome(login->session, Json::object());
    for (const auto &input : ready)
      sessions.complete(input.session, input.sequence, {{"things", Json::array()}});
    ready.clear();
    notices.clear();
    sessions.drain(ready, notices, outputs);
    for (const auto &output : outputs)
      if (const auto *send = std::get_if<Send>(&output))
        bytes += send->message.dump().size();
  };
  for (net::SessionId id = 1; id <= 500; ++id) {
    sessions.accept(net::Connected{id});
    sessions.accept(net::Control{
        id, R"({"t":"hello","version":1,"key":"benchmark-only","mode":"watch","binary":false})"});
  }
  complete();
  std::vector<double> samples;
  constexpr int repetitions = 101;
  for (int sample = 0; sample < repetitions; ++sample) {
    const auto began = Clock::now();
    for (net::SessionId id = 1; id <= 500; ++id)
      sessions.accept(net::Control{id, R"({"t":"look","id":1,"limit":40})"});
    complete();
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
