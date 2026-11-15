#pragma once

#include "bindings/resources/resources.hpp"
#include "catalog/catalog.hpp"

namespace amber::bindings {
struct ModelRevisionRead;
struct ModelRevisionMeasure {
  const ModelRevisionRead &read;
};
struct ModelRevisionQueue {
  const catalog::Models &models;
  const react::Properties &properties;
  const react::Processes &processes;
  const react::Terms &terms;
  std::size_t exchange_max;
};
int measure_model_change_binding(lua_State *);
int revise_model_binding(lua_State *);
} // namespace amber::bindings
