#pragma once

#include "bindings/resources/resources.hpp"
#include "catalog/catalog.hpp"

namespace amber::bindings {
struct ConstructionBinding {
  const catalog::Models &models;
  const catalog::ParameterBlocks &parameters;
  const react::Terms &terms;
  const react::Properties &properties;
  const react::Processes &processes;
  std::size_t exchange_max;
};
struct RemovalBinding {
  IdentityRead identity;
  const react::Terms &terms;
  const react::Properties &properties;
  const react::Processes &processes;
  std::size_t exchange_max;
};
int create_binding(lua_State *);
int remove_binding(lua_State *);
int replace_binding(lua_State *);
} // namespace amber::bindings
