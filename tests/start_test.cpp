#include "catalog/catalog.hpp"
#include "settings.hpp"
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
    std::vector<SeedRuleApproval> fixture_approvals;
    for (const auto &row : plan.at("scripts"))
      if (row.at("world_rule").get<bool>())
        fixture_approvals.push_back(
            {row.at("key"), "isolated importer test authority; not a gameplay approval"});
    auto &vm = react_state.vm;
    catalog::Scripts &scripts = catalog_state.scripts;
    Symbols symbols;
    install_scripts(std::move(prepared), scripts, vm, symbols, fixture_approvals);
    catalog::Assets &assets = catalog_state.assets;
    import_assets(prepare_assets(plan, argv[2], pins, terms), assets);
    auto malformed = plan;
    malformed["models"][0]["canonical_params"] =
        malformed["models"][0]["canonical_params"].get<std::string>() + " ";
    rejects([&] { (void)prepare_assets(malformed, argv[2], pins, terms); },
            "asset canonical byte mismatch must fail");
    catalog::Models &models = catalog_state.models;
    catalog::ModelTexts &model_text = catalog_state.model_text;
    react::ModelSettings &model_settings = react_state.model_settings;
    install_models(plan, assets, scripts, terms, models, model_text, model_settings, symbols);

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
    net::Profiles profiles;
    auto profile_plan = plan;
    for (auto &profile : profile_plan["profiles"])
      profile["key_file"] = "keys/" + std::to_string(profile.at("id").get<PlayerId>()) + ".key";
    install_profiles(profile_plan, symbols, profiles, players, directory);
    struct stat info{};
    require(::stat((directory / "keys/2.key").c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
            "generated credentials must remain private");
    const auto settings = amber::read_settings(AMBER_CONFIG_PATH);
    space::State land_space(settings.space, settings.light_cell_m);
    install_land(plan, perm_state, land_space, settings.land);
    perm::update_plots(identity(objects), transforms(objects), plot_membership(objects), plots,
                       created, true);
    for (const auto &lot : plan.at("land").value("lots", Json::array())) {
      const auto at = lot.at("at").get<std::array<float, 2>>();
      const auto found = std::ranges::find_if(plots.rows, [&](const auto &plot) {
        return plot.id && plot.bounds[0] == at[0] && plot.bounds[1] == at[1];
      });
      require(found != plots.rows.end() && found->owner == lot.at("owner").get<PlayerId>(),
              "each seeded resident owns the lot named by its minimum corner");
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
    react::RuleVersions &rules = react_state.rules;
    for (const auto &script : scripts.rows)
      if (script.id && script.declaration.kind == react::ScriptDeclaration::Kind::Rule)
        react::add_rule(rules, scripts, terms, script.id);
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
          return row.at("holder").is_object() && row.at("holder").at("script") == "power_buyer";
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
    wrong_installation["grants"][0]["holder"]["script"] = "body";
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
