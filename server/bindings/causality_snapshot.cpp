#include "causality_snapshot.hpp"
#include "record/snapshot.hpp"

#include <stdexcept>

namespace amber::bindings {
void capture_motion_causes(const MechanicalCauses &causes, const space::Space &space,
                           IdentityRead identity, std::span<const Handle> selected,
                           std::vector<BodyCauseRow> &out) {
  out.clear();
  for (auto object : selected) {
    const auto mode = space::body_motion(space, identity, object);
    if (!mode || !mode->independent ||
        (mode->motion != space::Motion::Dynamic && mode->motion != space::Motion::Character))
      continue;
    const auto token = space::motion_cause(space, identity, object);
    if (token)
      out.push_back({object, causes.lookup(token)});
  }
}
void restore_motion_causes(MechanicalCauses &causes, space::Space &space, IdentityRead identity,
                           std::span<const BodyCauseRow> rows) {
  for (const auto &row : rows) {
    auto token = causes.restore(row.summary);
    auto result = space::restore_motion_cause(space, identity, row.object, token);
    if (result.status != space::Status::Ok) {
      throw std::runtime_error("restoring physical cause: " +
                               std::string(space::status_text(result.status)));
    }
  }
}
void write_motion_causes(snap::Output &out, const std::vector<BodyCauseRow> &rows,
                         const snap::ObjectSelection &selected) {
  std::vector<BodyCauseRow> kept;
  if (!selected.all)
    for (const auto &row : rows)
      if (selected.contains(row.object))
        kept.push_back(row);
  out.container(selected.all ? rows : kept, snap::max_collection(),
                [](snap::Output &s, BodyCauseRow &row) {
                  s.object(row.object);
                  s.object(row.summary.origin);
                  s.boolValue(row.summary.ambiguous);
                });
}
void read_motion_causes(snap::Input &in, std::vector<BodyCauseRow> &rows) {
  in.container(rows, snap::max_collection(), [](snap::Input &s, BodyCauseRow &row) {
    s.object(row.object);
    s.object(row.summary.origin);
    s.boolValue(row.summary.ambiguous);
  });
}
} // namespace amber::bindings
