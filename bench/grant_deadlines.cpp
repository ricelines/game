#include "perm/perm.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

int main() {
  using namespace amber;
  using namespace amber::perm;
  using Clock = std::chrono::steady_clock;
  nlohmann::json rows = nlohmann::json::array();
  for (const auto count : {1000U, 10000U, 100000U}) {
    Players players;
    players.rows.resize(2);
    Grants grants;
    const auto holder = Holder::script(2, Handle::make(99, 1));
    constexpr Step first_expiry = 1000000, reminder_steps = 600;
    constexpr std::size_t batch = 256;
    for (unsigned i = 0; i < count; ++i) {
      const auto scope = Scope::object(Handle::make(i + 1, 1));
      const auto result = create_grant(
          grants, players, {1, holder, scope, bit(Verb::Use), 0, 0, first_expiry + i % 100},
          {scope, 1, all_rights}, {holder, 1, true}, 1);
      if (!result)
        throw std::runtime_error("real grant setup failed");
    }
    std::vector<double> samples;
    samples.reserve(10000);
    for (unsigned i = 0; i < 10000; ++i) {
      const auto start = Clock::now();
      const auto due = advance_grants(grants, players, 10, reminder_steps, batch);
      samples.push_back(std::chrono::duration<double, std::nano>(Clock::now() - start).count());
      if (!due.reminders.empty() || !due.expired.empty())
        throw std::runtime_error("future grant work became due early");
    }
    std::sort(samples.begin(), samples.end());
    std::vector<double> reminder_ms, expire_ms;
    std::size_t reminded = 0, expired = 0;
    while (!grants.reminders.empty()) {
      const auto start = Clock::now();
      const auto due = advance_grants(grants, players, first_expiry - reminder_steps + 100,
                                      reminder_steps, batch);
      reminder_ms.push_back(
          std::chrono::duration<double, std::milli>(Clock::now() - start).count());
      if (due.reminders.size() > batch || !due.expired.empty())
        throw std::runtime_error("reminder batch exceeded its scheduled quota");
      reminded += due.reminders.size();
    }
    while (!grants.expiring.empty()) {
      const auto start = Clock::now();
      const auto due = advance_grants(grants, players, first_expiry + 100, reminder_steps, batch);
      expire_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
      if (due.expired.size() > batch || !due.reminders.empty())
        throw std::runtime_error("expiry batch exceeded its scheduled quota");
      expired += due.expired.size();
    }
    if (reminded != count || expired != count || !grants.by_key.empty() ||
        !grants.expiring.empty() || !grants.reminders.empty() ||
        !players.rows[1].grants_given.empty())
      throw std::runtime_error("deadline batch lost authority/index/list cleanup");
    std::sort(reminder_ms.begin(), reminder_ms.end());
    std::sort(expire_ms.begin(), expire_ms.end());
    rows.push_back({{"live_grants", count},
                    {"idle_samples", samples.size()},
                    {"idle_ns_p50", samples[samples.size() / 2]},
                    {"idle_ns_p95", samples[samples.size() * 95 / 100]},
                    {"max_entries_per_pass", batch},
                    {"actual_reminders", reminded},
                    {"reminder_passes", reminder_ms.size()},
                    {"reminder_pass_p95_ms", reminder_ms[reminder_ms.size() * 95 / 100]},
                    {"reminder_pass_max_ms", reminder_ms.back()},
                    {"actual_expired", expired},
                    {"expiry_passes", expire_ms.size()},
                    {"expiry_pass_p95_ms", expire_ms[expire_ms.size() * 95 / 100]},
                    {"expiry_pass_max_ms", expire_ms.back()}});
  }
  std::cout << rows.dump(2) << '\n';
}
