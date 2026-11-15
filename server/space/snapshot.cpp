#include "snap/snap.hpp"
#include "space.hpp"
#include <algorithm>

namespace amber::space {
namespace {
void write_relation(snap::Output &out, const Edges &relation,
                    const snap::ObjectSelection &selected) {
  auto column = [&](const auto &source) {
    auto field = [](snap::Output &s, auto &edges) { snap::field(s, edges); };
    if (selected.all) {
      snap::write_sparse(out, source, field);
      return;
    }
    SparseColumn<std::vector<Handle>> kept;
    for (auto slot : selected.slots)
      if (const auto *row = source.get(slot)) {
        auto edges = *row;
        std::erase_if(edges, [&](Handle other) { return !selected.contains(other); });
        if (!edges.empty())
          kept.set(slot, std::move(edges));
      }
    snap::write_sparse(out, kept, field);
  };
  column(relation.out);
  column(relation.in);
}
void read_relation(snap::Input &in, Edges &relation, std::size_t slots) {
  snap::read_sparse(in, relation.out, [](snap::Input &s, auto &edges) { snap::field(s, edges); });
  snap::read_sparse(in, relation.in, [](snap::Input &s, auto &edges) { snap::field(s, edges); });
  relation.out.rebuild_positions(slots);
  relation.in.rebuild_positions(slots);
}
} // namespace
void write_physical_relations(snap::Output &out, const PhysicalRelations &v,
                              const snap::ObjectSelection &selected) {
  write_relation(out, v.contains, selected);
  write_relation(out, v.supports, selected);
  write_relation(out, v.attached_to, selected);
  write_relation(out, v.in_zone, selected);
}
void read_physical_relations(snap::Input &in, PhysicalRelations &v, std::size_t slots) {
  read_relation(in, v.contains, slots);
  read_relation(in, v.supports, slots);
  read_relation(in, v.attached_to, slots);
  read_relation(in, v.in_zone, slots);
}
void write_motion(snap::Output &out, const std::vector<MotionRow> &rows,
                  const snap::ObjectSelection &selected) {
  std::vector<MotionRow> kept;
  if (!selected.all)
    for (const auto &row : rows)
      if (selected.contains(row.object))
        kept.push_back(row);
  out.container(selected.all ? rows : kept, snap::max_collection(),
                [](snap::Output &s, MotionRow &row) {
                  s.object(row.object);
                  s.object(row.linear_velocity_m_s);
                  s.object(row.angular_velocity_rad_s);
                  s.boolValue(row.asleep);
                });
}
void read_motion(snap::Input &in, std::vector<MotionRow> &rows) {
  in.container(rows, snap::max_collection(), [](snap::Input &s, MotionRow &row) {
    s.object(row.object);
    s.object(row.linear_velocity_m_s);
    s.object(row.angular_velocity_rad_s);
    s.boolValue(row.asleep);
  });
}
} // namespace amber::space
