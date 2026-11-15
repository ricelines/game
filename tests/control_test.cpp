#include "control/control.hpp"
#include "control/snapshot.hpp"

#include <iostream>
#include <stdexcept>
#include <sys/stat.h>

using namespace amber;
using namespace amber::control;
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
  Transport callbacks() {
    return {this,
            [](void *ctx, net::SessionId id, std::string text) {
              static_cast<Wire *>(ctx)->sent.emplace_back(id, Json::parse(text));
            },
            [](void *ctx, net::SessionId id, std::uint16_t, std::string reason) {
              static_cast<Wire *>(ctx)->closed.emplace_back(id, std::move(reason));
            },
            [](void *ctx, net::SessionId id, Vec3 pos) {
              static_cast<Wire *>(ctx)->views.emplace_back(id, pos);
            }};
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
struct HandlerState {
  std::vector<Invocation> calls;
  static Reply deferred(void *ctx, const Invocation &call) {
    static_cast<HandlerState *>(ctx)->calls.push_back(call);
    return Reply::pending();
  }
  static Reply immediate(void *ctx, const Invocation &call) {
    static_cast<HandlerState *>(ctx)->calls.push_back(call);
    return Reply::result({{"ok", true}, {"sequence", call.sequence}});
  }
};
void protocol() {
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
  require(validate_look({{"full", true}}).empty() &&
              validate_look({{"limit", 3}, {"full", false}}).empty(),
          "look full is an optional boolean");
  for (const auto &bad : {Json(1), Json("true"), Json(nullptr)})
    require(!validate_look({{"full", bad}}).empty(), "nonboolean full rejected");
  require(!validate_look({{"limit", false}}).empty(), "bool is not integer");
  require(!validate_do({{"action", "arbitrary.action"}, {"player", "p:2"}}).empty(),
          "player spoof field rejected");
  require(!validate_inspect(Json::object()).empty(), "inspect target required");
  Json variant = {{"model", "m:7"},
                  {"expected_source_hash", "source"},
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
void authentication_and_presentation() {
  auto p = profiles();
  Wire wire;
  Welcome welcome{nullptr, [](void *, const Principal &) {
                    return Json{{"t", "spoof"},           {"version", 999},  {"player", "p:999"},
                                {"body", "o:9.9"},        {"mode", "debug"}, {"binary", true},
                                {"asset_root", "/assets"}};
                  }};
  Sessions sessions(p, wire.callbacks(), welcome);
  connect(sessions, 1);
  require(wire.last()["t"] == "welcome" && wire.last()["version"] == 1 &&
              wire.last()["player"] == "p:1" && wire.last()["body"] == "o:17.3" &&
              wire.last()["mode"] == "play" && wire.last()["binary"] == false &&
              wire.last()["asset_root"] == "/assets",
          "welcome extras cannot override auth");
  require(sessions.principal(1)->roles == p.rows[1].roles, "authored roles preserved");
  connect(sessions, 2, "fixture-player", "watch", true);
  require(sessions.principal(1).has_value(), "watch does not replace controlling session");
  request(sessions, 2, {{"t", "view"}, {"id", 1}, {"pos", {1, 2, 3}}});
  require(wire.views.size() == 1 && wire.views[0].second == Vec3{1, 2, 3}, "authorized Z-up view");
  connect(sessions, 3, "fixture-player");
  require(!sessions.principal(1) && sessions.principal(3), "new play replaces old");
  require(wire.closed.back() == std::pair<net::SessionId, std::string>{1, "replaced"},
          "replacement reason");
  connect(sessions, 4, "fixture-watch", "debug");
  require(wire.closed.back().second == "mode_not_allowed", "game role never grants debug");
  connect(sessions, 5, "invalid-fixture-key");
  require(wire.closed.back().second == "bad_key", "unknown credential rejected");
  const auto count = wire.sent.size();
  sessions.event_for_player(1, {{"id", 20}, {"kind", "action"}});
  require(wire.sent.size() == count + 1 && wire.last().at("t") == "event" &&
              wire.sent.back().first == 3,
          "player events reach the live play session and exclude its watch session");
  const auto rendered = Json(wire.last()).dump();
  require(rendered.find("fixture-player") == rendered.npos, "no credentials in public event");
}
void lanes_and_orphans() {
  auto p = profiles();
  Wire wire;
  HandlerState state;
  Sessions sessions(p, wire.callbacks());
  sessions.register_operation({"register_script", "result", modes(Mode::Play), Lane::Mutation,
                               validate_register_script, HandlerState::deferred, &state});
  sessions.register_operation({"do", "result", modes(Mode::Play), Lane::Mutation, validate_do,
                               HandlerState::immediate, &state});
  sessions.register_operation(
      {"look", "obs", all_modes, Lane::Query, validate_look, HandlerState::immediate, &state});
  connect(sessions, 10);
  request(sessions, 10, {{"t", "register_script"}, {"id", 1}, {"source", "authored Luau"}});
  request(sessions, 10, {{"t", "do"}, {"id", 2}, {"action", "any.action"}});
  request(sessions, 10, {{"t", "look"}, {"id", 3}});
  require(state.calls.size() == 2 && state.calls[0].sequence == 1 && state.calls[1].sequence == 3,
          "read-only query bypasses deferred mutation preserving accepted sequence");
  require(wire.last()["t"] == "obs" && wire.last()["id"] == 3, "query response correlation");
  sessions.complete(10, 1, {{"ok", true}, {"script", "s:1"}, {"id", 999}, {"t", "spoof"}});
  require(state.calls.size() == 3 && state.calls.back().sequence == 2 &&
              state.calls.back().principal.player == 1,
          "deferred completion releases original FIFO identity");
  require(wire.sent[wire.sent.size() - 2].second["id"] == 1 &&
              wire.sent[wire.sent.size() - 2].second["t"] == "result",
          "reply cannot override envelope");
  request(sessions, 10, {{"t", "register_script"}, {"id", 4}, {"source", "pending"}});
  const auto old_sequence = state.calls.back().sequence;
  connect(sessions, 11);
  const auto count = wire.sent.size();
  sessions.complete(10, old_sequence, {{"ok", true}, {"script", "s:orphan"}});
  require(wire.sent.size() == count && state.calls.back().session == 10,
          "replaced session completion neither replies nor migrates identity");
  connect(sessions, 12, "fixture-player", "watch");
  request(sessions, 12, {{"t", "do"}, {"id", 1}, {"action", "any.action"}});
  require(wire.last()["ok"] == false && state.calls.back().session == 10, "watch cannot mutate");
}
void untrusted_manifest() {
  auto p = profiles();
  Wire wire;
  Json seen;
  Sessions sessions(p, wire.callbacks());
  sessions.register_operation(
      {"register_model", "result", modes(Mode::Play), Lane::Mutation, validate_register_model,
       [](void *ctx, const Invocation &call) {
         *static_cast<Json *>(ctx) = call.request.fields;
         // The catalog binding verifies real artifacts; transport cannot approve a claimed
         // manifest.
         return Reply::result({{"ok", false}, {"errors", {"verified artifact does not match"}}});
       },
       &seen});
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
  require(seen == fields && seen["canonical_params"] == "{\"x\":1e-09}",
          "exact canonical bytes and untrusted copy preserved");
  require(wire.last()["ok"] == false && !wire.last().contains("model"),
          "transport never approves manifest");
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
