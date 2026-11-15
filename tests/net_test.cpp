#include "net/net.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <numbers>
#include <random>
#include <stdexcept>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

using namespace amber;
namespace nt = amber::net;
namespace {
void require(bool value, const char *text) {
  if (!value)
    throw std::runtime_error(text);
}
struct Reader {
  std::span<const std::uint8_t> bytes;
  std::size_t offset = 0;
  std::uint64_t integer(int n) {
    require(offset + n <= bytes.size(), "truncated fixture");
    std::uint64_t value = 0;
    for (int i = 0; i < n; ++i)
      value |= std::uint64_t(bytes[offset++]) << (i * 8);
    return value;
  }
  std::int32_t signed32() { return std::bit_cast<std::int32_t>(std::uint32_t(integer(4))); }
  float number() { return std::bit_cast<float>(std::uint32_t(integer(4))); }
  std::int32_t varint() {
    std::uint32_t value = 0;
    for (int shift = 0; shift <= 28; shift += 7) {
      auto byte = integer(1);
      value |= std::uint32_t(byte & 127) << shift;
      if (!(byte & 128))
        return std::int32_t(value >> 1) ^ -std::int32_t(value & 1);
    }
    throw std::runtime_error("overlong varint");
  }
};
struct Row {
  std::uint64_t handle;
  std::array<std::uint8_t, 32> asset;
  std::uint64_t params;
  std::uint8_t flags;
  std::array<std::int32_t, 3> position;
  std::uint32_t rotation;
  std::map<std::uint16_t, float> states;
  bool operator==(const Row &) const = default;
};
void read_asset(Reader &r, Row &row) {
  for (auto &byte : row.asset)
    byte = r.integer(1);
  row.params = r.integer(8);
}
void read_states(Reader &r, Row &row) {
  row.states.clear();
  auto count = r.integer(1);
  while (count--) {
    const auto term = std::uint16_t(r.integer(2));
    row.states.emplace(term, r.number());
  }
}
Row read_entity(Reader &r) {
  Row row{};
  row.handle = r.integer(8);
  read_asset(r, row);
  row.flags = r.integer(1);
  for (auto &position : row.position)
    position = r.signed32();
  row.rotation = r.integer(4);
  read_states(r, row);
  return row;
}
struct Client {
  std::map<nt::Cell, std::map<std::uint16_t, Row>> cells;
  std::uint32_t step = 0;
  void apply(const nt::Bytes &bytes) {
    Reader r{bytes};
    const auto type = r.integer(1);
    auto i = std::bit_cast<std::int16_t>(std::uint16_t(r.integer(2)));
    auto j = std::bit_cast<std::int16_t>(std::uint16_t(r.integer(2)));
    nt::Cell cell{i, j};
    if (type == 3) {
      cells.erase(cell);
    } else {
      step = r.integer(4);
      auto &rows = cells[cell];
      if (type == 1)
        rows.clear();
      else {
        require(type == 2, "unknown fixture frame");
        auto count = r.integer(2);
        while (count--)
          rows.erase(std::uint16_t(r.integer(2)));
      }
      auto count = r.integer(2);
      while (count--) {
        const auto local = std::uint16_t(r.integer(2));
        rows[local] = read_entity(r);
      }
      if (type == 2) {
        count = r.integer(2);
        while (count--) {
          auto &row = rows.at(std::uint16_t(r.integer(2)));
          const auto mask = r.integer(1);
          if (mask & 1)
            for (auto &position : row.position)
              position += r.varint();
          if (mask & 2)
            row.rotation = r.integer(4);
          if (mask & 4)
            read_asset(r, row);
          if (mask & 8)
            read_states(r, row);
          if (mask & 16)
            row.flags = r.integer(1);
        }
      }
    }
    require(r.offset == bytes.size(), "fixture trailing bytes");
  }
};
nt::Bytes hex(std::string_view text) {
  nt::Bytes bytes;
  auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (std::size_t i = 0; i < text.size(); i += 2)
    bytes.push_back(nibble(text[i]) * 16 + nibble(text[i + 1]));
  return bytes;
}
nt::Entity entity(std::uint32_t slot = 17) {
  nt::Entity row{};
  row.object = Handle::make(slot, 1);
  for (std::size_t i = 0; i < row.asset.size(); ++i)
    row.asset[i] = i;
  row.params = 0x0807060504030201;
  row.flags = 3;
  row.position = {1.234f, 2.006f, .126f};
  row.rotation = {};
  row.footprint_m = 1;
  row.states = {{7, .5f}, {9, 1}};
  return row;
}
void deliver(const std::vector<nt::Delivery> &packets, std::map<nt::SessionId, Client> &clients) {
  for (const auto &packet : packets)
    for (auto recipient : packet.recipients)
      clients[recipient].apply(*packet.bytes);
}
const nt::Bytes *frame(const std::vector<nt::Delivery> &packets, nt::SessionId session,
                       nt::Cell cell, int type) {
  for (const auto &packet : packets) {
    Reader r{*packet.bytes};
    const auto t = r.integer(1);
    const auto i = std::bit_cast<std::int16_t>(std::uint16_t(r.integer(2)));
    const auto j = std::bit_cast<std::int16_t>(std::uint16_t(r.integer(2)));
    if (t == std::uint64_t(type) && nt::Cell{i, j} == cell &&
        std::ranges::find(packet.recipients, session) != packet.recipients.end())
      return packet.bytes.get();
  }
  return nullptr;
}
void golden_and_stream_consistency() {
  nt::Replicator replication({32, 0, 1, 262144, 4194304});
  replication.set_view(1, {});
  std::map<nt::SessionId, Client> clients;
  auto row = entity();
  auto packets = replication.advance({20, {row}, {}});
  const auto *key = frame(packets, 1, {0, 0}, 1);
  require(key, "missing initial keyframe");
  auto expected = hex("010000000014000000010000001100000001000000"
                      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
                      "0102030405060708037b000000c90000000d00000003082080"
                      "0207000000003f09000000803f");
  require(*key == expected, "keyframe differs from labelled little-endian fixture");
  deliver(packets, clients);
  row.position = {1.244f, 2.0001f, .126f};
  row.states = {{7, .75f}};
  packets = replication.advance({22, {row}, {}});
  auto expected_delta = hex("0200000000160000000000000001000000090201000107000000403f");
  const auto *change = frame(packets, 1, {0, 0}, 2);
  require(change && *change == expected_delta, "delta differs from position/state fixture");
  deliver(packets, clients);
  require(clients[1].cells[{0, 0}].at(0).states.size() == 1, "mask8 did not replace states");
  replication.set_view(2, {});
  row.position.x += .1f;
  row.asset[0] = 255;
  row.params = 9;
  row.flags = 6;
  row.rotation = {0, 0, std::sqrt(.5f), std::sqrt(.5f)};
  row.states.clear();
  packets = replication.advance({24, {row}, {}});
  require(frame(packets, 1, {0, 0}, 2) && !frame(packets, 1, {0, 0}, 1),
          "existing viewer got keyframe");
  require(frame(packets, 2, {0, 0}, 1) && !frame(packets, 2, {0, 0}, 2),
          "joiner got same-step delta");
  deliver(packets, clients);
  require(clients[1].cells == clients[2].cells, "join keyframe and delta baselines differ");
  auto replacement = row;
  replacement.object = Handle::make(18, 1);
  packets = replication.advance({26, {replacement}, {row.object}});
  deliver(packets, clients);
  require(clients[1].cells[{0, 0}].size() == 1 &&
              clients[1].cells[{0, 0}].at(0).handle == replacement.object.bits,
          "local ID was not reused after removal in same delta");
  replication.set_view(3, {33, 0, 0});
  replacement.position.x = 33.01f;
  packets = replication.advance({28, {replacement}, {}});
  deliver(packets, clients);
  require(clients[1].cells[{0, 0}].empty() && clients[3].cells[{1, 0}].at(0).position[0] == 101,
          "migration was not old removal and absolute new addition");
  require(replication.advance({30, {}, {}}).empty(), "clean cells emitted a recurring update");
  auto global = entity(19);
  global.footprint_m = 40;
  global.position = {-44.12f, 90.13f, 7};
  packets = replication.advance({32, {global}, {}});
  deliver(packets, clients);
  require(clients[1].cells[nt::global_cell].at(0).position[0] == -4412,
          "global stream position was cell-relative");
  replication.set_view(1, {33, 0, 0});
  deliver(replication.advance({34, {}, {}}), clients);
  require(clients[1].cells.contains({0, 0}), "hysteresis dropped adjacent old cell");
  replication.set_writable(1, false);
  replication.set_view(1, {129, 0, 0});
  replacement.position.x += .25f;
  packets = replication.advance({36, {replacement}, {}});
  require(std::ranges::none_of(packets,
                               [](const auto &p) {
                                 return std::ranges::find(p.recipients, 1) != p.recipients.end();
                               }),
          "stale viewer received a delta");
  replication.set_writable(1, true);
  packets = replication.advance({38, {}, {}});
  require(frame(packets, 1, {0, 0}, 3) && frame(packets, 1, {4, 0}, 1) &&
              frame(packets, 1, nt::global_cell, 1),
          "recovery did not drop obsolete cells and keyframe current cells");
  deliver(packets, clients);
  require(!clients[1].cells.contains({0, 0}) && clients[1].cells.contains({4, 0}),
          "recovery client membership mismatch");
}
void quaternion_accuracy() {
  require(nt::pack_rotation({}) == 0x80200803u, "identity quaternion fixture mismatch");
  std::mt19937 random(42);
  std::normal_distribution<float> normal;
  float worst = 0;
  for (int n = 0; n < 10000; ++n) {
    Quat q{normal(random), normal(random), normal(random), normal(random)};
    auto length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    q = {q.x / length, q.y / length, q.z / length, q.w / length};
    auto decoded = nt::unpack_rotation(nt::pack_rotation(q));
    auto dot = std::abs(q.x * decoded.x + q.y * decoded.y + q.z * decoded.z + q.w * decoded.w);
    worst = std::max(worst, 2 * std::acos(std::min(1.f, dot)));
  }
  require(worst < .006f, "smallest-three angular error too large");
}
void capacity_and_shared_encoding() {
  nt::Replicator replication({32, 0, 1, 262144, 4194304});
  for (int i = 1; i <= 500; ++i)
    replication.set_view(i, {});
  nt::Publication publication{2, {}, {}};
  for (int i = 1; i <= 300; ++i)
    publication.upserts.push_back(entity(i));
  auto packets = replication.advance(std::move(publication));
  require(packets.size() == 2 && packets[0].recipients.size() == 500 &&
              packets[1].recipients.size() == 500,
          "cell bytes were not shared across500 viewers");
  std::vector<double> timings;
  std::size_t encoded = 0;
  for (int n = 0; n < 100; ++n) {
    nt::Publication next{std::uint32_t(4 + n * 2), {}, {}};
    for (int i = 1; i <= 300; ++i) {
      auto row = entity(i);
      row.position.x += float(n + 1) * .01f;
      next.upserts.push_back(std::move(row));
    }
    auto start = std::chrono::steady_clock::now();
    packets = replication.advance(std::move(next));
    timings.push_back(
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
            .count());
    require(packets.size() == 1 && packets[0].recipients.size() == 500,
            "motion delta encoded per viewer");
    encoded = packets[0].bytes->size();
  }
  std::sort(timings.begin(), timings.end());
  std::cout << "replicate300_to500 us p50=" << timings[50] << " p95=" << timings[95]
            << " max=" << timings.back() << " stream_bytes=" << encoded << "\n";
  nt::Replicator crowded({32, 0, 1, 262144, 4194304});
  nt::Publication overflow{1, {}, {}};
  overflow.upserts.reserve(65536);
  for (int i = 1; i <= 65536; ++i)
    overflow.upserts.push_back(entity(i));
  bool rejected = false;
  try {
    crowded.advance(std::move(overflow));
  } catch (const std::length_error &) {
    rejected = true;
  }
  require(rejected, "u16 cell count overflow was not reported");
  crowded.set_view(1, {});
  auto valid = crowded.advance({2, {entity()}, {}});
  std::map<nt::SessionId, Client> clients;
  deliver(valid, clients);
  require(clients[1].cells[{0, 0}].size() == 1, "rejected publication partially mutated baseline");
}
void empty_world_roaming_does_not_retain_streams() {
#if defined(__GLIBC__)
  nt::Replicator replication({32, 0, 1, 262144, 4194304});
  replication.set_view(1, {});
  replication.advance({0, {}, {}});
  const auto before = mallinfo2().uordblks;
  for (int i = 1; i <= 16384; ++i) {
    replication.set_view(1, {float(i % 128) * 128, float(i / 128) * 128, 0});
    auto frames = replication.advance({std::uint32_t(i), {}, {}});
    require(!frames.empty(), "roaming viewer got no new keyframe");
  }
  const auto after = mallinfo2().uordblks;
  const auto growth = after > before ? after - before : 0;
  std::cout << "empty_world_roam16384 heap_growth_bytes=" << growth << "\n";
  require(growth < 262144, "empty-world roaming retained stream allocations");
#endif
}

} // namespace
int main() {
  try {
    golden_and_stream_consistency();
    quaternion_accuracy();
    capacity_and_shared_encoding();
    empty_world_roaming_does_not_retain_streams();
    std::cout << "net codec tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << "net test: " << error.what() << "\n";
    return 1;
  }
}
