#pragma once
#include "snap/snap.hpp"
#include "space.hpp"

namespace amber::space {
void write_physical_relations(snap::Output &, const PhysicalRelations &,
                              const snap::ObjectSelection & = {});
void read_physical_relations(snap::Input &, PhysicalRelations &, std::size_t slots);
void write_motion(snap::Output &, const std::vector<MotionRow> &,
                  const snap::ObjectSelection & = {});
void read_motion(snap::Input &, std::vector<MotionRow> &);
} // namespace amber::space
