#pragma once

#include "causality.hpp"
#include "snap/snap.hpp"

namespace amber::bindings {
struct BodyCauseRow {
  Handle object;
  CauseSummary summary;
};
void capture_motion_causes(const MechanicalCauses &, const space::Space &, IdentityRead,
                           std::span<const Handle>, std::vector<BodyCauseRow> &);
void restore_motion_causes(MechanicalCauses &, space::Space &, IdentityRead,
                           std::span<const BodyCauseRow>);
void write_motion_causes(snap::Output &, const std::vector<BodyCauseRow> &,
                         const snap::ObjectSelection & = {});
void read_motion_causes(snap::Input &, std::vector<BodyCauseRow> &);
} // namespace amber::bindings
