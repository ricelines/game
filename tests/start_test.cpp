#include "bindings/start/start.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>

using namespace amber;
using namespace amber::bindings::start;
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
    if (argc != 3)
      throw std::invalid_argument("start_test PLAN.json ASSET_ROOT");
    const auto started = std::chrono::steady_clock::now();
    const auto plan = read_plan(argv[1]);
    const auto directory = std::filesystem::current_path() / ".cache/start-test";
    std::filesystem::remove_all(directory);
    react::Terms terms;
    react::Materials materials;
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
    react::Vm vm(vm_settings);
    catalog::Scripts scripts;
    Symbols symbols;
    install_scripts(std::move(prepared), scripts, vm, symbols, fixture_approvals);
    catalog::Assets assets;
    import_assets(prepare_assets(plan, argv[2], pins, terms), assets);
    auto malformed = plan;
    malformed["models"][0]["canonical_params"] =
        malformed["models"][0]["canonical_params"].get<std::string>() + " ";
    rejects([&] { (void)prepare_assets(malformed, argv[2], pins, terms); },
            "asset canonical byte mismatch must fail");
    catalog::Models models;
    catalog::ModelTexts model_text;
    react::ModelSettings model_settings;
    install_models(plan, assets, scripts, terms, models, model_text, model_settings, symbols);
    catalog::ParameterBlocks parameters;
    Objects objects;
    lifecycle::Parts parts;
    Relation declared_parts;
    lifecycle::Instances instances(objects, parts, declared_parts);
    const auto created =
        install_objects(plan, symbols, models, parameters, assets, terms, instances, 0);
    std::size_t expected_children = 0;
    for (const auto &row : plan.at("objects"))
      expected_children +=
          models.rows[symbols.models.at(row.at("model").get<std::string>())].parts.size();
    require(created.size() == plan.at("objects").size() + expected_children,
            "ordinary declared child objects must actually exist");
    react::Properties properties;
    react::Processes processes;
    react::InstanceSettings instance_settings;
    react::WorldSettings world_settings;
    react::PlotSettings plot_settings;
    install_properties(plan, symbols, terms, properties, processes, instance_settings,
                       world_settings, plot_settings, 0, 20);
    react::AuthoredRelations relations;
    react::DeferredEffects initial;
    install_relations(plan, symbols, terms, relations, initial);
    react::RelationsRead relation_read;
    for (auto &[id, relation] : relations)
      relation_read.columns[id] = &relation;
    react::ScriptStates states;
    install_script_state(plan, symbols, states);
    perm::Plots plots;
    perm::Groups groups;
    perm::Players players;
    control::Profiles profiles;
    auto profile_plan = plan;
    for (auto &profile : profile_plan["profiles"])
      profile["key_file"] = "keys/" + std::to_string(profile.at("id").get<PlayerId>()) + ".key";
    install_profiles(profile_plan, symbols, profiles, players, directory);
    struct stat info{};
    require(::stat((directory / "keys/2.key").c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
            "generated credentials must remain private");
    install_land(plan, plots, groups, players);
    perm::Ledger ledger;
    install_money(plan, players, ledger);
    require(perm::balance(ledger, 2) == 1000,
            "actual player account receives its authored initial balance");
    react::RuleVersions rules;
    for (const auto &script : scripts.rows)
      if (script.id && script.declaration.kind == react::ScriptDeclaration::Kind::Rule)
        install::add_rule(rules, scripts, terms, script.id);
    react::AssetMeasurements measurements;
    react::SharedColumns shared;
    react::RuntimeMeasurements runtime;
    const react::ValueRead values{terms,
                                  identity(objects),
                                  objects.plot,
                                  objects.params,
                                  materials,
                                  measurements,
                                  properties,
                                  processes,
                                  instance_settings,
                                  model_settings,
                                  plot_settings,
                                  world_settings,
                                  shared,
                                  relation_read,
                                  runtime,
                                  0};
    const install::InstanceScripts installations;
    const perm::OwnerLimits limits;
    perm::Grants grants;
    install_grants(plan, symbols, values, authority(std::as_const(objects)), limits, models,
                   scripts, installations, rules, plots, profiles, grants, players, 0, 20);
    require(grants.rows.size() == plan.at("grants").size() + 1,
            "every initial grant passed real permission creation");
    auto wrong_owner = plan;
    wrong_owner["grants"] = Json::array({plan.at("grants")[0]});
    wrong_owner["grants"][0]["issuer"] = 3;
    wrong_owner["grants"][0]["controller"] = 3;
    perm::Grants rejected_grants;
    rejects(
        [&] {
          install_grants(wrong_owner, symbols, values, authority(std::as_const(objects)), limits,
                         models, scripts, installations, rules, plots, profiles, rejected_grants,
                         players, 0, 20);
        },
        "plan cannot forge current goods control");
    auto wrong_installation = plan;
    wrong_installation["grants"] = Json::array({plan.at("grants")[0]});
    wrong_installation["grants"][0]["holder"]["script"] = "body";
    rejects(
        [&] {
          install_grants(wrong_installation, symbols, values, authority(std::as_const(objects)),
                         limits, models, scripts, installations, rules, plots, profiles,
                         rejected_grants, players, 0, 20);
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
