#pragma once

#include "net/net.hpp"

#include <deque>
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace amber::control {
using Json = nlohmann::json;
using KeyHash = std::string;
enum class Mode : std::uint8_t { Play = 1, Watch = 2, Debug = 4 };
using Modes = std::uint8_t;
constexpr Modes modes(Mode a) {
  return static_cast<Modes>(a);
}
constexpr Modes operator|(Mode a, Mode b) {
  return modes(a) | modes(b);
}
constexpr Modes operator|(Modes a, Mode b) {
  return a | modes(b);
}
inline constexpr Modes all_modes = Mode::Play | Mode::Watch | Mode::Debug;
std::string_view mode_name(Mode);
struct Profile {
  PlayerId id = 0;
  std::string name;
  Handle body;
  std::vector<std::string> roles;
  Modes connection_modes = modes(Mode::Play);
  std::vector<KeyHash> key_hashes;
};
struct Profiles {
  std::vector<Profile> rows{Profile{}};
  std::unordered_map<KeyHash, PlayerId> by_key;
};
KeyHash hash_key(std::string_view);
void put_profile(Profiles &, Profile);
void rebuild_key_index(Profiles &);
const Profile *authenticate(const Profiles &, std::string_view key);
// Exclusive creation: returns a hash, never the random credential itself.
KeyHash create_key_file(const std::filesystem::path &);
KeyHash hash_key_file(const std::filesystem::path &);

struct Hello {
  std::string key;
  Mode mode = Mode::Watch;
  bool binary = true;
};
struct Request {
  std::uint64_t id = 0;
  std::string operation;
  Json fields = Json::object();
};
struct Parsed {
  std::variant<std::monostate, Hello, Request> message;
  std::uint64_t id = 0;
  std::string error;
};
Parsed parse(std::string_view);
using Validator = std::string (*)(const Json &);
std::string validate_look(const Json &);
std::string validate_inspect(const Json &);
std::string validate_actions(const Json &);
std::string validate_do(const Json &);
std::string validate_cancel(const Json &);
std::string validate_register_model(const Json &);
std::string validate_register_variant(const Json &);
std::string validate_register_script(const Json &);
std::string validate_authoring_failed(const Json &);
std::string validate_events(const Json &);
std::string validate_debug(const Json &);

struct Principal {
  PlayerId player = 0;
  Handle body;
  std::string name;
  std::vector<std::string> roles;
  Mode mode = Mode::Watch;
  bool binary = false;
};
struct Invocation {
  net::SessionId session = 0;
  std::uint64_t sequence = 0;
  Principal principal;
  Request request;
};
struct Reply {
  bool deferred = false;
  Json fields = Json::object();
  static Reply pending() { return {true, Json::object()}; }
  static Reply result(Json fields) { return {false, std::move(fields)}; }
};
enum class Lane { Query, Mutation };
using Handler = Reply (*)(void *, const Invocation &);
struct Operation {
  std::string name, reply_type;
  Modes allowed_modes = 0;
  Lane lane = Lane::Query;
  Validator validate = nullptr;
  Handler handler = nullptr;
  void *context = nullptr;
};
struct Transport {
  void *context = nullptr;
  void (*send)(void *, net::SessionId, std::string) = nullptr;
  void (*close)(void *, net::SessionId, std::uint16_t, std::string) = nullptr;
  void (*view)(void *, net::SessionId, Vec3) = nullptr;
};
struct Welcome {
  void *context = nullptr;
  Json (*fields)(void *, const Principal &) = nullptr;
};
class Sessions {
public:
  Sessions(const Profiles &, Transport, Welcome = {});
  void register_operation(Operation);
  void accept(const net::Input &);
  void complete(net::SessionId, std::uint64_t sequence, Json result);
  void send(net::SessionId, Json);
  void event(net::SessionId, Json);
  void event_for_player(PlayerId, const Json &);
  void broadcast(Json, Modes recipients = all_modes);
  std::optional<Principal> principal(net::SessionId) const;
  std::vector<net::SessionId> sessions_for(PlayerId) const;
  std::size_t size() const { return sessions_.size(); }

private:
  struct Pending {
    Invocation invocation;
    std::string reply_type;
    Lane lane;
  };
  struct Session {
    std::optional<Principal> principal;
    std::uint64_t next_sequence = 1;
    std::deque<Pending> mutations;
    bool dispatching = false;
    std::optional<std::uint64_t> active_mutation;
    std::unordered_map<std::uint64_t, Pending> waiting;
    std::unordered_map<std::uint64_t, std::uint64_t> ids;
  };
  const Profiles &profiles_;
  Transport transport_;
  Welcome welcome_;
  std::unordered_map<std::string, Operation> operations_;
  std::unordered_map<net::SessionId, Session> sessions_;
  std::unordered_map<PlayerId, net::SessionId> controlling_;
  void hello(net::SessionId, Hello);
  void request(net::SessionId, Request);
  void start(net::SessionId, Pending);
  void advance(net::SessionId);
  void forget(net::SessionId);
  void reject(net::SessionId, std::string);
  void error(net::SessionId, std::uint64_t, std::string);
};
} // namespace amber::control
