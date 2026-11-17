#include "snap.hpp"

#include <algorithm>

namespace amber::snap {
namespace {
void write_all(Output &out, const Objects &o) {
  out.container4b(o.generation, max_collection());
  out.container(o.position, max_collection());
  out.container(o.rotation, max_collection());
  out.container4b(o.model, max_collection());
  out.container4b(o.material, max_collection());
  out.container4b(o.owner, max_collection());
  out.container4b(o.maker, max_collection());
  out.container8b(o.created_step, max_collection());
  out.container4b(o.group, max_collection());
  out.container4b(o.preset, max_collection());
  out.container4b(o.plot, max_collection());
  out.container(o.parent, max_collection());
  out.container4b(o.flags, max_collection());
  out.container4b(o.params, max_collection());
  out.container4b(o.free_slots, max_collection());
}
} // namespace
void write_objects(Output &out, const Objects &o,
                   std::optional<std::span<const Handle>> selection) {
  if (!selection) {
    write_all(out, o);
    return;
  }
  std::size_t slots = 1;
  for (auto h : *selection) {
    assert(identity(o).valid(h));
    slots = std::max(slots, static_cast<std::size_t>(h.slot()) + 1);
  }
  // A capture preserves original slot/generation identity; the other rows are empty.
  Objects subset;
  subset.generation.assign(o.generation.begin(), o.generation.begin() + slots);
  subset.position.resize(slots);
  subset.rotation.resize(slots);
  subset.model.resize(slots);
  subset.material.resize(slots);
  subset.owner.resize(slots);
  subset.maker.resize(slots);
  subset.created_step.resize(slots);
  subset.group.resize(slots);
  subset.preset.resize(slots);
  subset.plot.resize(slots);
  subset.parent.resize(slots);
  subset.flags.resize(slots);
  subset.params.resize(slots);
  for (auto h : *selection) {
    const auto i = h.slot();
    subset.position[i] = o.position[i];
    subset.rotation[i] = o.rotation[i];
    subset.model[i] = o.model[i];
    subset.material[i] = o.material[i];
    subset.owner[i] = o.owner[i];
    subset.maker[i] = o.maker[i];
    subset.created_step[i] = o.created_step[i];
    subset.group[i] = o.group[i];
    subset.preset[i] = o.preset[i];
    set_plot(subset, i, o.plot[i]);
    subset.parent[i] = o.parent[i];
    subset.flags[i] = o.flags[i];
    subset.params[i] = o.params[i];
  }
  for (std::uint32_t i = 1; i < slots; ++i)
    if (!subset.model[i])
      subset.free_slots.push_back(i);
  write_all(out, subset);
}
void read_objects(Input &in, Objects &o) {
  in.container4b(o.generation, max_collection());
  in.container(o.position, max_collection());
  in.container(o.rotation, max_collection());
  in.container4b(o.model, max_collection());
  in.container4b(o.material, max_collection());
  in.container4b(o.owner, max_collection());
  in.container4b(o.maker, max_collection());
  in.container8b(o.created_step, max_collection());
  in.container4b(o.group, max_collection());
  in.container4b(o.preset, max_collection());
  in.container4b(o.plot, max_collection());
  in.container(o.parent, max_collection());
  in.container4b(o.flags, max_collection());
  in.container4b(o.params, max_collection());
  in.container4b(o.free_slots, max_collection());
  check_complete(in);
  const auto n = o.generation.size();
  if (!n || o.position.size() != n || o.rotation.size() != n || o.model.size() != n ||
      o.material.size() != n || o.owner.size() != n || o.maker.size() != n ||
      o.created_step.size() != n || o.group.size() != n || o.preset.size() != n ||
      o.plot.size() != n || o.parent.size() != n || o.flags.size() != n || o.params.size() != n ||
      o.model[0] || o.generation[0])
    throw std::runtime_error("inconsistent snapshot object columns");
  std::vector<bool> free(n);
  for (auto slot : o.free_slots) {
    if (!slot || slot >= n || free[slot] || o.model[slot])
      throw std::runtime_error("invalid snapshot free slot");
    free[slot] = true;
  }
  for (std::size_t slot = 1; slot < n; ++slot)
    if (!o.generation[slot] || (o.model[slot] == 0) != free[slot])
      throw std::runtime_error("snapshot object liveness disagrees with free slots");
  rebuild_plot_membership(o);
}
} // namespace amber::snap
