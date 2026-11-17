#include "perm/perm.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

int main() {
  using namespace amber;
  using namespace amber::perm;
  using Clock = std::chrono::steady_clock;
  nlohmann::json report = nlohmann::json::array();
  for (const auto count : {100000U, 200000U}) {
    State state;
    state.players.rows.resize(7);
    state.plots.rows.push_back({1, 1, 0, 0, {0, 0, 16, 32}, {}});
    auto issue = [&](PlayerId issuer, Holder holder, Scope scope, PlayerId owner, Rights rights) {
      const auto made =
          create_grant(state.grants, state.players, Holder::player(issuer),
                       {issuer, holder, scope, rights, 0, 0, 10000}, {scope, owner, all_rights},
                       {holder, holder.kind == HolderKind::Script ? owner : holder.id, true}, 1);
      if (!made)
        throw std::runtime_error("grant benchmark setup failed");
      return made.grant;
    };
    for (unsigned i = 1; i <= count; ++i)
      issue(3, Holder::player(4), Scope::object(Handle::make(i, 1)), 3, bit(Verb::Use));
    issue(1, Holder::player(5), Scope::plot(1), 1, 24);
    std::vector<double> revoke_ns, record_ns;
    std::size_t checksum = 0;
    for (unsigned i = 0; i < 1000; ++i) {
      const auto script = Holder::script(7, Handle::make(count + 1, 1));
      const auto root = issue(1, script, Scope::plot(1), 1, 31);
      for (auto recipient : {2U, 6U}) {
        const auto made = create_grant(
            state.grants, state.players, script,
            {1, Holder::player(recipient), Scope::plot(1), 8, 0, 0, 10000},
            {Scope::plot(1), 1, all_rights}, {Holder::player(recipient), recipient, false}, 1);
        if (!made)
          throw std::runtime_error("benchmark delegation failed");
      }
      const auto start = Clock::now();
      const auto removed = revoke(state.grants, state.players, root);
      revoke_ns.push_back(std::chrono::duration<double, std::nano>(Clock::now() - start).count());
      if (removed.size() != 3)
        throw std::runtime_error("benchmark did not revoke both children");
      const auto read_start = Clock::now();
      const auto record = plot_record(state.plots, state.grants, 1, 1000, 2, state.offers, 1);
      record_ns.push_back(
          std::chrono::duration<double, std::nano>(Clock::now() - read_start).count());
      checksum += record.at("rights").size();
    }
    std::sort(revoke_ns.begin(), revoke_ns.end());
    std::sort(record_ns.begin(), record_ns.end());
    report.push_back({{"unrelated_live_grants", count},
                      {"samples", revoke_ns.size()},
                      {"revoke_root_and_two_children_ns_p50", revoke_ns[500]},
                      {"revoke_ns_p95", revoke_ns[950]},
                      {"plot_record_ns_p50", record_ns[500]},
                      {"plot_record_ns_p95", record_ns[950]},
                      {"checksum", checksum}});
  }
  std::cout << nlohmann::json(
                   {{"workload", "revoke one grant with two children; read one lot record"},
                    {"exclusions",
                     "grant creation, world startup, network, physics and script execution"},
                    {"results", report}})
                   .dump(2)
            << '\n';
}
