#include "bindings/commerce/commerce.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

int main() {
  using namespace amber;
  namespace cm = bindings::commerce;
  Objects objects;
  perm::Players players;
  players.rows.resize(3);
  perm::Ledger ledger;
  ledger.balance = {{perm::Account::player(1), 100}, {perm::Account::player(2), 100}};
  perm::Grants grants;
  perm::Groups groups;
  cm::PaymentBinding binding{ledger, grants, players, identity(objects), authority(objects),
                             groups};
  drive::NativeOperations operations;
  drive::register_native(operations, 1, {cm::validate_pay, cm::pay, &binding});
  constexpr int count = 100000;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < count; ++i) {
    const PlayerId from = i % 2 ? 2 : 1, to = i % 2 ? 1 : 2;
    auto completed =
        drive::invoke_native(operations, {{from, 1, {}, {}},
                                          {{"to", "p:" + std::to_string(to)}, {"amount", 1}},
                                          {},
                                          Step(i),
                                          20,
                                          {}});
    if (!completed.changed || completed.records.size() != 1)
      throw std::runtime_error("benchmark did not complete a real transfer and diagnostic draft");
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  if (ledger.balance[perm::Account::player(1)] != 100 ||
      ledger.balance[perm::Account::player(2)] != 100)
    throw std::runtime_error("benchmark lost currency");
  std::cout << "native validated payment + completed diagnostic: "
            << std::chrono::duration<double, std::nano>(elapsed).count() / count << " ns/op\n";
}
