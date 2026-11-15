#pragma once

#include "store/store.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <set>
#include <span>
#include <variant>

namespace amber::net {
using SessionId = std::uint64_t;
using Bytes = std::vector<std::uint8_t>;
using AssetHash = std::array<std::uint8_t, 32>;
struct State {
  std::uint16_t term;
  float value;
  bool operator==(const State &) const = default;
};
struct Entity {
  Handle object;
  AssetHash asset;
  std::uint64_t params;
  std::uint8_t flags;
  Vec3 position;
  Quat rotation;
  float footprint_m;
  bool faces_motion;
  std::vector<State> states;
};
struct Publication {
  std::uint32_t step;
  std::vector<Entity> upserts;
  std::vector<Handle> removed;
};
struct Settings {
  float cell_m;
  int enter_cells, leave_cells;
  std::size_t send_soft, send_hard;
};
struct Cell {
  std::int16_t x, y;
  auto operator<=>(const Cell &) const = default;
};
inline constexpr Cell global_cell{32767, 32767};
Cell replication_cell(Vec3 position, float footprint_m, float cell_m);
std::set<Cell> interest_cells(Cell center, const std::set<Cell> &previous, const Settings &);
struct Delivery {
  std::vector<SessionId> recipients;
  std::shared_ptr<const Bytes> bytes;
};
class Replicator {
public:
  explicit Replicator(Settings);
  ~Replicator();
  Replicator(const Replicator &) = delete;
  Replicator &operator=(const Replicator &) = delete;
  void set_view(SessionId, Vec3);
  void remove_viewer(SessionId);
  void set_writable(SessionId, bool);
  std::vector<Delivery> advance(Publication);

private:
  struct Data;
  std::unique_ptr<Data> data;
};
std::uint32_t pack_rotation(Quat);
Quat unpack_rotation(std::uint32_t);
struct ServerOptions {
  std::string bind_address;
  std::uint16_t port;
  std::filesystem::path client_root, assets_root;
  Settings replication;
  std::uint32_t max_control_bytes;
};
struct Connected {
  SessionId session;
};
struct Control {
  SessionId session;
  std::string text;
};
struct Disconnected {
  SessionId session;
  std::uint16_t code;
  std::string reason;
};
struct Fault {
  std::string reason;
};
using Input = std::variant<Connected, Control, Disconnected, Fault>;
class Server {
public:
  explicit Server(ServerOptions);
  ~Server();
  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;
  std::string start();
  void stop();
  void drain(std::vector<Input> &);
  void send(SessionId, std::string);
  void close(SessionId, std::uint16_t, std::string);
  void view(SessionId, Vec3);
  void publish(Publication);

private:
  struct Data;
  std::unique_ptr<Data> data;
};
} // namespace amber::net
