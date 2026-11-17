#include "catalog/catalog.hpp"
#include "react.hpp"

#include <algorithm>

namespace amber::react {
void install_reactions(const catalog::State &catalog, State &state, const ValueRead &values,
                       std::span<const Handle> objects, Step now, double hz,
                       const amber::Origin &origin, DeferredEffects &effects) {
  for (auto object : objects) {
    project_instance(state.triggers, state.installations, catalog.scripts, values.terms, object);
    baseline_watches(state, values, catalog.scripts, object, now, hz, origin, effects);
  }
}
void rebind_reactions(const catalog::State &catalog, State &state, const ValueRead &values,
                      std::span<const Handle> objects, Step now, double hz,
                      const amber::Origin &origin, DeferredEffects &effects) {
  for (auto object : objects) {
    project_instance(state.triggers, state.installations, catalog.scripts, values.terms, object);
    // Rebinding can change baked, intrinsic and setting answers without a write.
    // Preserve prior answers, including watches removed by this edit.
    for (const auto &[term, trigger] : state.triggers) {
      (void)trigger;
      const auto kind = values.terms.rows.at(term).kind;
      const FactKey fact{object, term};
      if ((kind == Kind::Quantity || kind == Kind::Flag || kind == Kind::Relation) &&
          (state.watch_answers.contains(fact) ||
           has_watch(state, values, catalog.scripts, fact, now, hz)))
        effects.ordered.emplace_back(WatchWork{fact, origin});
    }
  }
}
void refresh_plot_settings(const ValueRead &values, State &state, const catalog::Scripts &scripts,
                           std::span<const Handle> objects, Step now, double hz,
                           const amber::Origin &origin, DeferredEffects &effects) {
  if (objects.empty())
    return;
  std::vector<std::pair<TermId, bool>> settings;
  for (const auto &[term, trigger] : state.triggers) {
    if (values.terms.rows[term].source == Source::Setting)
      settings.emplace_back(term, false);
    for (const auto &[flag, handler] : trigger.by_flag)
      if (handler.enabled && values.terms.rows[flag].source == Source::Setting)
        settings.emplace_back(flag, true);
  }
  // Keep the selector role when a setting flag is also watched directly.
  std::sort(settings.begin(), settings.end(), [](const auto &a, const auto &b) {
    return a.first == b.first ? a.second > b.second : a.first < b.first;
  });
  settings.erase(std::unique(settings.begin(), settings.end(),
                             [](const auto &a, const auto &b) { return a.first == b.first; }),
                 settings.end());
  for (auto object : objects) {
    for (const auto &[term, selector] : settings)
      if (selector || has_watch(state, values, scripts, {object, term}, now, hz))
        effects.ordered.emplace_back(WatchWork{{object, term}, origin});
    reconcile_object_priorities(values, state, object, effects, now, hz);
  }
}
void retire_reactions(const ValueRead &values, amber::react::State &react_state,
                      std::span<const Handle> objects, Step now, double hz,
                      const amber::Origin &origin, DeferredEffects &effects) {
  auto &instances = react_state.installations;
  auto &triggers = react_state.triggers;
  auto &properties = react_state.properties;
  auto &processes = react_state.processes;
  auto &settings = react_state.instance_settings;
  auto &states = react_state.states;
  auto &random = react_state.random;
  auto &shared = react_state.shared;
  auto &gives_to = react_state.gives_to;
  auto &relations = react_state.authored_relations;

  for (auto h : objects) {
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
        effects.ordered.emplace_back(react::WatchWork{{h, term}, origin});
      }
  }
  reconcile_allocations(values, react_state, effects, now, hz);
  std::vector<Handle> retired(objects.begin(), objects.end());
  std::sort(retired.begin(), retired.end());
  std::erase_if(states, [&](const auto &row) {
    return std::binary_search(retired.begin(), retired.end(), row.first.object);
  });
  for (auto h : objects) {
    for (const auto &term : values.terms.rows)
      react_state.watch_answers.erase({h, term.id});
    for (auto &column : properties)
      column.erase(h.slot());
    for (auto &column : processes)
      column.erase(h.slot());
    random.erase(h.bits);
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
                          std::span<const Handle> initial, Step now, double hz,
                          DeferredEffects &effects) {
  auto &models = catalog_state.models;
  auto &scripts = catalog_state.scripts;

  auto &rules = react_state.rules;
  auto &triggers = react_state.triggers;

  for (const auto &model : models.rows)
    if (model.id)
      react::project_model(triggers, models, scripts, values.terms, model.id);
  react::project_rules(triggers, rules);
  react::project_world(react_state, scripts);
  react::install_reactions(catalog_state, react_state, values, initial, now, hz, {}, effects);
}
} // namespace amber::react
