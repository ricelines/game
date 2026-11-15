#pragma once

#include "perm/perm.hpp"
#include "react/react.hpp"

namespace amber::bindings {
struct PlotTransferBinding {
  perm::Plots &plots;
  perm::Grants &grants;
  perm::Players &players;
  void *invalidation = nullptr;
  void (*changed)(void *) = nullptr;
};
int transfer_plot_binding(lua_State *);
} // namespace amber::bindings
