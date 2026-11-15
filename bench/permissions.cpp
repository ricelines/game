#include "perm/perm.hpp"

#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>

int main() {
  using namespace amber;
  using namespace amber::perm;
  using Clock = std::chrono::steady_clock;
  nlohmann::json report = nlohmann::json::array();
  for (const auto count : {1000U, 10000U, 100000U, 500000U}) {
    Players players;
    players.rows.resize(std::max(64U, count / 2));
    players.rows[2].groups = {1, 5, 9, 17, 33};
    Grants grants;
    grants.rows.reserve(count + 2);
    grants.by_key.reserve(count + 1);
    const auto actor = Holder::player(2);
    for (std::uint32_t index = 1; index <= count; ++index) {
      const auto scope = Scope::object(Handle::make(index, 1));
      const auto granted = create_grant(grants, players, Holder::player(1),
                                        {1, actor, scope, bit(Verb::Use), 0, 0, 0},
                                        {scope, 1, all_rights}, {actor}, 1);
      if (!granted)
        return 1;
    }
    const auto plot_scope = Scope::owned_on_plot(1, 3);
    const auto plot_grant = create_grant(grants, players, Holder::player(1),
                                         {1, actor, plot_scope, bit(Verb::Use), 0, 0, 0},
                                         {plot_scope, 1, all_rights}, {actor}, 1);
    if (!plot_grant)
      return 1;
    constexpr std::uint32_t samples = 1000000;
    std::uint64_t checksum = 0;
    auto measure = [&](unsigned mode) {
      const auto start = Clock::now();
      for (std::uint32_t index = 0; index < samples; ++index) {
        const auto slot = mode == 1 ? 1 + index % count : count + 1 + index % 4096;
        Target target{Handle::make(slot, 1), 1, 37, bit(Verb::Use) << 8, mode == 2 ? 3U : 4U};
        const auto decision = check(actor, Verb::Use, target, players, grants, 10);
        checksum += static_cast<bool>(decision);
      }
      return std::chrono::duration<double, std::nano>(Clock::now() - start).count() / samples;
    };
    report.push_back({{"players", players.rows.size()},
                      {"grants", count + 1},
                      {"denied_ns", measure(0)},
                      {"direct_grant_ns", measure(1)},
                      {"plot_grant_ns", measure(2)},
                      {"samples_per_path", samples},
                      {"checksum", checksum}});
  }
  std::cout << report.dump(2) << '\n';
}
