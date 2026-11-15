#pragma once

#include "react/exchange.hpp"

namespace amber::bindings {
struct ExchangeBinding {
  react::Properties &properties;
  react::Processes &processes;
  const react::Terms &terms;
  std::size_t entries_max;
};
react::ResourceExchange capture_exchange(lua_State *, int, const react::Properties &,
                                         const react::Processes &, const react::Terms &,
                                         std::size_t entries_max);
int exchange_binding(lua_State *);
} // namespace amber::bindings
