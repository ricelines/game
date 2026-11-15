#pragma once

#include "bindings/structural/structural.hpp"

namespace amber::bindings {
struct ConstructionActions {
  TermId create, destroy, reconfigure;
};
ConstructionActions declare_construction_actions(react::Terms &, command::NativeActions &);
void register_construction_actions(drive::NativeOperations &, ConstructionActions,
                                   CreateOperation &, RemoveOperation &, ReplaceOperation &);
} // namespace amber::bindings
