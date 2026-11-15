#include "react.hpp"

#include <algorithm>

namespace amber::react {
namespace {
void install_with_mode(const amber::catalog::State &catalog_state, amber::react::State &react_state,
                       const ValueRead &values, std::span<const Handle> objects, Step now,
                       double hz, const record::Origin &origin, DeferredEffects &effects,
                       react::SyncMode mode) {
  auto &models = catalog_state.models;
  auto &scripts_table = catalog_state.scripts;
  auto &instances = react_state.installations;
  auto &rules = react_state.rules;
  auto &triggers = react_state.triggers;
  auto &subscriptions = react_state.subscriptions;

  for (auto h : objects) {
    react::project_instance(triggers, instances, scripts_table, values.terms, h);
    auto scripts = react::installed_scripts(models, instances, values.identity, h);
    auto applicable = react::rule_scripts(rules, values, h, now, hz);
    scripts.insert(scripts.end(), applicable.begin(), applicable.end());
    if (mode == react::SyncMode::Change)
      for (auto consumer : watch_consumers(subscriptions, h))
        if (std::find(scripts.begin(), scripts.end(), consumer.script) == scripts.end())
          react::remove_watches(subscriptions, consumer);
    for (const auto id : scripts) {
      const auto &script = scripts_table.rows.at(id);
      const auto allowed = react::rule_watch_terms(rules, values, h, id, now, hz);
      auto changes = react::sync_watches(
          react_state, values, {h, id}, script.declaration, now, hz, origin, mode,
          script.declaration.kind == react::ScriptDeclaration::Kind::Rule
              ? std::optional<std::span<const TermId>>(allowed)
              : std::nullopt);
      for (auto &work : changes.dirty)
        effects.ordered.emplace_back(std::move(work));
      for (auto &timer : changes.timers)
        effects.ordered.emplace_back(std::move(timer));
    }
  }
}
} // namespace
void install_reactions(const amber::catalog::State &catalog_state, amber::react::State &react_state,
                       const ValueRead &values, std::span<const Handle> objects, Step now,
                       double hz, const record::Origin &origin, DeferredEffects &effects) {

  install_with_mode(catalog_state, react_state, values, objects, now, hz, origin, effects,
                    react::SyncMode::Initial);
}
void restore_reactions(const amber::catalog::State &catalog_state, amber::react::State &react_state,
                       const ValueRead &values, std::span<const Handle> objects, Step now,
                       double hz, const record::Origin &origin, DeferredEffects &effects) {

  install_with_mode(catalog_state, react_state, values, objects, now, hz, origin, effects,
                    react::SyncMode::Restore);
}
void rebind_reactions(const amber::catalog::State &catalog_state, amber::react::State &react_state,
                      const ValueRead &values, std::span<const Handle> objects, Step now, double hz,
                      const record::Origin &origin, DeferredEffects &effects) {

  auto &subscriptions = react_state.subscriptions;

  install_with_mode(catalog_state, react_state, values, objects, now, hz, origin, effects,
                    react::SyncMode::Change);
  // Binding changes can alter intrinsic, baked and default answers without a
  // state write. Include subscribers on other objects following this source;
  // preserve their saved answers so the ordinary driver compares actual values.
  for (auto object : objects)
    for (auto source : watch_sources(subscriptions, object))
      effects.ordered.emplace_back(step::WatchWork{source, origin});
}
void retire_reactions(const catalog::Models &models, const ValueRead &values,
                      amber::react::State &react_state, std::span<const Handle> objects, Step now,
                      double hz, const record::Origin &origin, DeferredEffects &effects) {
  auto &instances = react_state.installations;
  auto &triggers = react_state.triggers;
  auto &subscriptions = react_state.subscriptions;
  auto &properties = react_state.properties;
  auto &processes = react_state.processes;
  auto &settings = react_state.instance_settings;
  auto &states = react_state.states;
  auto &random = react_state.random;
  auto &shared = react_state.shared;
  auto &gives_to = react_state.gives_to;
  auto &relations = react_state.authored_relations;
  auto &allocations = react_state.allocations;
  auto &meters = react_state.meters;

  for (auto h : objects) {
    const auto consumers = watch_consumers(subscriptions, h);
    for (const auto key : consumers)
      react::remove_watches(subscriptions, key);
    react::withdraw_contributor(shared, gives_to, values.terms, h, effects);
    for (auto &[term, relation] : relations) {
      const auto *out = relation.out.get(h.slot());
      const auto outgoing = out ? *out : std::vector<Handle>{};
      const auto *in = relation.in.get(h.slot());
      const auto incoming = in ? *in : std::vector<Handle>{};
      for (auto other : outgoing)
        react::set_relation(relation, term, h, other, false, effects);
      for (auto other : incoming)
        react::set_relation(relation, term, other, h, false, effects);
    }
    for (TermId term = 0; term < shared.size(); ++term)
      if (const auto *row = shared[term].get(h.slot())) {
        for (const auto &part : row->parts)
          if (auto *reverse = gives_to.get(part.from.slot()))
            std::erase(*reverse, react::SubjectTerm{h, term});
        shared[term].erase(h.slot());
        effects.ordered.emplace_back(react::ContributionChange{{h, term}});
        effects.ordered.emplace_back(step::WatchWork{{h, term}, origin});
      }
  }
  // Rebind surviving consumers while their old answers still exist. In
  // particular, an along edge disappearing must deliver absence once.
  std::vector<FactKey> changed;
  for (const auto &effect : effects.ordered)
    if (const auto *work = std::get_if<step::WatchWork>(&effect))
      changed.push_back(work->source);
  for (auto fact : changed) {
    auto rebound = react::refresh_dependencies(subscriptions, values, fact, now, hz, origin);
    for (auto &dirty : rebound.dirty)
      effects.ordered.emplace_back(std::move(dirty));
    for (auto &timer : rebound.timers)
      effects.ordered.emplace_back(std::move(timer));
  }
  reconcile_allocations(values, react_state, effects, now, hz);
  for (auto h : objects) {
    auto installed = react::installed_scripts(models, instances, values.identity, h);
    react::retire_subscriptions(subscriptions, h);
    react::retire_allocations(allocations, meters, h);
    react::retire_object(properties, processes, states, random, h, installed);
    std::erase_if(states, [&](const auto &row) { return row.first.object == h; });
    for (auto &column : settings.columns)
      column.erase(h.slot());
    instances.erase(h.slot());
    for (auto &[term, trigger] : triggers) {
      (void)term;
      trigger.by_instance.erase(h.bits);
    }
  }
}
void initialize_reactions(const amber::catalog::State &catalog_state,
                          amber::react::State &react_state, const ValueRead &values,
                          std::span<const Handle> initial, Step now, double hz, bool restored,
                          DeferredEffects &effects) {
  auto &models = catalog_state.models;
  auto &scripts = catalog_state.scripts;

  auto &rules = react_state.rules;
  auto &triggers = react_state.triggers;

  for (const auto &model : models.rows)
    if (model.id)
      react::project_model(triggers, models, scripts, values.terms, model.id);
  react::project_rules(triggers, rules);
  if (restored)
    react::restore_reactions(catalog_state, react_state, values, initial, now, hz, {}, effects);
  else
    react::install_reactions(catalog_state, react_state, values, initial, now, hz, {}, effects);
  for (auto object : initial)
    react::reconcile_object_priorities(values, react_state, object, effects, now, hz);
}
} // namespace amber::react
