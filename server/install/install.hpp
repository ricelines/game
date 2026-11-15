#pragma once
#include "catalog/catalog.hpp"
#include "snap/codec.hpp"

namespace amber::install {
using InstanceScripts = SparseColumn<std::vector<ScriptId>>;
struct RuleKey {
  TermId trigger = 0, selector = 0;
  bool flag = false;
  auto operator<=>(const RuleKey &) const = default;
};
struct RuleChange {
  std::vector<react::RuleVersion> active, retired;
};
void set_instances(InstanceScripts &, Handle, std::span<const ScriptId>, const catalog::Scripts &);
std::vector<ScriptId> installed_scripts(const catalog::Models &, const InstanceScripts &,
                                        IdentityRead, Handle);
void project_model(react::Triggers &, const catalog::Models &, const catalog::Scripts &,
                   const react::Terms &, ModelId);
void project_instance(react::Triggers &, const InstanceScripts &, const catalog::Scripts &,
                      const react::Terms &, Handle);
RuleChange add_rule(react::RuleVersions &, const catalog::Scripts &, const react::Terms &,
                    ScriptId);
RuleChange set_rule_enabled(react::RuleVersions &, RuleKey, bool);
std::vector<react::RuleVersion> latest_rules(const react::RuleVersions &);
void project_rules(react::Triggers &, const react::RuleVersions &);
std::vector<ScriptId> rule_scripts(const react::RuleVersions &, const react::ValueRead &, Handle,
                                   Step, double hz);
std::vector<TermId> rule_watch_terms(const react::RuleVersions &, const react::ValueRead &, Handle,
                                     ScriptId, Step, double hz);

struct WatchKey {
  ScriptKey consumer;
  std::uint32_t index = 0;
  bool operator==(const WatchKey &) const = default;
};
struct WatchKeyHash {
  std::size_t operator()(const WatchKey &) const;
};
struct WatchLink {
  FactKey source;
  std::vector<FactKey> dependencies;
  react::WatchDeclaration declaration;
};
struct WatchLinks {
  std::unordered_map<WatchKey, WatchLink, WatchKeyHash> rows;
  std::unordered_map<FactKey, std::vector<WatchKey>, FactKeyHash> by_dependency;
  std::unordered_map<ScriptKey, std::vector<WatchKey>, ScriptKeyHash> by_consumer;
  std::unordered_map<ScriptId, std::vector<ScriptKey>> by_script;
};
enum class SyncMode { Initial, Change, Restore };
struct WatchChanges {
  std::vector<step::WatchWork> dirty;
  std::vector<step::Timer> timers;
};
struct WatchResult {
  FactKey source;
  std::vector<react::WatchDelivery> deliveries;
  std::vector<step::Timer> timers;
};
WatchChanges sync_watches(react::Subscriptions &, WatchLinks &, const react::ValueRead &,
                          const react::AllocationRows &, ScriptKey,
                          const react::ScriptDeclaration &, Step, double hz, const record::Origin &,
                          SyncMode, std::optional<std::span<const TermId>> allowed_terms = {});
void remove_watches(react::Subscriptions &, WatchLinks &, ScriptKey);
WatchChanges refresh_dependencies(react::Subscriptions &, WatchLinks &, const react::ValueRead &,
                                  FactKey changed, Step, double hz, const record::Origin &);
WatchChanges sync_rule_watches(react::Subscriptions &, WatchLinks &, const react::ValueRead &,
                               const react::AllocationRows &, const catalog::Scripts &,
                               const react::RuleVersions &, std::span<const react::Effect>, Step,
                               double hz, const record::Origin &);
std::vector<Handle> source_consumers(const react::ValueRead &, FactKey,
                                     const react::WatchDeclaration &);
WatchResult check_changed(react::Subscriptions &, const react::ValueRead &,
                          const react::AllocationRows &, FactKey, Step, double hz,
                          const record::Origin &, ScriptId recipient = 0);
struct InvocationPlan {
  react::HandlerRef handler;
  Handle receiver, sender;
  TermId trigger = 0;
  EventPayload payload;
  std::vector<Handle> acquired;
};
std::vector<InvocationPlan> plan_deliveries(const WatchResult &, const react::Triggers &,
                                            const catalog::Scripts &, const react::ValueRead &,
                                            Step, double hz);
bool has_applicable_watch(const react::Subscriptions &, const react::Triggers &,
                          const catalog::Scripts &, const react::ValueRead &, FactKey, Step,
                          double hz);
std::vector<step::BackgroundItem> catch_up(const react::SharedColumns &, IdentityRead,
                                           const catalog::Scripts &, const react::Terms &,
                                           const react::RuleVersion &, const record::Origin &);
bool catch_up_valid(const step::BackgroundItem &, const react::RuleVersions &,
                    const react::ValueRead &, Step, double hz);
void cancel_catch_up(step::WorkQueues &, const react::RuleVersion &);
using ObjectSelection = std::optional<std::span<const Handle>>;
void write_instances(snap::Output &, const InstanceScripts &, IdentityRead, ObjectSelection = {});
void read_instances(snap::Input &, InstanceScripts &, std::size_t slots);
} // namespace amber::install
