#pragma once

#include "catalog/catalog.hpp"

namespace amber::bindings::viewing {
struct PublicSources {
  std::unordered_map<catalog::Hash, std::string> assets;
  std::unordered_map<ScriptId, std::string> scripts;
};
} // namespace amber::bindings::viewing
