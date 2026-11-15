#pragma once

#include "record/record.hpp"
#include "snap/snap.hpp"
#include "store/store.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <unordered_map>

struct lua_State;

namespace amber::catalog {
struct Models;
struct Scripts;
} // namespace amber::catalog

namespace amber::react {

Answer read_intrinsic(const Materials &, TermId material, const Term &);
Answer read_baked(const AssetMeasurements &, ModelId, const Term &, std::uint32_t params = 0);
double read_mass(double volume, double density);
double process_value(const Process &, Step now, double hz);
Answer read_state(const Properties &, const Processes &, Handle, TermId, Step, double hz);
Answer read_setting(const InstanceSettings &, const ModelSettings &, const PlotSettings &,
                    const WorldSettings &, Handle, ModelId, PlotId, TermId);
Answer read_shared(const SharedColumns &, const RelationsRead &, Handle, const Term &);
Answer read_runtime_measurement(const RuntimeMeasurements &, FactKey);
Answer read(const ValueRead &, Handle, TermId, Step, double hz);

std::vector<HandlerRef> dispatch(const Triggers &, IdentityRead, std::span<const TermId> flags,
                                 Handle receiver, TermId trigger);
std::vector<HandlerRef> dispatch(const Triggers &, const ValueRead &, Handle receiver,
                                 TermId trigger, Step, double hz);

std::optional<std::pair<HandlerRef, std::size_t>> find_handler(const Triggers &, const ValueRead &,
                                                               ScriptKey, TermId, Step, double hz);
void acknowledge_watch(Subscriptions &, ScriptKey, const WatchRun &);

RelationAnswer read_relation(const Relation &, Handle);

std::vector<WatchDelivery> check_watch(Subscriptions &, FactKey, const CurrentAnswers &,
                                       ScriptId recipient = 0);
std::optional<Step> next_crossing(const Process &, const Threshold &, Step now, double hz);
bool valid_threshold(const step::ThresholdToken &, IdentityRead, const Processes &,
                     const Subscriptions &, Step now, double hz);
void bind_watch(Subscriptions &, FactKey, WatchBinding);
void unbind_watch(Subscriptions &, ScriptKey, std::uint32_t index);
void retire_subscriptions(Subscriptions &, Handle);
std::vector<FactKey> watch_sources(const Subscriptions &, ScriptKey);
std::vector<FactKey> watch_sources(const Subscriptions &, Handle subject);
std::vector<ScriptKey> watch_consumers(const Subscriptions &, Handle object);
std::vector<step::Timer> threshold_timers(const Subscriptions &, const Processes &, FactKey,
                                          Step now, double hz, const record::Origin &);

void validate_shared_terms(const Terms &);
std::vector<FactKey> allocation_inputs(const Terms &, FactKey);
std::vector<FactKey> priority_inputs(const ValueRead &, SettingKey);
bool reconcile_allocation(const ValueRead &, AllocationRows &, MeterRows &, FactKey demand, Step,
                          double hz);

double metered(const Meter &, Step now, double hz);
std::vector<AllocationKey> allocate(const Shared &supply, const Shared &demand, Combine,
                                    const PriorityValues &, Handle subject, TermId supply_term,
                                    TermId demand_term, AllocationRows &, MeterRows &, Step now,
                                    double hz);
double get_share(const AllocationRows &, Handle subject, TermId demand_term, Handle contributor);
double live_share(const ValueRead &, Handle subject, TermId demand_term, Handle contributor,
                  Step now, double hz);
void retire_allocations(AllocationRows &, MeterRows &, Handle);

bool set_state(Properties &, Processes &, const Term &, Handle, Value, Step, double hz,
               DeferredEffects &);
bool set_rate(Properties &, Processes &, const Term &, Handle, double rate, double lo, double hi,
              Step, double hz, DeferredEffects &);
bool transfer_state(Properties &, Processes &, const Term &, Handle from, Handle to, double amount,
                    Step, double hz, DeferredEffects &);
bool set_contribution(SharedColumns &, GivesTo &, const Term &, Handle from, Handle subject,
                      double amount, DeferredEffects &);
void withdraw_contributor(SharedColumns &, GivesTo &, const Terms &, Handle, DeferredEffects &);
bool set_relation(Relation &, TermId, Handle from, Handle to, bool present, DeferredEffects &);
bool apply_derived_fact(RuntimeMeasurements &, const DerivedFactInput &, DeferredEffects &);
void retire_object(Properties &, Processes &, ScriptStates &, RandomStates &, Handle,
                   std::span<const ScriptId> installed_scripts);

ScriptValidation validate_script(std::string_view, const Terms &, const ScriptDeclaration &,
                                 const RightsDeclaration &);
CompiledScript compile_script(std::string_view);

void consult_object(const Invocation &, Handle);

class Vm;
// Direct VM execution.
bool start_invocation(Vm &, ScriptStates &, RandomStates &, Subscriptions &, const ValueRead &,
                      const Triggers &, const Relation &declared_parts, std::uint64_t initial_seed,
                      AttributionRead, const catalog::Models &, const catalog::Scripts &,
                      record::Journal &, const HandlerRef &, InvocationInput, Invocation &,
                      CompletedInvocation &);
void finish_invocation(IdentityRead, AttributionRead, const catalog::Models &,
                       const catalog::Scripts &, record::Journal &, Invocation &,
                       CompletedInvocation &);
void refresh_parts(Invocation &, IdentityRead, const Relation &declared_parts);

class Vm {
public:
  explicit Vm(VmSettings);
  ~Vm();
  Vm(const Vm &) = delete;
  Vm &operator=(const Vm &) = delete;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  friend void register_binding(Vm &, std::string_view, int (*)(lua_State *), std::shared_ptr<void>);
  friend void bind_values(Vm &, const ValueRead &, Properties &, Processes &, SharedColumns &,
                          GivesTo &);
  friend RunOutcome load_script(Vm &, const CompiledScript &, const ScriptDeclaration &);
  friend RunOutcome run_handler(Vm &, const HandlerRef &, Invocation &);
  friend RunOutcome resume_handler(Vm &, Invocation &, const nlohmann::json &);
  friend bool set_activation(Vm &, ScriptId, bool, bool);
  friend ScriptAnalysis analyze_script(std::string_view, const Terms &, const RightsDeclaration &,
                                       VmSettings);
};
void register_binding(Vm &, std::string_view, int (*)(lua_State *),
                      std::shared_ptr<void> binding_local);
void bind_events(Vm &, const Terms &);
ScriptAnalysis analyze_script(std::string_view, const Terms &, const RightsDeclaration &,
                              VmSettings);
RunOutcome load_script(Vm &, const CompiledScript &, const ScriptDeclaration &);
RunOutcome run_handler(Vm &, const HandlerRef &, Invocation &);
// An act call suspends only the currently running handler. The step executes
// its checked action and immediately resumes this same call with the result.
RunOutcome resume_handler(Vm &, Invocation &, const nlohmann::json &);
bool set_activation(Vm &, ScriptId, bool approved, bool active);
Invocation &invocation(lua_State *);
void *binding_data(lua_State *);
void push_handle(lua_State *, Handle);
Handle check_handle(lua_State *, int index);
EventPayload check_event_payload(lua_State *, int index);
// Core value methods keep their required C-callback data private to the VM.
void bind_values(Vm &, const ValueRead &, Properties &, Processes &, SharedColumns &, GivesTo &);

void set_instances(InstanceScripts &, Handle, std::span<const ScriptId>, const catalog::Scripts &);
std::vector<ScriptId> installed_scripts(const catalog::Models &, const InstanceScripts &,
                                        IdentityRead, Handle);
void project_model(Triggers &, const catalog::Models &, const catalog::Scripts &, const Terms &,
                   ModelId);
void project_instance(Triggers &, const InstanceScripts &, const catalog::Scripts &, const Terms &,
                      Handle);
RuleChange add_rule(RuleVersions &, const catalog::Scripts &, const Terms &, ScriptId);
RuleChange set_rule_enabled(RuleVersions &, RuleKey, bool);
std::vector<RuleVersion> latest_rules(const RuleVersions &);
void project_rules(Triggers &, const RuleVersions &);
std::vector<ScriptId> rule_scripts(const RuleVersions &, const ValueRead &, Handle, Step,
                                   double hz);
std::vector<TermId> rule_watch_terms(const RuleVersions &, const ValueRead &, Handle, ScriptId,
                                     Step, double hz);

WatchChanges sync_watches(Subscriptions &, const ValueRead &, const AllocationRows &, ScriptKey,
                          const ScriptDeclaration &, Step, double hz, const record::Origin &,
                          SyncMode, std::optional<std::span<const TermId>> allowed_terms = {});
void remove_watches(Subscriptions &, ScriptKey);
WatchChanges refresh_dependencies(Subscriptions &, const ValueRead &, FactKey changed, Step,
                                  double hz, const record::Origin &);
WatchChanges sync_rule_watches(Subscriptions &, const ValueRead &, const AllocationRows &,
                               const catalog::Scripts &, const RuleVersions &,
                               std::span<const Effect>, Step, double hz, const record::Origin &);
std::vector<Handle> source_consumers(const ValueRead &, FactKey, const WatchDeclaration &);
WatchResult check_changed(Subscriptions &, const ValueRead &, const AllocationRows &, FactKey, Step,
                          double hz, const record::Origin &, ScriptId recipient = 0);

std::vector<InvocationPlan> plan_deliveries(const WatchResult &, const Triggers &,
                                            const catalog::Scripts &, const ValueRead &, Step,
                                            double hz);
bool has_applicable_watch(const Subscriptions &, const Triggers &, const catalog::Scripts &,
                          const ValueRead &, FactKey, Step, double hz);
std::vector<step::BackgroundItem> catch_up(const SharedColumns &, IdentityRead,
                                           const catalog::Scripts &, const Terms &,
                                           const RuleVersion &, const record::Origin &);
bool catch_up_valid(const step::BackgroundItem &, const RuleVersions &, const ValueRead &, Step,
                    double hz);
void cancel_catch_up(step::WorkQueues &, const RuleVersion &);

void write_instances(snap::Output &, const InstanceScripts &, IdentityRead, ObjectSelection = {});
void read_instances(snap::Input &, InstanceScripts &, std::size_t slots);

// Direct reactive installation and cleanup. No lifecycle-owned references are retained.
void install_reactions(const catalog::Models &, const catalog::Scripts &,
                       const SparseColumn<std::vector<ScriptId>> &instances, const RuleVersions &,
                       const ValueRead &, Triggers &, Subscriptions &, const AllocationRows &,
                       std::span<const Handle>, Step, double hz, const record::Origin &,
                       DeferredEffects &);
void restore_reactions(const catalog::Models &, const catalog::Scripts &,
                       const SparseColumn<std::vector<ScriptId>> &instances, const RuleVersions &,
                       const ValueRead &, Triggers &, Subscriptions &, const AllocationRows &,
                       std::span<const Handle>, Step, double hz, const record::Origin &,
                       DeferredEffects &);
void initialize_reactions(const catalog::Models &models, const catalog::Scripts &scripts,
                          const InstanceScripts &installations, const RuleVersions &rules,
                          const ValueRead &values, Triggers &triggers, Subscriptions &subscriptions,
                          AllocationRows &allocations, MeterRows &meters,
                          std::span<const Handle> initial, Step now, double hz, bool restored,
                          DeferredEffects &effects);

void rebind_reactions(const catalog::Models &, const catalog::Scripts &,
                      const SparseColumn<std::vector<ScriptId>> &instances, const RuleVersions &,
                      const ValueRead &, Triggers &, Subscriptions &, const AllocationRows &,
                      std::span<const Handle>, Step, double hz, const record::Origin &,
                      DeferredEffects &);
void retire_reactions(const catalog::Models &, const ValueRead &,
                      SparseColumn<std::vector<ScriptId>> &instances, Triggers &, Subscriptions &,
                      Properties &, Processes &, InstanceSettings &, ScriptStates &, RandomStates &,
                      SharedColumns &, GivesTo &, AuthoredRelations &, AllocationRows &,
                      MeterRows &, std::span<const Handle>, Step, double hz, const record::Origin &,
                      DeferredEffects &);

// True means cached rule state changed and the caller must persist its registry pin.
bool update_rule(catalog::Scripts &, RuleVersions &, Triggers &, Subscriptions &,
                 step::WorkQueues &, Vm &, const ValueRead &, const AllocationRows &, ScriptId,
                 RuleMode, Step, double hz, const record::Origin &, DeferredEffects &);
void reconcile_allocations(const ValueRead &, AllocationRows &, MeterRows &, DeferredEffects &,
                           Step, double hz);
void reconcile_priority(const ValueRead &, AllocationRows &, MeterRows &, SettingKey,
                        DeferredEffects &, Step, double hz);
void reconcile_object_priorities(const ValueRead &, AllocationRows &, MeterRows &, Handle,
                                 DeferredEffects &, Step, double hz);

using SnapshotSelection = snap::ObjectSelection;
void write_properties(snap::Output &, const Properties &, const Processes &,
                      const SnapshotSelection & = {});
void read_properties(snap::Input &, Properties &, Processes &, std::size_t slots);
void write_relations(snap::Output &, const AuthoredRelations &, const SnapshotSelection & = {});
void read_relations(snap::Input &, AuthoredRelations &, std::size_t slots);
void write_shared(snap::Output &, const SharedColumns &, const GivesTo &, const AllocationRows &,
                  const MeterRows &, const SnapshotSelection & = {});
void read_shared(snap::Input &, SharedColumns &, GivesTo &, AllocationRows &, MeterRows &,
                 std::size_t slots);
void write_script_states(snap::Output &, const ScriptStates &, const Subscriptions &,
                         const SnapshotSelection & = {});
void read_script_states(snap::Input &, ScriptStates &, Subscriptions &);
void write_random(snap::Output &, const RandomStates &, const SnapshotSelection & = {});
void read_random(snap::Input &, RandomStates &);
void write_settings(snap::Output &, const InstanceSettings &, const ModelSettings &,
                    const PlotSettings &, const WorldSettings &, const SnapshotSelection & = {},
                    std::optional<std::span<const ModelId>> models = {},
                    std::optional<std::span<const PlotId>> plots = {});
void read_settings(snap::Input &, InstanceSettings &, ModelSettings &, PlotSettings &,
                   WorldSettings &, std::size_t slots);

} // namespace amber::react

namespace amber::catalog {
void resolve_manifest_terms(Asset &, const react::Terms &);
PreparedScript prepare_script(std::string_view source, const react::Terms &,
                              const react::RightsDeclaration &, Hash registry_pin,
                              const RuntimePins &, react::VmSettings);
void write_script(const std::filesystem::path &directory, const PreparedScript &);
PreparedScript read_script(const std::filesystem::path &root, Hash expected, const RuntimePins &,
                           Hash registry_pin, const react::Terms &,
                           const react::RightsDeclaration &);
ScriptId import_script(Scripts &, PreparedScript, PlayerId author, double created_s);
void restore_scripts(Scripts &, react::Vm &, react::RuleVersions &, const RegistryDocument &,
                     const react::Terms &, const RuntimePins &,
                     const std::filesystem::path &source_root,
                     const std::filesystem::path &destination_root);
void restore_assets(Assets &, const Models &, const ParameterBlocks &, const react::Terms &,
                    const RuntimePins &, const std::filesystem::path &asset_root);
ModelRegistration submit_model(Models &, ModelTexts &, react::ModelSettings &, const Assets &,
                               const Scripts &, const react::Terms &, const ModelSubmission &);
std::uint32_t add_variant(ParameterBlocks &, Models &, ModelTexts &, react::ModelSettings &,
                          const Assets &, const Scripts &, const react::Terms &, ModelId,
                          Hash asset, std::string canonical_params);
void project_measurements(react::AssetMeasurements &, ModelId, std::uint32_t, const Asset &,
                          const MeasurementBindings &);
void project_settings(react::ModelSettings &, const react::Terms &, ModelId, const react::Values &);
TermId append_term(react::Terms &, const TermProposal &, const DuplicateChoice &);
void set_material(react::Materials &, const react::Terms &, TermId, const react::Values &);
Hash save_registry(const std::filesystem::path &, const react::Terms &, const react::Materials &,
                   const react::RuleVersions &, const MechanicalBindings &);
RegistryDocument read_registry(const std::filesystem::path &, Hash);
void load_terms(const RegistryDocument &, react::Terms &);
void load_materials(const RegistryDocument &, const react::Terms &, react::Materials &);
void load_rules(const RegistryDocument &, const Scripts &, react::RuleVersions &);
nlohmann::json term_json(const react::Term &);
react::Term term_from_json(const nlohmann::json &);
nlohmann::json declaration_json(const react::ScriptDeclaration &);
react::ScriptDeclaration declaration_from_json(const nlohmann::json &);
void inherit_definition_settings(react::ModelSettings &, const ModelUpdate &);
void project_definition_measurements(react::AssetMeasurements &, const Models &,
                                     const ParameterBlocks &, const Assets &, const ModelUpdate &,
                                     MeasurementBindings);

using ScriptSelection = std::optional<std::span<const ScriptId>>;
void write_scripts(snap::Output &, const Scripts &, ScriptSelection = {});
void read_scripts(snap::Input &, Scripts &);
} // namespace amber::catalog

namespace amber::react::authoring {
using Json = nlohmann::json;
struct AssetRequest {
  catalog::Hash asset;
  std::string canonical_params;
};
struct ModelRequest {
  catalog::Hash asset;
  std::string canonical_params;
  TermId material = 0;
  catalog::Text text;
  std::vector<AssetRequest> variants;
};
struct ScriptRequest {
  std::string source;
};
struct VariantRequest {
  ModelId model = 0;
  catalog::Hash asset;
  std::string canonical_params;
};
ModelRequest model_request(const Json &, const react::Terms &);
VariantRequest variant_request(const Json &);
ScriptRequest script_request(const Json &);
struct RegistrySnapshot {
  react::Terms terms;
  catalog::Hash pin;
};
struct WorkerSettings {
  std::filesystem::path asset_root;
  catalog::RuntimePins pins;
  react::VmSettings limits;
};
struct PreparedModel {
  std::vector<catalog::Asset> assets; // root first, then its real declared parts
  TermId material = 0;
  catalog::Text text;
  std::vector<std::vector<catalog::Asset>> variants;
};
struct PreparedVariant {
  ModelId model = 0;
  std::vector<catalog::Asset> assets;
};
struct Failure {
  std::string error;
};
struct Completion {
  std::uint64_t input_id = 0;
  std::variant<PreparedModel, PreparedVariant, catalog::PreparedScript, Failure> result;
};
class Worker {
public:
  explicit Worker(WorkerSettings);
  ~Worker();
  Worker(const Worker &) = delete;
  Worker &operator=(const Worker &) = delete;
  void submit(std::uint64_t input_id, ModelRequest, RegistrySnapshot);
  void submit(std::uint64_t input_id, VariantRequest, RegistrySnapshot);
  void submit(std::uint64_t input_id, ScriptRequest, RegistrySnapshot);
  std::vector<Completion> poll();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// These writer functions install only validated immutable submissions. Geometry
// preparation and record/publication policy belong to step's direct caller.
step::NativeCompletion install_model(PreparedModel, PlayerId author, double created_s,
                                     catalog::Assets &, catalog::Models &,
                                     catalog::ParameterBlocks &, catalog::ModelTexts &,
                                     react::ModelSettings &, react::AssetMeasurements &,
                                     const catalog::Scripts &, const react::Terms &,
                                     catalog::MeasurementBindings);
step::NativeCompletion install_variant(PreparedVariant, catalog::Assets &, catalog::Models &,
                                       catalog::ParameterBlocks &, catalog::ModelTexts &,
                                       react::ModelSettings &, react::AssetMeasurements &,
                                       const catalog::Scripts &, const react::Terms &,
                                       catalog::MeasurementBindings);
step::NativeCompletion install_script(catalog::PreparedScript, PlayerId author, double created_s,
                                      catalog::Scripts &, react::Vm &, const react::NativeActions &,
                                      const std::filesystem::path &script_root);
Json failure(std::string);
} // namespace amber::react::authoring

namespace amber::react {

TermId declare_native(NativeActions &, react::Terms &, std::string name, std::string description,
                      std::string right, bool reach, nlohmann::json inputs_schema, NativeOperation);

Resolution resolve(const NativeActions &, const react::Terms &, const catalog::Models &,
                   const catalog::Scripts &, const InstalledScripts &, IdentityRead,
                   const ActionRequest &, Handle authenticated_body);
bool collides(const NativeActions &, const react::ScriptDeclaration &);

std::vector<ArgumentError> validate_arguments(const react::Terms &, const react::Term &,
                                              const EventPayload &);
std::string format_unit(const std::array<std::int8_t, 8> &dimensions);
std::string action_signature(const react::Terms &, const NativeActions &, TermId action);
} // namespace amber::react
