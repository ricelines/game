#pragma once
namespace amber::net {
struct State;
}

#include "perm/perm.hpp"
#include "react/react.hpp"
#include "snap/snap.hpp"
#include "space/space.hpp"
#include "store/store.hpp"

#include <array>
#include <deque>
#include <filesystem>
#include <memory>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace amber::net {
using SessionId = std::uint64_t;
using Bytes = std::vector<std::uint8_t>;
using AssetHash = std::array<std::uint8_t, 32>;
struct ReplicatedValue {
  std::uint16_t term;
  float value;
  bool operator==(const ReplicatedValue &) const = default;
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
  std::vector<ReplicatedValue> states;
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
enum class MessageType : std::uint8_t { Keyframe = 1, Delta = 2, Drop = 3 };
struct Delivery {
  std::vector<SessionId> recipients;
  std::shared_ptr<const Bytes> bytes;
  Cell cell;
  MessageType type;
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
  // A frame written or buffered by the socket counts as sent; no peer ack is needed.
  void sent(SessionId, const Delivery &);

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
struct SendOverflow {
  SessionId session;
  std::size_t buffered_bytes;
  bool closed;
};
using Input = std::variant<Connected, Control, Disconnected, Fault, SendOverflow>;
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
struct ObjectDescription {
  Handle object;
  std::string model;
  nlohmann::json measured;
  catalog::Text says;
  std::vector<std::string> actions;
  nlohmann::json elaborated = nlohmann::json::array();
};
struct PreparedThing {
  observe::Seen seen;
  ObjectDescription description;
};
struct SelfDescription {
  Handle body, holding;
  Vec3 position;
  float facing_degrees;
  nlohmann::json intent;
  std::vector<std::pair<Handle, std::string>> zones;
};
nlohmann::json observation(Step, double hz, const SelfDescription &, std::span<const PreparedThing>,
                           std::span<const PreparedThing>,
                           std::span<const std::string> body_actions,
                           std::span<const std::string> player_actions);
nlohmann::json event_payload_json(const EventPayload &);
using ScalarBinding = std::variant<double, TermRef>;
struct LightRole {
  Handle source;
  Vec3 local_site;
  ScalarBinding intensity;
  float radius_m, source_radius_m;
};
double binding_value(const ScalarBinding &, Handle, const react::ValueRead &, Step, double hz);
ObjectDescription describe_object(Handle, IdentityRead, const amber::catalog::State &catalog_state,
                                  const react::ValueRead &,
                                  std::span<const TermId> inspection_terms,
                                  std::span<const std::string> declared_actions, Step, double hz);
nlohmann::json object_metadata(IdentityRead, AuthorityRead, ParentRead, const catalog::ModelTexts &,
                               const catalog::InstanceTexts &, std::span<const Handle>);
nlohmann::json model_metadata(const catalog::Models &, const catalog::ModelTexts &,
                              std::span<const ModelId>);
nlohmann::json term_metadata(const react::Terms &, std::span<const TermId>);
struct MaterialAppearanceRoles {
  TermId red = 0, green = 0, blue = 0, alpha = 0, metallic = 0, roughness = 0;
};
nlohmann::json material_metadata(const react::Terms &, const react::Materials &,
                                 MaterialAppearanceRoles, std::span<const TermId>);
struct ObserverBinding {
  Vec3 eye, forward;
  ScalarBinding dim_lux;
};
struct BoxSensor {
  Vec3 size, offset;
};
struct ZoneBinding {
  ScalarBinding ambient;
  int priority;
  std::optional<BoxSensor> box;
};
std::optional<LightRole> light_binding(const catalog::Asset &, const react::Terms &, Handle source,
                                       float default_source_radius_m);
std::optional<ZoneBinding> zone_binding(const catalog::Asset &, const react::Terms &);
std::optional<ObserverBinding> observer_binding(const catalog::Asset &, const react::Terms &);
observe::Observer observer(const ObserverBinding &, Handle, TransformRead, const react::ValueRead &,
                           Step, double hz);
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
std::string_view operation_name(ControlKind);
struct Request {
  std::uint64_t id = 0;
  ControlKind kind;
  Json fields = Json::object();
};
struct Parsed {
  std::variant<std::monostate, Hello, Request> message;
  std::uint64_t id = 0;
  std::string error;
};
Parsed parse(std::string_view);
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
  SessionId session = 0;
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
struct Authenticated {
  SessionId session;
  Principal principal;
};
struct Closed {
  SessionId session;
  std::optional<Principal> principal;
};
using SessionNotice = std::variant<Authenticated, Closed>;
struct Send {
  SessionId session;
  Json message;
};
struct Close {
  SessionId session;
  std::uint16_t code;
  std::string reason;
};
struct View {
  SessionId session;
  Vec3 position;
};
using SessionOutput = std::variant<Send, Close, View>;
class Sessions {
public:
  explicit Sessions(const Profiles &);
  void welcome(SessionId, Json fields);
  void drain(std::vector<Invocation> &, std::vector<SessionNotice> &, std::vector<SessionOutput> &);
  void accept(const Input &);
  void complete(SessionId, std::uint64_t sequence, Json result);
  void send(SessionId, Json);
  void event(SessionId, Json);
  void event_for_player(PlayerId, const Json &);
  void broadcast(Json, Modes recipients = all_modes);
  std::optional<Principal> principal(SessionId) const;
  std::vector<SessionId> sessions_for(PlayerId) const;
  std::size_t size() const { return sessions_.size(); }

private:
  struct Pending {
    Invocation invocation;
    bool mutation;
  };
  struct Session {
    std::optional<Principal> principal;
    std::uint64_t next_sequence = 1;
    std::deque<Pending> mutations;
    std::optional<std::uint64_t> active_mutation;
    std::unordered_map<std::uint64_t, Pending> waiting;
    std::unordered_map<std::uint64_t, std::uint64_t> ids;
  };
  const Profiles &profiles_;
  std::vector<Invocation> ready_;
  std::vector<SessionNotice> notices_;
  std::vector<SessionOutput> outputs_;
  std::unordered_map<SessionId, Session> sessions_;
  std::unordered_map<PlayerId, SessionId> controlling_;
  void hello(SessionId, Hello);
  void request(SessionId, Request);
  void start(SessionId, Pending);
  void advance(SessionId);
  void forget(SessionId);
  void reject(SessionId, std::string);
  void error(SessionId, std::uint64_t, std::string);
};
void write_profiles(snap::Output &, const Profiles &, const snap::ObjectSelection * = nullptr);
void read_profiles(snap::Input &, Profiles &);
struct PendingView {
  Step step = 0;
  bool ready = false;
  std::size_t limit = 0;
  SelfDescription self;
  std::vector<PreparedThing> things, far;
  std::vector<std::string> body_actions, player_actions;
};
struct SessionView {
  PlayerId player = 0;
  Handle body;
  Vec3 eye;
  PendingView pending;
  std::unordered_set<std::uint64_t> acquired;
  std::vector<Handle> last_observation;
};
struct SessionViews {
  std::unordered_map<SessionId, SessionView> rows;
};
struct RecordFeed {
  struct Entry {
    Json record;
    std::vector<Vec3> positions;
  };
  std::deque<Entry> rows;
  std::size_t capacity;
};
std::vector<TermId> object_action_ids(const catalog::Models &models,
                                      const catalog::Scripts &scripts,
                                      const react::InstanceScripts &instances,
                                      const react::ValueRead &values, Handle object);
std::vector<TermId> player_action_ids(const react::NativeActions &native,
                                      const react::ValueRead &values);
std::vector<std::string> object_actions(const catalog::Models &models,
                                        const catalog::Scripts &scripts,
                                        const react::InstanceScripts &instances,
                                        const react::NativeActions &native,
                                        const react::ValueRead &values, Handle object);
std::vector<std::string> player_actions(const react::NativeActions &native,
                                        const react::ValueRead &values);
void view_changed(SessionViews &views, SessionId session, Vec3 eye);
void invalidate(SessionViews &views, std::span<const Handle> changed);
void remove_session(SessionViews &views, observe::ActiveViews &active, observe::Hearing &hearing,
                    SessionId session);
void append_heard(Json &result, observe::Hearing &hearing, SessionId session, std::size_t limit);
void refresh_view(const amber::catalog::State &catalog_state,
                  const amber::react::State &react_state, const react::ValueRead &values,
                  SessionViews &sessions, amber::space::State &space_state,
                  const space::Space &space, const amber::ObjectState &objects_state,
                  const step::Intents &intents, observe::Settings settings, Step now, double hz,
                  double world_ambient_lux, SessionId session, const Principal &principal,
                  std::size_t limit);
Reply read_look(const amber::catalog::State &catalog_state, const amber::react::State &react_state,
                const react::ValueRead &values, SessionViews &sessions,
                amber::space::State &space_state, const space::Space &space,
                const amber::ObjectState &objects_state, const step::Intents &intents,
                observe::Settings settings, Step now, double hz, double world_ambient_lux,
                const Invocation &call);
Reply read_actions(const amber::catalog::State &catalog_state,
                   const amber::react::State &react_state, const react::ValueRead &values,
                   SessionViews &sessions, observe::Hearing &hearing, std::size_t page_size,
                   std::size_t events_per_response, AuthorityRead authority,
                   const amber::perm::State &perm_state, Step now, const Invocation &call);
Reply read_inspect(const amber::catalog::State &catalog_state,
                   const amber::react::State &react_state, const react::ValueRead &values,
                   SessionViews &sessions, observe::Hearing &hearing,
                   const amber::ObjectState &objects_state, const amber::perm::State &perm_state,
                   Step now, double hz, std::size_t events_per_response, const Invocation &call);
void append_records(RecordFeed &feed, std::span<const Json> records, IdentityRead identity,
                    TransformRead transforms);
Reply read_events(const RecordFeed &feed, const SessionViews &sessions, float radius_m,
                  std::size_t default_limit, const Invocation &call);
Reply read_debug(const space::Space &space, IdentityRead identity, Step now, float default_radius_m,
                 Json stats, const Invocation &call);
struct ReplicationIndex {
  struct Asset {
    AssetHash hash;
    std::uint64_t parameters;
    float footprint_m;
    bool faces_motion;
    std::vector<TermId> replicated;
  };
  struct Row {
    std::string asset;
    Cell cell;
    bool process, published;
    std::optional<Step> sampled;
  };
  Settings settings;
  std::unordered_map<std::string, Asset> assets;
  std::unordered_map<std::uint64_t, Row> rows;
  std::map<Cell, std::unordered_set<std::uint64_t>> cells, process_cells;
  std::map<Cell, std::size_t> viewers;
  std::unordered_map<SessionId, std::set<Cell>> interests;
  std::unordered_set<std::uint64_t> dirty, active_processes, removed;
  explicit ReplicationIndex(Settings value) : settings(value) {}
};
// Register again after a model/parameter rebind; no authoritative instance fields are copied.
void register_replication(ReplicationIndex &, Handle, IdentityRead, TransformRead,
                          const catalog::Models &, const catalog::ParameterBlocks &,
                          const catalog::Assets &, const react::ValueRead &);
void moved(ReplicationIndex &, Handle, TransformRead);
void value_changed(ReplicationIndex &, Handle, TermId, const react::Processes &);
void changed(ReplicationIndex &, Handle);
void retire_replication(ReplicationIndex &, Handle);
void view_interest(ReplicationIndex &, SessionId, Vec3);
void remove_view_interest(ReplicationIndex &, SessionId);
Publication publication(ReplicationIndex &, IdentityRead, TransformRead, const react::ValueRead &,
                        Step, double hz);
} // namespace amber::net

namespace amber::net {
struct State {
  Profiles profiles;
  Sessions sessions;
  SessionViews session_views;
  ReplicationIndex replication;
  RecordFeed feed;
  explicit State(Settings settings) : sessions(profiles), replication(settings), feed{{}, 4096} {}
};
} // namespace amber::net
