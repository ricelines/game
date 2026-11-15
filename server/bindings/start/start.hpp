#pragma once

#include "control/control.hpp"
#include "install/install.hpp"
#include "lifecycle/lifecycle.hpp"
#include "perm/perm.hpp"

namespace amber::bindings::start {
using Json = nlohmann::json;
struct Symbols {
  std::unordered_map<std::string, ModelId> models;
  std::unordered_map<std::string, ScriptId> scripts;
  std::unordered_map<std::string, Handle> objects;
};
struct SeedRuleApproval {
  std::string key, evidence;
};
struct PreparedSeedScript {
  std::string key;
  PlayerId author = 0;
  bool requested_rule = false;
  catalog::PreparedScript artifact;
};

Json read_plan(const std::filesystem::path &);
catalog::RegistryDocument load_registry(const Json &, const std::filesystem::path &registry_root,
                                        react::Terms &, react::Materials &,
                                        catalog::MechanicalBindings &);
std::vector<PreparedSeedScript> prepare_scripts(const Json &, const react::Terms &,
                                                const catalog::Hash &registry,
                                                const catalog::RuntimePins &, react::VmSettings);
void install_scripts(std::vector<PreparedSeedScript>, catalog::Scripts &, react::Vm &, Symbols &,
                     std::span<const SeedRuleApproval>);
std::vector<catalog::PreparedAsset> prepare_assets(const Json &, const std::filesystem::path &,
                                                   const catalog::RuntimePins &,
                                                   const react::Terms &);
void import_assets(std::vector<catalog::PreparedAsset>, catalog::Assets &);
void install_models(const Json &, const catalog::Assets &, const catalog::Scripts &,
                    const react::Terms &, catalog::Models &, catalog::ModelTexts &,
                    react::ModelSettings &, Symbols &);
std::vector<Handle> install_objects(const Json &, Symbols &, const catalog::Models &,
                                    const catalog::ParameterBlocks &, const catalog::Assets &,
                                    const react::Terms &, lifecycle::Instances &, Step);
void install_properties(const Json &, const Symbols &, const react::Terms &, react::Properties &,
                        react::Processes &, react::InstanceSettings &, react::WorldSettings &,
                        react::PlotSettings &, Step, double hz);
void install_relations(const Json &, const Symbols &, const react::Terms &,
                       react::AuthoredRelations &, react::DeferredEffects &);
void install_profiles(const Json &, const Symbols &, control::Profiles &, perm::Players &,
                      const std::filesystem::path &key_root);
void install_land(const Json &, perm::Plots &, perm::Groups &, perm::Players &);
void install_money(const Json &, const Symbols &, IdentityRead, AuthorityRead,
                   const perm::Players &, const perm::Groups &, perm::Ledger &);
void install_grants(const Json &, const Symbols &, const react::ValueRead &, AuthorityRead,
                    const perm::OwnerLimits &, const catalog::Models &, const catalog::Scripts &,
                    const install::InstanceScripts &, const react::RuleVersions &,
                    const perm::Plots &, const control::Profiles &, perm::Grants &, perm::Players &,
                    Step, double hz, const perm::Groups &);
void install_script_state(const Json &, const Symbols &, react::ScriptStates &);
void queue_bootstrap(const Json &, const Symbols &, const react::Terms &, step::WorkQueues &,
                     std::size_t event_limit);
} // namespace amber::bindings::start
