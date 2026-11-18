#include "catalog/catalog.hpp"
#include "net_fixture.hpp"
#include "settings.hpp"
#include "step/run_private.hpp"
#include "step/start_private.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>

using namespace amber;
using namespace amber::step::start;
namespace {
void require(bool value, const char *message) {
  if (!value)
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
void check_operator(net::State &net, perm::State &permissions, catalog::State &catalog,
                    const ObjectState &objects, const Settings &settings, PlayerId player,
                    const std::string &old_key) {
  const auto group = settings.land.way_group;
  const step::Clock clock{.step = 8, .hz = settings.clock.step_hz};
  const std::string credential = "test host credential";
  const auto operator_hash = net::hash_key(credential);
  record::Journal journal(10, {});
  net::SessionId next_session = 200;
  auto command = [&](Json fields) {
    const auto session = next_session++;
    fields.update({{"t", "operator"}, {"version", 1}});
    net.sessions.accept(net::Connected{session});
    const auto admitted = net.sessions.accept(net::Control{session, fields.dump()});
    require(admitted.has_value(), "operator wire command did not reach step admission");
    step::admit_operator(*admitted, net, permissions, operator_hash, settings, journal, clock);
    std::vector<net::Invocation> ready;
    std::vector<net::SessionNotice> notices;
    std::vector<net::SessionOutput> outputs;
    net.sessions.drain(ready, notices, outputs);
    require(ready.empty() && !net.sessions.principal(session),
            "operator command authenticated as a game player");
    for (const auto &output : outputs)
      if (const auto *send = std::get_if<net::Send>(&output); send && send->session == session)
        return send->message;
    throw std::runtime_error("operator command produced no reply");
  };
  auto role = Json{{"key", credential},
                   {"op", "role"},
                   {"player", "p:" + std::to_string(player)},
                   {"role", "architect"},
                   {"on", true}};
  auto forged = role;
  forged["key"] = "not the host credential";
  require(command(forged).at("reason") == "bad_key" && net.profiles.rows[player].roles.empty(),
          "a forged operator credential changed roles");
  const auto model = std::ranges::find_if(catalog.models.rows, [&](const auto &row) {
                       return row.id && row.group == group;
                     })->id;
  step::NativeCall edit;
  edit.request.caller = player;
  edit.arguments = {{"model", "m:" + std::to_string(model)}, {"text", {{"name", "Operator test"}}}};
  auto edit_model = [&] {
    return step::editing::model_text(edit, catalog.model_text, catalog.models,
                                     identity(objects.objects), permissions.players);
  };
  require(!edit_model().reply.at("ok").get<bool>(), "unprivileged newcomer edited a shared model");
  require(command(role).at("ok") && edit_model().changed,
          "operator's architect grant did not permit an actual group model edit");
  const auto holder = perm::Holder::player(player);
  const auto wallet = perm::Account::player(player);
  const auto root = perm::AccountTarget::player(0);
  auto pay = [&](perm::Holder payer, std::int64_t amount) {
    return perm::pay(permissions.ledger, permissions.grants, payer, root, wallet, amount,
                     clock.step);
  };
  require(pay(holder, settings.architect_allowance - 2) &&
              perm::balance(permissions.ledger, wallet) == settings.architect_allowance - 2 &&
              pay(holder, 3).reason == perm::Reason::Allowance,
          "operator promotion must provide a usable capped root allowance");
  const auto body = net.profiles.rows[player].body;
  const auto script = catalog.models.rows[objects.objects.model[body.slot()]].script;
  const auto child = perm::Holder::script(script, body);
  const auto scope = perm::Scope::account(root.account);
  const auto delegated = perm::create_grant(
      permissions.grants, permissions.players, holder,
      {player, child, scope, 1, 2,
       step::duration_steps(settings.architect_period_seconds, clock.hz),
       clock.step + step::duration_steps(settings.architect_expiry_seconds, clock.hz)},
      {scope, 0, 1}, {child, player, true}, clock.step);
  require(delegated && pay(child, 1),
          "the architect's actual body script can spend a bounded child of the allowance");
  require(command(role).at("ok") && pay(holder, 2).reason == perm::Reason::Allowance &&
              perm::balance(permissions.ledger, wallet) == settings.architect_allowance - 1,
          "repeated architect promotion must not reset the allowance already spent");
  step::NativeCall leave;
  leave.request.caller = player;
  leave.arguments = {{"group", "group:" + std::to_string(group)},
                     {"player", "p:" + std::to_string(player)},
                     {"member", false}};
  require(!step::group_member(permissions.groups, permissions.players, leave)
               .reply.at("ok")
               .get<bool>(),
          "a game action removed operator-controlled membership");
  role["on"] = false;
  require(command(role).at("ok") && !edit_model().reply.at("ok").get<bool>() &&
              net.profiles.rows[player].roles.empty(),
          "operator's architect revoke did not remove shared model edit authority");
  require(pay(holder, 1).reason == perm::Reason::MissingRight &&
              pay(child, 1).reason == perm::Reason::MissingRight &&
              perm::balance(permissions.ledger, wallet) == settings.architect_allowance - 1,
          "demotion must revoke the allowance and its child without removing earned money");
  const auto reply =
      command({{"key", credential}, {"op", "key"}, {"player", "p:" + std::to_string(player)}});
  const auto replacement = reply.at("key").get<std::string>();
  require(!net::authenticate(net.profiles, old_key) &&
              net::authenticate(net.profiles, replacement)->id == player &&
              net.sessions.principal(100)->player == player,
          "key replacement did not replace authentication while retaining the live session");
  const auto records = journal.take();
  require(records.size() == 5 && records.front().at("kind") == "denial" &&
              std::ranges::all_of(std::span(records).subspan(1),
                                  [](const auto &row) {
                                    return row.at("kind") == "system" &&
                                           row.at("name") == "operator";
                                  }),
          "operator changes and denial were not recorded");
  const auto recorded = Json(records).dump();
  require(recorded.find(replacement) == std::string::npos &&
              recorded.find(credential) == std::string::npos &&
              recorded.find(old_key) == std::string::npos,
          "operator event record contains a plaintext credential");
}
void check_joining(const Json &plan, catalog::State &catalog, react::State &react,
                   ObjectState &objects, perm::State &permissions, net::State &net,
                   const Settings &settings, const std::filesystem::path &asset_root) {
  const auto model = newcomer_model(plan, catalog, react);
  const step::Clock clock{.step = 7, .hz = 20};
  step::State step;
  step.clock = clock;
  space::State space(settings.space, settings.light_cell_m);
  const auto expected = static_cast<PlayerId>(net.profiles.rows.size());
  auto joined = step::create_join({"  New arrival  "}, net.profiles, permissions, objects, catalog,
                                  plan.at("newcomer"), model, clock);
  require(joined.player == expected && joined.refusal.empty(),
          "join did not allocate the next player");
  const auto &profile = net.profiles.rows.at(joined.player);
  const auto body = profile.body;
  const auto ids = identity(objects.objects);
  require(profile.name == "New arrival" && profile.roles.empty() &&
              profile.connection_modes == net::modes(net::Mode::Play) &&
              net::authenticate(net.profiles, joined.key)->id == joined.player,
          "join did not establish the named, unprivileged player with the returned key");
  require(ids.valid(body) && ids.model[body.slot()] == model && (ids.flags[body.slot()] & Body) &&
              objects.objects.owner[body.slot()] == joined.player &&
              objects.objects.position[body.slot()] == Vec3{},
          "newcomer body is not owned by the new player at Port Zero");
  require(permissions.players.rows[joined.player].groups.empty() &&
              perm::balance(permissions.ledger, perm::Account::player(joined.player)) == 0 &&
              std::ranges::none_of(
                  permissions.plots.rows,
                  [&](const auto &plot) { return plot.id && plot.owner == joined.player; }),
          "join gave an unsolicited group, land or balance");
  const auto script = catalog.models.rows[model].script;
  const auto holder = perm::Holder::script(script, body);
  require(
      bool(perm::check_scope(holder, perm::Scope::avatar(body), 3, permissions.grants, clock.step)),
      "new body cannot animate or see through its avatar");
  for (auto verb : {perm::Verb::Use, perm::Verb::Take, perm::Verb::Move})
    require(bool(perm::check_scope(holder, perm::Scope::owned(joined.player), perm::bit(verb),
                                   permissions.grants, clock.step)),
            "new body cannot act on its owner's goods across plots");
  for (auto grant : permissions.players.rows[joined.player].grants_given)
    require(permissions.grants.rows[grant].issuer == joined.player,
            "newcomer grants were not issued by the newcomer");

  for (auto object : joined.objects) {
    const auto &asset = catalog.assets.rows.at(catalog.models.rows[ids.model[object.slot()]].asset);
    step::import_geometry_asset(step.geometry, space.space, asset, asset_root);
  }
  auto effects =
      step::activate_join(catalog, react, objects, space, net, step, settings, joined.objects);
  const auto motion = space::body_motion(space.space, ids, body);
  require(motion && motion->motion == space::Motion::Character,
          "joining did not install a physical character");
  const auto start = catalog.mechanical.values.at("events").at("start").get<TermId>();
  require(std::ranges::any_of(effects.ordered,
                              [&](const auto &effect) {
                                const auto *event = std::get_if<react::EventWork>(&effect);
                                return event && event->receiver == body && event->event == start;
                              }),
          "joining did not schedule the real body startup event");

  const auto profiles_before = net.profiles.rows.size(),
             players_before = permissions.players.rows.size();
  const auto objects_before = objects.objects.model.size(),
             grants_before = permissions.grants.rows.size();
  for (const auto &name : {std::string(" new ARRIVAL "), std::string(""), std::string(33, 'x')}) {
    const auto refused = step::create_join({name}, net.profiles, permissions, objects, catalog,
                                           plan.at("newcomer"), model, clock);
    require(!refused.player &&
                refused.refusal == (name == " new ARRIVAL " ? "name_taken" : "bad_input"),
            "join did not reject a duplicate or invalid name");
  }
  require(net.profiles.rows.size() == profiles_before &&
              permissions.players.rows.size() == players_before &&
              objects.objects.model.size() == objects_before &&
              permissions.grants.rows.size() == grants_before,
          "a refused join partially created a player, body or grants");
  std::string unicode_name;
  for (int i = 0; i < 32; ++i)
    unicode_name += "界";
  const auto second = step::create_join({unicode_name}, net.profiles, permissions, objects, catalog,
                                        plan.at("newcomer"), model, clock);
  require(second.player == joined.player + 1,
          "second join did not preserve Unicode or allocate the next id");
  step::activate_join(catalog, react, objects, space, net, step, settings, second.objects);

  net.sessions.accept(net::Connected{100});
  const auto connected =
      net.sessions.establish(100, net.profiles.rows[joined.player], net::Mode::Play, false);
  const auto &appearance = catalog.mechanical.values.at("appearance");
  step::connection_notice(
      connected, net, space, step.delivery, catalog, react,
      {appearance.at("red").get<TermId>(), appearance.at("green").get<TermId>(),
       appearance.at("blue").get<TermId>(), appearance.at("alpha").get<TermId>(),
       appearance.at("metallic").get<TermId>(), appearance.at("roughness").get<TermId>()},
      objects, clock, settings);
  require(step.delivery.present_bodies.at(joined.player) == body &&
              net.session_views.rows.at(100).body == body,
          "joined connection did not become an ordinary player view");
  check_operator(net, permissions, catalog, objects, settings, joined.player, joined.key);

  step::NativeCall clear_script;
  clear_script.request.caller = catalog.models.rows[model].author;
  clear_script.arguments = {{"model", "m:" + std::to_string(model)}, {"script", nullptr}};
  clear_script.step = clock.step;
  clear_script.hz = clock.hz;
  require(step::editing::model_script(clear_script, react,
                                      react::value_read(objects, catalog, react), catalog,
                                      permissions, start)
              .changed,
          "the normal model edit must allow removing the newcomer body's script");
  const auto configured_model = newcomer_model(plan, catalog, react);
  require(configured_model == model,
          "startup must still resolve an edited body model when joining is unavailable");
  const auto profiles_without_script = net.profiles.rows.size(),
             players_without_script = permissions.players.rows.size();
  const auto objects_without_script = objects.objects.model.size(),
             grants_without_script = permissions.grants.rows.size();
  const auto unavailable = step::create_join({"Later arrival"}, net.profiles, permissions, objects,
                                             catalog, plan.at("newcomer"), configured_model, clock);
  require(!unavailable.player && unavailable.refusal == "refused" &&
              net.profiles.rows.size() == profiles_without_script &&
              permissions.players.rows.size() == players_without_script &&
              objects.objects.model.size() == objects_without_script &&
              permissions.grants.rows.size() == grants_without_script,
          "a join with an unavailable body script must refuse before creating anything");
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc != 4)
      throw std::invalid_argument("start_test PLAN.json ASSET_ROOT NATIVE_REGISTRY.json");
    const auto started = std::chrono::steady_clock::now();
    auto plan = read_plan(argv[1]);
    const auto registry_path = std::filesystem::path(argv[3]);
    const auto native_registry =
        catalog::read_registry(registry_path.parent_path(), registry_path.stem().string());
    plan["registry"] = native_registry.data;
    const auto directory = std::filesystem::current_path() / ".cache/start-test";
    std::filesystem::remove_all(directory);
    amber::react::State react_state{{4096, 50, 5, 64}};
    catalog::State catalog_state;
    perm::State perm_state;
    react::Terms &terms = react_state.terms;
    react::Materials &materials = react_state.materials;
    catalog::MechanicalBindings mechanical;
    const auto registry = load_registry(plan, directory / "registry", terms, materials, mechanical);
    catalog_state.mechanical = mechanical;
    step::editing::declare_editing(terms, react_state.native_declarations);
    std::ifstream metadata(std::filesystem::path(argv[2]) /
                           plan.at("models").front().at("asset").get<std::string>() /
                           "manifest.json");
    const catalog::RuntimePins pins{Json::parse(metadata).at("bake_version")};
    const react::VmSettings vm_settings{4096, 50, 5, 64};
    auto prepared = prepare_scripts(plan, terms, registry.hash, pins, vm_settings);
    {
      react::Vm vm(vm_settings);
      catalog::Scripts scripts;
      Symbols symbols;
      install_scripts(prepared, scripts, vm, symbols, {});
      for (const auto &script : scripts.rows)
        if (script.declaration.kind == react::ScriptDeclaration::Kind::Rule)
          require(!script.declaration.approved && !script.declaration.active,
                  "source cannot approve an initial rule");
    }
    Symbols symbols;
    install_catalog(plan, react_state, registry.hash, pins, vm_settings, argv[2],
                    directory / "scripts", catalog_state, symbols);
    catalog::Assets &assets = catalog_state.assets;
    auto malformed = plan;
    malformed["models"][0]["canonical_params"] =
        malformed["models"][0]["canonical_params"].get<std::string>() + " ";
    rejects([&] { (void)prepare_assets(malformed, argv[2], pins, terms); },
            "asset canonical byte mismatch must fail");
    catalog::Models &models = catalog_state.models;
    const auto land_script = symbols.scripts.at("land");
    const auto *world_land = react::world_script(react_state.world_scripts, land_script);
    require(world_land && world_land->space == "land",
            "startup installs land on the world with a discoverable action space");

    amber::ObjectState objects_state;
    Objects &objects = objects_state.objects;

    const auto created = install_objects(plan, symbols, catalog_state, terms, objects_state, 0);
    std::size_t expected_children = 0;
    for (const auto &row : plan.at("objects"))
      expected_children +=
          models.rows[symbols.models.at(row.at("model").get<std::string>())].parts.size();
    require(created.size() == plan.at("objects").size() + expected_children,
            "ordinary declared child objects must actually exist");
    install_properties(plan, symbols, react_state, 0, 20);
    react::AuthoredRelations &relations = react_state.authored_relations;
    react::DeferredEffects initial;
    install_relations(plan, symbols, terms, relations, initial);
    react::RelationsRead &relation_read = react_state.relations;
    for (auto &[id, relation] : relations)
      relation_read.columns[id] = &relation;
    react::ScriptStates &states = react_state.states;
    install_script_state(plan, symbols, states);
    perm::Plots &plots = perm_state.plots;
    perm::Groups &groups = perm_state.groups;
    perm::Players &players = perm_state.players;
    net::State net_state{test_net::options()};
    auto &profiles = net_state.profiles;
    auto profile_plan = plan;
    for (auto &profile : profile_plan["profiles"])
      profile["key_file"] = "keys/" + std::to_string(profile.at("id").get<PlayerId>()) + ".key";
    install_profiles(profile_plan, symbols, profiles, players, directory);
    struct stat info{};
    require(::stat((directory / "keys/2.key").c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
            "generated credentials must remain private");
    auto settings = amber::read_settings(AMBER_CONFIG_PATH);
    settings.land.way_group = plan.at("land").at("way_group").get<GroupId>();
    space::State land_space(settings.space, settings.light_cell_m);
    auto player_controlled_ways = plan;
    for (auto &group : player_controlled_ways["groups"])
      if (group.at("id") == settings.land.way_group)
        group["controller"] = 1;
    perm::State invalid_land;
    rejects([&] { install_land(player_controlled_ways, invalid_land, land_space, settings.land); },
            "the architects' way group must be controlled by root");
    install_land(plan, perm_state, land_space, settings.land);
    perm::update_plots(objects, plots, created, true);
    Json owned_lots = Json::array();
    for (const auto &lot : plan.at("land").at("lots"))
      if (lot.at("owner") != 0)
        owned_lots.push_back(lot);
    require(owned_lots == Json::array({{{"owner", 2}, {"at", {8, 50}}},
                                       {{"owner", 2}, {"at", {24, 50}}},
                                       {{"owner", 3}, {"at", {40, 50}}},
                                       {{"owner", 4}, {"at", {56, 50}}},
                                       {{"owner", 5}, {"at", {-16, 50}}},
                                       {{"owner", 6}, {"at", {-32, 50}}},
                                       {{"owner", 7}, {"at", {-48, 50}}}}),
            "seeded residents own the specified lots, including both cafe lots");
    for (const auto &lot : plan.at("land").value("lots", Json::array())) {
      const auto at = lot.at("at").get<std::array<float, 2>>();
      const auto found = std::ranges::find_if(plots.rows, [&](const auto &plot) {
        return plot.id && plot.bounds[0] == at[0] && plot.bounds[1] == at[1];
      });
      require(found != plots.rows.end() && found->owner == lot.at("owner").get<PlayerId>(),
              "each initial lot has its authored owner");
      if (lot.contains("preset"))
        require(found->preset == step::rights(lot.at("preset"), perm::ScopeKind::Plot),
                "initial lot presets use the ordinary land rights");
    }
    perm::Ledger &ledger = perm_state.ledger;
    auto money_plan = plan;
    groups.rows.push_back({2, 1, "Funded institution"});
    money_plan["balances"].push_back({{"holder", "group:2"}, {"amount", 25}});
    install_money(money_plan, symbols, identity(objects), authority(std::as_const(objects)),
                  players, groups, ledger);
    require(perm::balance(ledger, perm::Account::player(2)) == 1000 &&
                perm::balance(ledger, perm::Account::group(2)) == 25 &&
                perm::balance(ledger, perm::Account::player(1)) == 0,
            "startup aliased an object or group account to its controller's player wallet");
    react::initialize_reactions(catalog_state, react_state,
                                react::value_read(objects_state, catalog_state, react_state),
                                created, 0, 20, initial);
    perm::Grants &grants = perm_state.grants;
    install_grants(plan, symbols, objects_state, perm_state, catalog_state, react_state,
                   amber::step::Clock{.step = 0, .hz = 20});
    require(grants.rows.size() == plan.at("grants").size() + 1,
            "every initial grant passed real permission creation");
    auto missing_body_choices = plan;
    for (auto &profile : missing_body_choices["profiles"])
      if (profile.contains("body")) {
        profile.erase("body_installs");
        break;
      }
    perm::State missing_permissions;
    missing_permissions.players = players;
    missing_permissions.plots = plots;
    missing_permissions.groups = groups;
    rejects(
        [&] {
          provision_body_grants(missing_body_choices, symbols, catalog_state, objects_state,
                                missing_permissions, profiles, 0, 20);
        },
        "a starting body with unfilled declared slots silently started");
    provision_body_grants(plan, symbols, catalog_state, objects_state, perm_state, profiles, 0, 20);
    for (const auto &profile : plan["profiles"])
      if (profile.contains("body")) {
        const auto player = profile.at("id").get<PlayerId>();
        const auto body = symbols.objects.at(profile.at("body").get<std::string>());
        const auto script = models.rows[objects.model[body.slot()]].script;
        const auto holder = perm::Holder::script(script, body);
        require(bool(perm::check_scope(holder, perm::Scope::avatar(body), 3, grants, 0)),
                "starting body cannot use its authored avatar grants");
        for (const auto verb : {perm::Verb::Use, perm::Verb::Take, perm::Verb::Move})
          require(bool(perm::check_scope(holder, perm::Scope::owned(player), perm::bit(verb),
                                         grants, 0)),
                  "starting body can use its owner's goods on any plot");
      }
    require(!players.rows[0].grants_given.empty() &&
                !ledger.balance.contains(perm::Account::player(0)),
            "start file loads root grants without storing a root balance");
    const auto script_grant =
        *std::find_if(plan.at("grants").begin(), plan.at("grants").end(), [](const auto &row) {
          return row.at("holder").is_object() && row.at("holder").at("on") == "resident.mira";
        });
    auto wrong_owner = plan;
    wrong_owner["grants"] = Json::array({script_grant});
    wrong_owner["grants"][0]["issuer"] = 3;
    wrong_owner["grants"][0]["controller"] = 3;
    perm::State rejected_permissions;
    rejected_permissions.players = players;
    rejected_permissions.plots = plots;
    rejected_permissions.groups = groups;
    rejects(
        [&] {
          install_grants(wrong_owner, symbols, objects_state, rejected_permissions, catalog_state,
                         react_state, amber::step::Clock{.step = 0, .hz = 20});
        },
        "plan cannot forge current goods control");
    auto wrong_installation = plan;
    wrong_installation["grants"] = Json::array({script_grant});
    wrong_installation["grants"][0]["holder"]["script"] = "lamp";
    rejects(
        [&] {
          install_grants(wrong_installation, symbols, objects_state, rejected_permissions,
                         catalog_state, react_state, amber::step::Clock{.step = 0, .hz = 20});
        },
        "plan cannot claim an uninstalled script is installed");
    step::WorkQueues queues;
    queue_bootstrap(plan, symbols, terms, queues, 1000000);
    require(queues.causal.size() == plan.at("bootstrap").size(),
            "authored bootstrap events queued");
    check_joining(plan, catalog_state, react_state, objects_state, perm_state, net_state, settings,
                  argv[2]);
    const auto elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    std::cout << Json{{"objects", created.size()},        {"fixture_bake_version", pins.bake},
                      {"assets", assets.rows.size()},     {"scripts", symbols.scripts.size()},
                      {"grants", grants.rows.size() - 1}, {"import_ms", elapsed}}
                     .dump()
              << '\n';
    std::filesystem::remove_all(directory);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
