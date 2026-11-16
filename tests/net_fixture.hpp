#pragma once

#include "net/net.hpp"
#include <toml++/toml.hpp>

namespace amber::test_net {
inline net::ServerOptions options() {
  const auto config = toml::parse_file(AMBER_CONFIG_PATH);
  const auto network = config["network"];
  const auto integer = [&](const char *name) {
    return network[name].value<std::int64_t>().value();
  };
  return {"127.0.0.1",
          0,
          {},
          {},
          {float(network["CELL_M"].value<double>().value()), int(integer("ENTER_CELLS")),
           int(integer("LEAVE_CELLS")), std::size_t(integer("SEND_SOFT")),
           std::size_t(integer("SEND_HARD"))},
          std::uint32_t(integer("CONTROL_MAX"))};
}
} // namespace amber::test_net
