#include "net.hpp"
#include "react/react.hpp"

#include <algorithm>
#include <limits>

namespace amber::net {
namespace {
template <class T> void erase_member(T &index, net::Cell cell, std::uint64_t h) {
  auto it = index.find(cell);
  if (it == index.end())
    return;
  it->second.erase(h);
  if (it->second.empty())
    index.erase(it);
}
bool process(const react::Processes &processes, Handle h, std::span<const TermId> terms,
             std::optional<std::pair<Step, double>> time = {}) {
  for (auto term : terms)
    if (term < processes.size())
      if (const auto *p = processes[term].get(h.slot())) {
        if (p->rate == 0)
          continue;
        if (!time)
          return true;
        auto value = react::process_value(*p, time->first, time->second);
        if ((p->rate > 0 && value < p->hi) || (p->rate < 0 && value > p->lo))
          return true;
      }
  return false;
}
void process_membership(ReplicationIndex &index, Handle h, bool active) {
  auto &row = index.rows.at(h.bits);
  if (row.process)
    erase_member(index.process_cells, row.cell, h.bits);
  index.active_processes.erase(h.bits);
  row.process = active;
  if (active) {
    index.process_cells[row.cell].insert(h.bits);
    if (index.viewers.contains(row.cell))
      index.active_processes.insert(h.bits);
  }
}
ReplicationIndex::Asset asset(const catalog::Asset &a) {
  ReplicationIndex::Asset result{};
  auto nibble = [](char value) -> std::uint8_t {
    return value <= '9' ? value - '0' : value - 'a' + 10;
  };
  for (std::size_t i = 0; i < result.hash.size(); ++i)
    result.hash[i] = (nibble(a.hash[i * 2]) << 4) | nibble(a.hash[i * 2 + 1]);
  auto digest = catalog::sha256(a.canonical_params);
  for (std::size_t i = 0; i < 8; ++i)
    result.parameters |= std::uint64_t((nibble(digest[i * 2]) << 4) | nibble(digest[i * 2 + 1]))
                         << (i * 8);
  auto size = a.maximum - a.minimum;
  result.footprint_m = std::max(size.x, size.y);
  result.faces_motion = a.manifest.at("bindings").value("face_motion", false);
  result.replicated = a.replicated;
  return result;
}
void interest_count(ReplicationIndex &index, net::Cell cell, int change) {
  if (change > 0) {
    if (index.viewers[cell]++ != 0)
      return;
    if (auto members = index.cells.find(cell); members != index.cells.end())
      index.dirty.insert(members->second.begin(), members->second.end());
    if (auto running = index.process_cells.find(cell); running != index.process_cells.end())
      index.active_processes.insert(running->second.begin(), running->second.end());
  } else {
    auto count = index.viewers.find(cell);
    if (--count->second != 0)
      return;
    index.viewers.erase(count);
    if (auto members = index.cells.find(cell); members != index.cells.end())
      index.dirty.insert(members->second.begin(), members->second.end());
    if (auto running = index.process_cells.find(cell); running != index.process_cells.end())
      for (auto h : running->second)
        index.active_processes.erase(h);
  }
}
} // namespace
void register_replication(ReplicationIndex &index, Handle h, IdentityRead identity,
                          TransformRead transforms, const catalog::Models &models,
                          const catalog::ParameterBlocks &parameters, const catalog::Assets &assets,
                          const react::ValueRead &values) {
  auto effective =
      catalog::resolve_asset(models, parameters, identity.model[h.slot()], values.params[h.slot()]);
  if (!index.assets.contains(effective.hash))
    index.assets.emplace(effective.hash, asset(assets.rows.at(effective.hash)));
  const auto &binding = index.assets.at(effective.hash);
  auto cell = net::replication_cell(transforms.position[h.slot()], binding.footprint_m,
                                    index.settings.cell_m);
  bool published = index.removed.erase(h.bits) != 0;
  if (auto old = index.rows.find(h.bits); old != index.rows.end()) {
    published |= old->second.published;
    erase_member(index.cells, old->second.cell, h.bits);
    process_membership(index, h, false);
  }
  index.rows[h.bits] = {effective.hash, cell, false, published, {}};
  index.cells[cell].insert(h.bits);
  index.dirty.insert(h.bits);
  process_membership(index, h, process(values.processes, h, binding.replicated));
}
void moved(ReplicationIndex &index, Handle h, TransformRead transforms) {
  auto found = index.rows.find(h.bits);
  if (found == index.rows.end())
    return;
  auto &row = found->second;
  const auto &binding = index.assets.at(row.asset);
  auto cell = net::replication_cell(transforms.position[h.slot()], binding.footprint_m,
                                    index.settings.cell_m);
  if (cell != row.cell) {
    bool was_process = row.process;
    process_membership(index, h, false);
    erase_member(index.cells, row.cell, h.bits);
    row.cell = cell;
    index.cells[cell].insert(h.bits);
    process_membership(index, h, was_process);
  }
  index.dirty.insert(h.bits);
}
void value_changed(ReplicationIndex &index, Handle h, TermId term,
                   const react::Processes &processes) {
  auto found = index.rows.find(h.bits);
  if (found == index.rows.end())
    return;
  const auto &terms = index.assets.at(found->second.asset).replicated;
  if (std::ranges::find(terms, term) == terms.end())
    return;
  process_membership(index, h, process(processes, h, terms));
  index.dirty.insert(h.bits);
}
void changed(ReplicationIndex &index, Handle h) {
  if (index.rows.contains(h.bits))
    index.dirty.insert(h.bits);
}
void retire_replication(ReplicationIndex &index, Handle h) {
  auto it = index.rows.find(h.bits);
  if (it == index.rows.end())
    return;
  process_membership(index, h, false);
  erase_member(index.cells, it->second.cell, h.bits);
  if (it->second.published)
    index.removed.insert(h.bits);
  index.rows.erase(it);
  index.dirty.erase(h.bits);
}
void view_interest(ReplicationIndex &index, net::SessionId viewer, Vec3 position) {
  auto &old = index.interests[viewer];
  auto center = net::replication_cell(position, 0, index.settings.cell_m);
  auto desired = net::interest_cells(center, old, index.settings);
  for (auto cell : old)
    if (!desired.contains(cell))
      interest_count(index, cell, -1);
  for (auto cell : desired)
    if (!old.contains(cell))
      interest_count(index, cell, 1);
  old = std::move(desired);
}
void remove_view_interest(ReplicationIndex &index, net::SessionId viewer) {
  auto found = index.interests.find(viewer);
  if (found == index.interests.end())
    return;
  for (auto cell : found->second)
    interest_count(index, cell, -1);
  index.interests.erase(found);
}
net::Publication publication(ReplicationIndex &index, IdentityRead identity,
                             TransformRead transforms, const react::ValueRead &values, Step now,
                             double hz) {
  std::vector<std::uint64_t> due(index.dirty.begin(), index.dirty.end());
  for (auto h : index.active_processes)
    if (index.rows.at(h).sampled != now)
      due.push_back(h);
  std::sort(due.begin(), due.end());
  due.erase(std::unique(due.begin(), due.end()), due.end());
  net::Publication result{std::uint32_t(now), {}, {}};
  for (auto bits : due) {
    auto it = index.rows.find(bits);
    if (it == index.rows.end())
      continue;
    auto &row = it->second;
    if (!index.viewers.contains(row.cell)) {
      if (row.published)
        index.removed.insert(bits);
      row.published = false;
      continue;
    }
    Handle h{bits};
    const auto &a = index.assets.at(row.asset);
    net::Entity entity{h,
                       a.hash,
                       a.parameters,
                       std::uint8_t(identity.flags[h.slot()]),
                       transforms.position[h.slot()],
                       transforms.rotation[h.slot()],
                       a.footprint_m,
                       a.faces_motion,
                       {}};
    entity.states.reserve(a.replicated.size());
    for (auto term : a.replicated) {
      auto answer = react::read(values, h, term, now, hz);
      if (answer.present)
        entity.states.push_back({std::uint16_t(term), float(answer.value.numeric())});
    }
    result.upserts.push_back(std::move(entity));
    row.published = true;
    row.sampled = now;
    if (row.process && !process(values.processes, h, a.replicated, std::pair{now, hz}))
      process_membership(index, h, false);
  }
  for (auto bits : index.removed)
    result.removed.push_back({bits});
  std::sort(result.removed.begin(), result.removed.end());
  index.removed.clear();
  index.dirty.clear();
  return result;
}
} // namespace amber::net
