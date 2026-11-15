#include "net.hpp"

#include <tracy/Tracy.hpp>

#include <bitsery/adapter/buffer.h>
#include <bitsery/traits/vector.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace amber::net {
namespace {
constexpr float component_range = .7071067811865475244f;
struct Wire {
  Bytes bytes;
  bitsery::Serializer<bitsery::OutputBufferAdapter<Bytes>> out{bytes};
  void header(std::uint8_t type, Cell cell) {
    out.value1b(type);
    out.value2b(cell.x);
    out.value2b(cell.y);
  }
  void varint(std::int32_t value) {
    auto word = (std::uint32_t(value) << 1) ^ std::uint32_t(-(value < 0));
    while (word >= 128) {
      out.value1b(std::uint8_t(word | 128));
      word >>= 7;
    }
    out.value1b(std::uint8_t(word));
  }
  std::shared_ptr<const Bytes> finish() {
    bytes.resize(out.adapter().writtenBytesCount());
    return std::make_shared<const Bytes>(std::move(bytes));
  }
};
struct Quantized {
  Handle object;
  AssetHash asset;
  std::uint64_t params;
  std::uint8_t flags;
  std::array<std::int32_t, 3> position;
  std::uint32_t rotation;
  std::vector<State> states;
};
void asset(Wire &wire, const Quantized &row) {
  for (auto value : row.asset)
    wire.out.value1b(value);
  wire.out.value8b(row.params);
}
void states(Wire &wire, const Quantized &row) {
  wire.out.value1b(std::uint8_t(row.states.size()));
  for (auto value : row.states) {
    wire.out.value2b(value.term);
    wire.out.value4b(value.value);
  }
}
void entity(Wire &wire, std::uint16_t local, const Quantized &row) {
  wire.out.value2b(local);
  wire.out.value8b(row.object.bits);
  asset(wire, row);
  wire.out.value1b(row.flags);
  for (auto value : row.position)
    wire.out.value4b(value);
  wire.out.value4b(row.rotation);
  states(wire, row);
}
struct Stream {
  std::map<std::uint16_t, Quantized> rows;
  std::unordered_map<std::uint64_t, std::uint16_t> local;
  std::vector<std::uint16_t> free;
  std::uint32_t next = 0;
};
struct Change {
  std::uint16_t local;
  std::uint8_t mask;
  std::array<std::int32_t, 3> delta;
};
struct Delta {
  std::vector<std::uint16_t> removed, added;
  std::vector<Change> changed;
};
std::shared_ptr<const Bytes> keyframe(Cell cell, std::uint32_t step, const Stream &stream) {
  Wire wire;
  wire.header(1, cell);
  wire.out.value4b(step);
  wire.out.value2b(std::uint16_t(stream.rows.size()));
  for (const auto &[local, row] : stream.rows)
    entity(wire, local, row);
  return wire.finish();
}
std::shared_ptr<const Bytes> delta(Cell cell, std::uint32_t step, const Stream &stream,
                                   const Delta &changes) {
  Wire wire;
  wire.header(2, cell);
  wire.out.value4b(step);
  wire.out.value2b(std::uint16_t(changes.removed.size()));
  for (auto local : changes.removed)
    wire.out.value2b(local);
  wire.out.value2b(std::uint16_t(changes.added.size()));
  for (auto local : changes.added)
    entity(wire, local, stream.rows.at(local));
  wire.out.value2b(std::uint16_t(changes.changed.size()));
  for (const auto &change : changes.changed) {
    const auto &row = stream.rows.at(change.local);
    wire.out.value2b(change.local);
    wire.out.value1b(change.mask);
    if (change.mask & 1)
      for (auto value : change.delta)
        wire.varint(value);
    if (change.mask & 2)
      wire.out.value4b(row.rotation);
    if (change.mask & 4)
      asset(wire, row);
    if (change.mask & 8)
      states(wire, row);
    if (change.mask & 16)
      wire.out.value1b(row.flags);
  }
  return wire.finish();
}
Cell cell_at(Vec3 position, float cell_m) {
  const auto x = std::floor(position.x / cell_m), y = std::floor(position.y / cell_m);
  if (x < -32768 || x > 32767 || y < -32768 || y > 32767 || !std::isfinite(x + y))
    throw std::out_of_range("replication position outside i16 cell grid");
  Cell result{std::int16_t(x), std::int16_t(y)};
  if (result == global_cell)
    throw std::out_of_range("replication position uses reserved global cell");
  return result;
}
Cell entity_cell(const Entity &row, float cell_m) {
  return replication_cell(row.position, row.footprint_m, cell_m);
}
Quantized quantize(const Entity &row, Cell cell, float cell_m) {
  if (row.states.size() > 255)
    throw std::length_error("entity exceeds255 replicated states");
  auto position = row.position;
  if (cell != global_cell) {
    position.x -= float(cell.x) * cell_m;
    position.y -= float(cell.y) * cell_m;
  }
  auto cm = [](float value) {
    const auto rounded = std::round(double(value) * 100);
    if (rounded < INT32_MIN || rounded > INT32_MAX || !std::isfinite(rounded))
      throw std::out_of_range("replication position exceeds centimeter i32");
    return std::int32_t(rounded);
  };
  return {row.object,
          row.asset,
          row.params,
          row.flags,
          {cm(position.x), cm(position.y), cm(position.z)},
          row.faces_motion ? pack_rotation({}) : pack_rotation(row.rotation),
          row.states};
}
} // namespace
Cell replication_cell(Vec3 position, float footprint_m, float cell_m) {
  return footprint_m > cell_m ? global_cell : cell_at(position, cell_m);
}
std::set<Cell> interest_cells(Cell center, const std::set<Cell> &previous,
                              const Settings &settings) {
  std::set<Cell> desired{global_cell};
  for (auto cell : previous)
    if (cell != global_cell && std::abs(int(cell.x) - center.x) <= settings.leave_cells &&
        std::abs(int(cell.y) - center.y) <= settings.leave_cells)
      desired.insert(cell);
  for (int y = -settings.enter_cells; y <= settings.enter_cells; ++y)
    for (int x = -settings.enter_cells; x <= settings.enter_cells; ++x) {
      const int i = int(center.x) + x, j = int(center.y) + y;
      if (i >= -32768 && i <= 32767 && j >= -32768 && j <= 32767)
        desired.insert({std::int16_t(i), std::int16_t(j)});
    }
  return desired;
}
std::uint32_t pack_rotation(Quat rotation) {
  float q[]{rotation.x, rotation.y, rotation.z, rotation.w};
  int largest = 0;
  for (int i = 1; i < 4; ++i)
    if (std::abs(q[i]) > std::abs(q[largest]))
      largest = i;
  const auto sign = q[largest] < 0 ? -1.f : 1.f;
  std::uint32_t word = largest;
  int shift = 2;
  for (int i = 0; i < 4; ++i)
    if (i != largest) {
      auto mapped = std::clamp((q[i] * sign / component_range + 1) * .5f, 0.f, 1.f);
      word |= std::uint32_t(std::lround(mapped * 1023)) << shift;
      shift += 10;
    }
  return word;
}
Quat unpack_rotation(std::uint32_t word) {
  float q[4]{};
  const int largest = word & 3;
  int shift = 2;
  float sum = 0;
  for (int i = 0; i < 4; ++i)
    if (i != largest) {
      q[i] = (float((word >> shift) & 1023) / 1023 * 2 - 1) * component_range;
      sum += q[i] * q[i];
      shift += 10;
    }
  q[largest] = std::sqrt(std::max(0.f, 1 - sum));
  const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  return {q[0] / length, q[1] / length, q[2] / length, q[3] / length};
}
struct Replicator::Data {
  struct Viewer {
    Cell center;
    std::set<Cell> cells, sent;
    bool writable = true, stale = true, moved = true;
  };
  Settings settings;
  std::map<Cell, Stream> streams;
  std::unordered_map<std::uint64_t, Cell> locations;
  std::map<SessionId, Viewer> viewers;
  explicit Data(Settings s) : settings(s) {
    if (!(s.cell_m > 0) || s.enter_cells < 0 || s.leave_cells < s.enter_cells)
      throw std::invalid_argument("invalid replication grid settings");
  }
};
Replicator::Replicator(Settings s) : data(std::make_unique<Data>(s)) {}
Replicator::~Replicator() = default;
void Replicator::set_view(SessionId session, Vec3 position) {
  auto center = cell_at(position, data->settings.cell_m);
  auto [it, added] = data->viewers.try_emplace(session, Data::Viewer{center, {}, {}});
  if (!added && it->second.center != center) {
    it->second.center = center;
    it->second.moved = true;
  }
}
void Replicator::remove_viewer(SessionId session) {
  data->viewers.erase(session);
}
void Replicator::set_writable(SessionId session, bool writable) {
  auto found = data->viewers.find(session);
  if (found == data->viewers.end())
    return;
  auto &viewer = found->second;
  viewer.writable = writable;
  if (!writable)
    viewer.stale = true;
}
void Replicator::sent(SessionId session, const Delivery &delivery) {
  auto found = data->viewers.find(session);
  if (found == data->viewers.end())
    return;
  if (delivery.type == MessageType::Drop)
    found->second.sent.erase(delivery.cell);
  else if (delivery.type == MessageType::Keyframe)
    found->second.sent.insert(delivery.cell);
}
std::vector<Delivery> Replicator::advance(Publication publication) {
  ZoneScopedN("replication encode and delivery grouping");
  ZoneValue(publication.upserts.size());
  auto &n = *data;
  std::map<std::uint64_t, const Entity *> latest;
  for (const auto &row : publication.upserts)
    latest[row.object.bits] = &row;
  // Validate touched-cell capacity before mutating any baseline. S6 counts are u16.
  std::unordered_map<std::uint64_t, std::optional<Cell>> planned;
  std::map<Cell, std::int64_t> counts;
  auto plan = [&](std::uint64_t bits, std::optional<Cell> next) {
    auto [it, inserted] = planned.try_emplace(bits);
    if (inserted) {
      auto old = n.locations.find(bits);
      if (old != n.locations.end())
        it->second = old->second;
    }
    if (it->second == next)
      return;
    auto count = [&](Cell cell) -> auto & {
      auto [found, added] = counts.try_emplace(cell);
      if (added) {
        auto stream = n.streams.find(cell);
        found->second = stream == n.streams.end() ? 0 : stream->second.rows.size();
      }
      return found->second;
    };
    if (it->second)
      --count(*it->second);
    if (next)
      ++count(*next);
    it->second = next;
  };
  for (auto object : publication.removed)
    plan(object.bits, {});
  std::map<std::uint64_t, Quantized> quantized;
  for (auto [bits, row] : latest) {
    auto cell = entity_cell(*row, n.settings.cell_m);
    quantized.emplace(bits, quantize(*row, cell, n.settings.cell_m));
    plan(bits, cell);
  }
  for (auto [cell, count] : counts) {
    (void)cell;
    if (count > 65535)
      throw std::length_error("replication cell exceeds65535 entities");
  }
  const std::set<Handle> removed(publication.removed.begin(), publication.removed.end());
  for (const auto &[bits, value] : quantized) {
    auto old = n.locations.find(bits);
    if (old == n.locations.end() || removed.contains(Handle{bits}) ||
        old->second != *planned.at(bits))
      continue;
    const auto &stream = n.streams.at(old->second);
    const auto &previous = stream.rows.at(stream.local.at(bits));
    for (int i = 0; i < 3; ++i) {
      const auto difference = std::int64_t(value.position[i]) - previous.position[i];
      if (difference < INT32_MIN || difference > INT32_MAX)
        throw std::out_of_range("replication displacement exceeds i32");
    }
  }
  std::map<Cell, Delta> changed;
  auto remove = [&](std::uint64_t bits) {
    auto location = n.locations.find(bits);
    if (location == n.locations.end())
      return;
    auto &stream = n.streams.at(location->second);
    auto local = stream.local.at(bits);
    changed[location->second].removed.push_back(local);
    stream.rows.erase(local);
    stream.local.erase(bits);
    stream.free.push_back(local);
    n.locations.erase(location);
  };
  for (auto object : publication.removed)
    remove(object.bits);
  for (auto [bits, row] : latest) {
    auto location = n.locations.find(bits);
    auto next = entity_cell(*row, n.settings.cell_m);
    if (location != n.locations.end() && location->second != next)
      remove(bits);
  }
  for (auto [bits, row] : latest) {
    const auto cell = entity_cell(*row, n.settings.cell_m);
    auto &stream = n.streams[cell];
    auto found = stream.local.find(bits);
    auto value = std::move(quantized.at(bits));
    if (found == stream.local.end()) {
      auto local = stream.free.empty() ? std::uint16_t(stream.next++) : stream.free.back();
      if (!stream.free.empty())
        stream.free.pop_back();
      stream.local.emplace(bits, local);
      stream.rows.emplace(local, std::move(value));
      n.locations[bits] = cell;
      changed[cell].added.push_back(local);
    } else {
      auto &previous = stream.rows.at(found->second);
      Change change{found->second, 0, {}};
      if (previous.position != value.position) {
        change.mask |= 1;
        for (int i = 0; i < 3; ++i) {
          change.delta[i] = std::int32_t(std::int64_t(value.position[i]) - previous.position[i]);
        }
      }
      if (previous.rotation != value.rotation)
        change.mask |= 2;
      if (previous.asset != value.asset || previous.params != value.params)
        change.mask |= 4;
      if (previous.states != value.states)
        change.mask |= 8;
      if (previous.flags != value.flags)
        change.mask |= 16;
      previous = std::move(value);
      if (change.mask)
        changed[cell].changed.push_back(change);
    }
  }
  std::map<Cell, std::vector<SessionId>> keys, deltas, drops;
  for (auto &[session, viewer] : n.viewers) {
    if (viewer.moved) {
      viewer.cells = interest_cells(viewer.center, viewer.cells, n.settings);
      viewer.moved = false;
    }
    if (!viewer.writable)
      continue;
    for (auto cell : viewer.sent)
      if (!viewer.cells.contains(cell))
        drops[cell].push_back(session);
    for (auto cell : viewer.cells) {
      if (viewer.stale || !viewer.sent.contains(cell))
        keys[cell].push_back(session);
      else if (changed.contains(cell))
        deltas[cell].push_back(session);
    }
    viewer.stale = false;
  }
  std::vector<Delivery> deliveries;
  for (auto &[cell, recipients] : drops) {
    Wire wire;
    wire.header(3, cell);
    deliveries.push_back({std::move(recipients), wire.finish(), cell, MessageType::Drop});
  }
  for (auto &[cell, recipients] : deltas)
    deliveries.push_back({std::move(recipients),
                          delta(cell, publication.step, n.streams.at(cell), changed.at(cell)), cell,
                          MessageType::Delta});
  const Stream empty;
  for (auto &[cell, recipients] : keys) {
    auto found = n.streams.find(cell);
    deliveries.push_back(
        {std::move(recipients),
         keyframe(cell, publication.step, found == n.streams.end() ? empty : found->second), cell,
         MessageType::Keyframe});
  }
  // Empty streams need no retained baseline after removals are serialized.
  for (const auto &[cell, changes] : changed) {
    (void)changes;
    auto found = n.streams.find(cell);
    if (found != n.streams.end() && found->second.rows.empty())
      n.streams.erase(found);
  }
  return deliveries;
}
} // namespace amber::net
