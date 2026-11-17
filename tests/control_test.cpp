#include "net/net.hpp"

#include <iostream>
#include <stdexcept>
#include <sys/stat.h>

using namespace amber;
using namespace amber::net;
namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class Fn> void rejects(Fn fn, const char *message) {
  try {
    fn();
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error(message);
}
struct Wire {
  std::vector<std::pair<net::SessionId, Json>> sent;
  std::vector<std::pair<net::SessionId, std::string>> closed;
  std::vector<std::pair<net::SessionId, Vec3>> views;
  std::vector<Invocation> drain(Sessions &sessions) {
    std::vector<Invocation> ready;
    std::vector<SessionNotice> notices;
    std::vector<SessionOutput> outputs;
    sessions.drain(ready, notices, outputs);
    for (const auto &notice : notices)
      if (const auto *login = std::get_if<Authenticated>(&notice))
        sessions.welcome(login->session, {{"asset_root", "/assets"}, {"player", "p:999"}});
    std::vector<Invocation> more;
    sessions.drain(more, notices, outputs);
    require(more.empty(), "welcome must not dispatch game inputs");
    for (auto &output : outputs)
      std::visit(
          [&](auto &row) {
            using T = std::decay_t<decltype(row)>;
            if constexpr (std::is_same_v<T, Send>)
              sent.emplace_back(row.session, std::move(row.message));
            else if constexpr (std::is_same_v<T, Close>)
              closed.emplace_back(row.session, std::move(row.reason));
            else
              views.emplace_back(row.session, row.position);
          },
          output);
    return ready;
  }
  const Json &last() const { return sent.back().second; }
};
Profiles profiles() {
  Profiles out;
  put_profile(out, {1,
                    "Test player",
                    Handle::make(17, 3),
                    {"architect", "invented.role"},
                    all_modes,
                    {hash_key("fixture-player")}});
  put_profile(
      out,
      {2, "Test observer", {}, {"elaborator"}, modes(Mode::Watch), {hash_key("fixture-watch")}});
  return out;
}
void connect(Sessions &sessions, net::SessionId id, std::string key = "fixture-player",
             std::string mode = "play", bool binary = false) {
  sessions.accept(net::Connected{id});
  sessions.accept(net::Control{
      id, Json{{"t", "hello"}, {"version", 1}, {"key", key}, {"mode", mode}, {"binary", binary}}
              .dump()});
}
void request(Sessions &sessions, net::SessionId id, Json message) {
  sessions.accept(net::Control{id, message.dump()});
}
void protocol() {
  const auto role = parse(
      R"({"t":"operator","version":1,"key":"host-test","op":"role","player":"p:2","role":"architect","on":true})");
  const auto &role_request = std::get<OperatorRequest>(role.message);
  require(role.error.empty() && role_request.player == 2 && role_request.op == "role" &&
              role_request.role == "architect" && role_request.on,
          "operator roles parse as host admission without a request id");
  const auto rekey =
      parse(R"({"t":"operator","version":1,"key":"host-test","op":"key","player":"p:2"})");
  require(rekey.error.empty() && std::get<OperatorRequest>(rekey.message).op == "key",
          "operator replacement parses independently of a player session");
  for (const auto player : {"p:0", "p:-1", "p:4294967297", "p:2suffix", "o:2.1", "p:"}) {
    const Json message = {
        {"t", "operator"}, {"version", 1}, {"key", "host-test"}, {"op", "key"}, {"player", player}};
    require(!parse(message.dump()).error.empty(),
            "operator player identifiers are complete and never narrow to another player");
  }
  for (
      const auto bad :
      {R"({"t":"operator","version":1,"key":"host-test","op":"role","player":"p:2","role":"root","on":true})",
       R"({"t":"operator","version":1,"key":"host-test","op":"role","player":"p:2","role":"architect","on":1})",
       R"({"t":"operator","version":1,"key":"host-test","op":"key","player":"p:2","on":true})",
       R"({"t":"operator","version":2,"key":"host-test","op":"key","player":"p:2"})"})
    require(!parse(bad).error.empty(), "operator wire fields match the selected operation");
  const auto join = parse(R"({"t":"join","version":1,"name":"Mira"})");
  require(join.error.empty() && std::get<JoinRequest>(join.message).name == "Mira",
          "join accepts a name without a credential or request id");
  for (const auto bad :
       {R"({"t":"join","version":2,"name":"Mira"})", R"({"t":"join","version":1,"name":false})",
        R"({"t":"join","version":1,"name":"Mira","purpose":"test"})"})
    require(!parse(bad).error.empty(), "join wire fields are validated");
  require(std::holds_alternative<Hello>(
              parse(R"({"t":"hello","version":1,"key":"x","mode":"play"})").message),
          "hello parsed");
  require(!parse(R"({"t":"hello","version":2,"key":"x","mode":"play"})").error.empty(),
          "version rejected");
  require(std::holds_alternative<Request>(parse(R"({"t":"view","pos":[1,2,3]})").message),
          "uncorrelated view update needs no request id");
  require(!parse(R"({"t":"hello","version":1,"key":"x","mode":"architect"})").error.empty(),
          "game role not mode");
  for (auto bad : {R"([])", R"({"t":"look","id":true})", R"({"t":"look","id":0})",
                   R"({"t":"look","id":1.5})", R"({"t":8,"id":1})"})
    require(!parse(bad).error.empty(), "malformed envelope rejected");
  require(validate_look({{"limit", 1}}).empty(), "look valid");
  require(!validate_look({{"full", true}}).empty(),
          "native look must not manage gateway presentation state");
  require(!validate_look({{"limit", false}}).empty(), "bool is not integer");
  require(!validate_do({{"action", "arbitrary.action"}, {"player", "p:2"}}).empty(),
          "player spoof field rejected");
  require(!validate_inspect(Json::object()).empty(), "inspect target required");
  Json variant = {{"model", "m:7"},
                  {"asset", "asset"},
                  {"canonical_params", "{}"},
                  {"manifest", Json::object()}};
  require(validate_register_variant(variant).empty(), "variant completion has an explicit schema");
  variant["model"] = 7;
  require(!validate_register_variant(variant).empty(), "variant model selector must be typed");
  variant["model"] = "m:7";
  variant["owner"] = "p:4";
  require(!validate_register_variant(variant).empty(),
          "variant authority cannot come from caller JSON");
  Json failed = {{"job", "actual-job"},
                 {"operation", "model"},
                 {"source_hash", std::string(64, 'a')},
                 {"errors", {"worker rejected CAD"}}};
  require(validate_authoring_failed(failed).empty(), "failed authoring DTO rejected");
  failed["player"] = "p:7";
  require(!validate_authoring_failed(failed).empty(), "authoring failure accepted actor spoof");
  failed.erase("player");
  failed["errors"] = Json::array();
  require(!validate_authoring_failed(failed).empty(), "authoring failure discarded its reason");
  require(validate_actions({{"page", 0}, {"name", "invented.action"}}).empty(),
          "actions optional fields");
  require(validate_events({{"root", 2}, {"limit", 200}}).empty(), "causal query fields");
  require(validate_events({{"detail", false}}).empty() &&
              !validate_events({{"detail", "false"}}).empty(),
          "event detail projection must be a boolean");
  require(!validate_debug({{"kind", "nav"}, {"around", {0, "x", 0}}}).empty(),
          "numeric diagnostics");
}
void joining_uses_the_existing_session() {
  auto p = profiles();
  Sessions sessions(p);
  Wire wire;
  sessions.accept(Connected{4});
  const auto admission = sessions.accept(Control{4, R"({"t":"join","version":1,"name":"Mira"})"});
  require(admission && admission->session == 4 && !sessions.principal(4) && p.rows.size() == 3,
          "join reaches the step owner before creating or authenticating a player");
  const auto &join = std::get<JoinRequest>(admission->request);
  require(join_allowed(join), "the prototype admits joins");
  const auto key = generate_key();
  put_profile(p, {3, join.name, Handle::make(18, 1), {}, modes(Mode::Play), {hash_key(key)}});
  const auto login = sessions.establish(4, p.rows[3], Mode::Play, false);
  require(login.session == 4 && login.principal.player == 3 &&
              login.principal.body == Handle::make(18, 1) && !login.principal.binary,
          "join uses shared establishment with the caller's explicit replication choice");
  sessions.send(4, {{"t", "joined"}, {"player", "p:3"}, {"key", key}});
  sessions.welcome(login.session, {{"step", 7}});
  wire.drain(sessions);
  require(wire.sent.size() == 2 && wire.sent[0].second["t"] == "joined" &&
              wire.sent[1].second["t"] == "welcome",
          "joining replies once before the ordinary welcome without a late authentication notice");
  request(sessions, 4, {{"t", "look"}, {"id", 1}});
  const auto ready = wire.drain(sessions);
  require(ready.size() == 1 && ready[0].principal.player == 3,
          "the joined connection immediately uses ordinary authenticated controls");
  require(!sessions.accept(Control{4, R"({"t":"join","version":1,"name":"Other"})"}),
          "an authenticated connection cannot create another join admission");
  wire.drain(sessions);
  require(wire.closed.back().second == "already_authenticated",
          "authenticated join is rejected by the handshake boundary");
  connect(sessions, 5, key);
  wire.drain(sessions);
  require(sessions.principal(5)->player == 3,
          "a generated join key reconnects through the ordinary hello path");
}
void operator_admission_is_not_a_player_session() {
  auto p = profiles();
  Sessions sessions(p);
  Wire wire;
  sessions.accept(Connected{8});
  const auto admission = sessions.accept(Control{
      8,
      R"({"t":"operator","version":1,"key":"host-test","op":"role","player":"p:1","role":"elaborator","on":true})"});
  require(admission && admission->session == 8 &&
              std::get<OperatorRequest>(admission->request).player == 1 && !sessions.principal(8) &&
              p.rows[1].roles.size() == 2,
          "operator transport defers key authority and role mutation to the step owner");
  connect(sessions, 9);
  wire.drain(sessions);
  require(!sessions.accept(Control{
              9, R"({"t":"operator","version":1,"key":"host-test","op":"key","player":"p:1"})"}),
          "a player connection cannot switch to operator admission");
  wire.drain(sessions);
  require(wire.closed.back().second == "already_authenticated",
          "operator admission uses the existing handshake boundary");
}
void authentication_and_presentation() {
  auto p = profiles();
  Wire wire;
  Sessions sessions(p);
  connect(sessions, 1);
  wire.drain(sessions);
  require(wire.last()["t"] == "welcome" && wire.last()["version"] == 1 &&
              wire.last()["player"] == "p:1" && wire.last()["body"] == "o:17.3" &&
              wire.last()["mode"] == "play" && wire.last()["binary"] == false &&
              wire.last()["asset_root"] == "/assets",
          "welcome extras cannot override auth");
  connect(sessions, 2, "fixture-player", "watch", true);
  wire.drain(sessions);
  require(sessions.principal(1).has_value(), "watch does not replace controlling session");
  request(sessions, 2, {{"t", "view"}, {"id", 1}, {"pos", {1, 2, 3}}});
  wire.drain(sessions);
  require(wire.views.size() == 1 && wire.views[0].second == Vec3{1, 2, 3}, "authorized Z-up view");
  connect(sessions, 3, "fixture-player");
  wire.drain(sessions);
  require(!sessions.principal(1) && sessions.principal(3), "new play replaces old");
  require(wire.closed.back() == std::pair<net::SessionId, std::string>{1, "replaced"},
          "replacement reason");
  connect(sessions, 4, "fixture-watch", "debug");
  wire.drain(sessions);
  require(wire.closed.back().second == "mode_not_allowed", "game role never grants debug");
  connect(sessions, 5, "invalid-fixture-key");
  wire.drain(sessions);
  require(wire.closed.back().second == "bad_key", "unknown credential rejected");
  connect(sessions, 6, "fixture-watch", "watch");
  wire.drain(sessions);
  const auto count = wire.sent.size();
  sessions.accept(Disconnected{1, 1000, {}});
  sessions.event_for_player(999, {{"id", 18}, {"kind", "action"}});
  sessions.event_for_player(2, {{"id", 19}, {"kind", "action"}});
  wire.drain(sessions);
  require(wire.sent.size() == count,
          "player events must ignore missing players and watch-only connections");
  sessions.event_for_player(1, {{"id", 20}, {"kind", "action"}});
  wire.drain(sessions);
  require(wire.sent.size() == count + 1 && wire.last().at("t") == "event" &&
              wire.sent.back().first == 3,
          "player events reach the live play session and exclude its watch session");
  const auto rendered = Json(wire.last()).dump();
  require(rendered.find("fixture-player") == rendered.npos, "no credentials in public event");
  sessions.accept(Disconnected{3, 1000, {}});
  sessions.event_for_player(1, {{"id", 21}, {"kind", "action"}});
  wire.drain(sessions);
  require(wire.sent.size() == count + 1,
          "disconnecting the play session must not redirect player events to its watcher");
  connect(sessions, 7);
  wire.drain(sessions);
  const auto reconnected_count = wire.sent.size();
  sessions.event_for_player(1, {{"id", 22}, {"kind", "action"}});
  wire.drain(sessions);
  require(wire.sent.size() == reconnected_count + 1 && wire.sent.back().first == 7 &&
              wire.last().at("t") == "event" && wire.last().at("id") == 22,
          "player events resume exactly once on the reconnected play session");
}
void lanes_and_orphans() {
  auto p = profiles();
  Wire wire;
  Sessions sessions(p);
  connect(sessions, 10);
  wire.drain(sessions);
  request(sessions, 10, {{"t", "register_script"}, {"id", 1}, {"source", "authored Luau"}});
  request(sessions, 10, {{"t", "do"}, {"id", 2}, {"action", "any.action"}});
  request(sessions, 10, {{"t", "look"}, {"id", 3}});
  auto ready = wire.drain(sessions);
  require(ready.size() == 2 && ready[0].sequence == 1 && ready[1].sequence == 3 &&
              ready[0].request.kind == ControlKind::RegisterScript &&
              ready[1].request.kind == ControlKind::Look,
          "read-only input bypasses parked mutation with original sequence");
  sessions.complete(10, 3, {{"ok", true}});
  wire.drain(sessions);
  require(wire.last()["t"] == "obs" && wire.last()["id"] == 3, "query response correlation");
  sessions.complete(10, 1, {{"ok", true}, {"script", "s:1"}, {"id", 999}, {"t", "spoof"}});
  ready = wire.drain(sessions);
  require(ready.size() == 1 && ready[0].sequence == 2 && ready[0].principal.player == 1 &&
              ready[0].request.kind == ControlKind::Do,
          "completion releases original FIFO input");
  require(wire.last()["id"] == 1 && wire.last()["t"] == "result", "reply cannot override envelope");
  sessions.complete(10, 2, {{"ok", true}});
  wire.drain(sessions);
  request(sessions, 10, {{"t", "register_script"}, {"id", 4}, {"source", "pending"}});
  ready = wire.drain(sessions);
  require(ready.size() == 1, "next mutation is admitted after completion");
  const auto old_sequence = ready[0].sequence;
  connect(sessions, 11);
  wire.drain(sessions);
  const auto count = wire.sent.size();
  sessions.complete(10, old_sequence, {{"ok", true}, {"script", "s:orphan"}});
  require(wire.drain(sessions).empty() && wire.sent.size() == count,
          "orphan completion cannot migrate identity or publish a reply");
  connect(sessions, 12, "fixture-player", "watch");
  wire.drain(sessions);
  request(sessions, 12, {{"t", "do"}, {"id", 1}, {"action", "any.action"}});
  require(wire.drain(sessions).empty() && wire.last()["ok"] == false, "watch cannot mutate");
}
void untrusted_manifest() {
  auto p = profiles();
  Wire wire;
  Sessions sessions(p);
  connect(sessions, 1);
  Json fields{{"job", "J"},
              {"asset", std::string(64, 'a')},
              {"canonical_params", "{\"x\":1e-09}"},
              {"material", "invented.material"},
              {"text", {{"name", "claim"}, {"description", "claim"}}},
              {"manifest", {{"volume", 999999}, {"approved", true}}}};
  auto message = fields;
  message["t"] = "register_model";
  message["id"] = 1;
  request(sessions, 1, message);
  const auto ready = wire.drain(sessions);
  require(ready.size() == 1, "accepted model completion reaches step once");
  const auto &seen = ready.front().request.fields;
  require(seen == fields && seen["canonical_params"] == "{\"x\":1e-09}",
          "exact canonical bytes and untrusted copy preserved");
  require(!wire.last().contains("model"), "transport never approves a manifest");
  fields["variants"] = Json::array({{{"asset", std::string(64, 'b')},
                                     {"canonical_params", "{\"x\":2}"},
                                     {"manifest", {{"volume", 888888}}}}});
  require(validate_register_model(fields).empty(), "ready variant batch rejected");
  auto invalid = fields;
  invalid["variants"][0]["actor"] = "p:999";
  require(!validate_register_model(invalid).empty(), "batch variant claimed actor accepted");
  invalid = fields;
  invalid["variants"][0]["canonical_params"] = Json::object();
  require(!validate_register_model(invalid).empty(), "batch canonical bytes not preserved");
}
void persistence_and_key_files() {
  auto p = profiles();
  const auto original = p.by_key;
  rejects(
      [&] { put_profile(p, {3, "duplicate", {}, {}, all_modes, {hash_key("fixture-player")}}); },
      "duplicate credentials rejected");
  require(p.by_key == original && p.rows.size() == 3, "profile rejection leaves index unchanged");
  auto changed = p.rows[1];
  changed.key_hashes = {hash_key("replacement"), hash_key("replacement")};
  rejects([&] { put_profile(p, changed); }, "duplicate keys in one profile are rejected");
  changed.key_hashes = {"invalid"};
  rejects([&] { put_profile(p, changed); }, "invalid key hashes are rejected before mutation");
  require(p.by_key == original && authenticate(p, "fixture-player"),
          "failed replacement preserves existing authentication");
  changed.key_hashes = {hash_key("replacement")};
  put_profile(p, changed);
  require(!authenticate(p, "fixture-player") && authenticate(p, "replacement")->id == 1 &&
              authenticate(p, "fixture-watch")->id == 2,
          "replacement invalidates only the selected player's old key");
  changed.key_hashes = {hash_key("fixture-player")};
  put_profile(p, changed);
  snap::Buffer bytes;
  snap::Output out{bytes};
  write_profiles(out, p);
  out.adapter().flush();
  bytes.resize(out.adapter().writtenBytesCount());
  const std::string payload(bytes.begin(), bytes.end());
  require(payload.find("fixture-player") == payload.npos, "PLYR has hashes not keys");
  snap::Input in{bytes.data(), bytes.size()};
  Profiles restored;
  read_profiles(in, restored);
  snap::check_complete(in);
  require(authenticate(restored, "fixture-player")->body == p.rows[1].body &&
              restored.rows[1].roles == p.rows[1].roles,
          "PLYR preserves profile identity/body/roles/hash");
  const auto path = std::filesystem::current_path() / "control-test-private.key";
  std::filesystem::remove(path);
  const auto hash = create_key_file(path);
  struct stat info{};
  require(::stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
          "random credential file mode 0600");
  require(hash.size() == 64 && hash == hash_key_file(path),
          "launch can reuse same credential hash");
  rejects([&] { create_key_file(path); }, "never overwrite private credential");
  ::chmod(path.c_str(), 0644);
  rejects([&] { hash_key_file(path); }, "insecure key mode rejected");
  std::filesystem::remove(path);
}
} // namespace
int main() {
  try {
    protocol();
    joining_uses_the_existing_session();
    operator_admission_is_not_a_player_session();
    authentication_and_presentation();
    lanes_and_orphans();
    untrusted_manifest();
    persistence_and_key_files();
    std::cout << "Native control: protocol, authentication, FIFO, replacement, artifact boundary "
                 "and PLYR passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
