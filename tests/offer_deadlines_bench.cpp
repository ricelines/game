#include "perm/perm.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

int main() {
  using namespace amber;
  using Clock = std::chrono::steady_clock;
  constexpr std::size_t count = 100000, batch = 256, idle_checks = 100000;
  Objects objects;
  perm::Players players;
  players.rows.resize(2);
  perm::Grants grants;
  perm::Offers offers;
  perm::OwnerLimits limits;
  std::vector<Handle> goods;
  goods.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    ObjectInit init;
    init.model = 1;
    init.owner = 1;
    goods.push_back(create(objects, init));
  }
  const auto ids = identity(objects);
  const auto owners = authority(objects);
  const auto flags = permission_flags(objects);
  for (auto object : goods)
    if (!perm::offer(offers, flags, perm::Holder::player(1),
                     perm::target(ids, owners, limits, object), players, grants, 1, 0, 100, true,
                     1))
      throw std::runtime_error("benchmark actual offer registration failed");
  const auto idle_start = Clock::now();
  for (std::size_t i = 0; i < idle_checks; ++i)
    if (!perm::advance_offers(offers, ids, flags, 99, batch).empty())
      throw std::runtime_error("benchmark offer expired early");
  const auto idle_ns =
      std::chrono::duration<double, std::nano>(Clock::now() - idle_start).count() / idle_checks;
  std::vector<double> batches;
  std::size_t expired = 0;
  while (!offers.expiring.empty()) {
    const auto queued = offers.expiring.size();
    const auto start = Clock::now();
    const auto result = perm::advance_offers(offers, ids, flags, 100, batch);
    batches.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
    if (result.empty() || result.size() > batch || queued - offers.expiring.size() > batch)
      throw std::runtime_error("benchmark cleanup violated the deadline work quota");
    expired += result.size();
  }
  if (expired != count)
    throw std::runtime_error("benchmark failed to clean every original offer exactly once");
  for (auto object : goods)
    if (flags.get(object.slot()) & Escrow)
      throw std::runtime_error("benchmark left original goods locked after final cleanup");
  std::sort(batches.begin(), batches.end());
  const auto percentile = [&](double fraction) {
    return batches[static_cast<std::size_t>((batches.size() - 1) * fraction)];
  };
  std::cout << "{\"offers\":" << count << ",\"batch\":" << batch
            << ",\"idle_checks\":" << idle_checks << ",\"idle_ns\":" << idle_ns
            << ",\"cleanup_batches\":" << batches.size() << ",\"batch_p50_us\":" << percentile(.50)
            << ",\"batch_p95_us\":" << percentile(.95) << ",\"batch_max_us\":" << batches.back()
            << "}\n";
}
