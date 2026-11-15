#include "installation.hpp"

#include <algorithm>

namespace amber::bindings {
namespace {
void install_with_mode(InstallReactions &b, std::span<const Handle> objects,
                       const InvocationInput &input, react::DeferredEffects &effects,
                       install::SyncMode mode) {
  for (auto h : objects) {
    install::project_instance(b.triggers, b.instances, b.scripts, b.values.terms, h);
    auto scripts = install::installed_scripts(b.models, b.instances, b.values.identity, h);
    auto rules = install::rule_scripts(b.rules, b.values, h, input.step, input.hz);
    scripts.insert(scripts.end(), rules.begin(), rules.end());
    if (mode == install::SyncMode::Change)
      if (auto found = b.subscriptions.consumers_on.find(h.bits);
          found != b.subscriptions.consumers_on.end()) {
        const auto old = found->second;
        for (auto consumer : old)
          if (std::find(scripts.begin(), scripts.end(), consumer.script) == scripts.end())
            install::remove_watches(b.subscriptions, b.links, consumer);
      }
    for (const auto id : scripts) {
      const auto &script = b.scripts.rows.at(id);
      const auto allowed =
          install::rule_watch_terms(b.rules, b.values, h, id, input.step, input.hz);
      auto changes =
          install::sync_watches(b.subscriptions, b.links, b.values, b.allocations, {h, id},
                                script.declaration, input.step, input.hz, input.origin, mode,
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
void install_reactions(InstallReactions &b, std::span<const Handle> objects,
                       const InvocationInput &input, react::DeferredEffects &effects) {
  install_with_mode(b, objects, input, effects, install::SyncMode::Initial);
}
void restore_reactions(InstallReactions &b, std::span<const Handle> objects,
                       const InvocationInput &input, react::DeferredEffects &effects) {
  install_with_mode(b, objects, input, effects, install::SyncMode::Restore);
}
void rebind_reactions(InstallReactions &b, std::span<const lifecycle::Rebinding> changes,
                      const InvocationInput &input, react::DeferredEffects &effects) {
  std::vector<Handle> objects;
  objects.reserve(changes.size());
  for (const auto &change : changes)
    objects.push_back(change.object);
  install_with_mode(b, objects, input, effects, install::SyncMode::Change);
  const auto &subscriptions = b.subscriptions;
  // Binding changes can alter intrinsic, baked and default answers without a
  // state write. Include subscribers on other objects following this source;
  // preserve their saved answers so the ordinary driver compares actual values.
  for (auto object : objects)
    if (const auto found = subscriptions.by_subject.find(object.bits);
        found != subscriptions.by_subject.end())
      for (auto source : found->second)
        effects.ordered.emplace_back(step::WatchWork{source, input.origin});
}
void rebind_priorities(RebindPriorities &binding, std::span<const lifecycle::Rebinding> rows,
                       const InvocationInput &input, react::DeferredEffects &effects) {
  for (const auto &row : rows)
    reconcile_object_priorities(binding.values, binding.allocation, row.object, effects, input.step,
                                input.hz);
}
void retire_reactions(RetireReactions &b, std::span<const Handle> objects,
                      const InvocationInput &input, react::DeferredEffects &effects) {
  for (auto h : objects) {
    const auto found = b.subscriptions.consumers_on.find(h.bits);
    const auto consumers =
        found == b.subscriptions.consumers_on.end() ? std::vector<ScriptKey>{} : found->second;
    for (const auto key : consumers)
      install::remove_watches(b.subscriptions, b.links, key);
    react::withdraw_contributor(b.shared, b.gives_to, b.values.terms, h, effects);
    for (auto &[term, relation] : b.relations) {
      const auto *out = relation.out.get(h.slot());
      const auto outgoing = out ? *out : std::vector<Handle>{};
      const auto *in = relation.in.get(h.slot());
      const auto incoming = in ? *in : std::vector<Handle>{};
      for (auto other : outgoing)
        react::set_relation(relation, term, h, other, false, effects);
      for (auto other : incoming)
        react::set_relation(relation, term, other, h, false, effects);
    }
    for (TermId term = 0; term < b.shared.size(); ++term)
      if (const auto *shared = b.shared[term].get(h.slot())) {
        for (const auto &part : shared->parts)
          if (auto *reverse = b.gives_to.get(part.from.slot()))
            std::erase(*reverse, react::SubjectTerm{h, term});
        b.shared[term].erase(h.slot());
        effects.ordered.emplace_back(react::ContributionChange{{h, term}});
        effects.ordered.emplace_back(step::WatchWork{{h, term}, {}});
      }
  }
  // Rebind surviving consumers while their old answers still exist. In
  // particular, an along edge disappearing must deliver absence once.
  std::vector<FactKey> changed;
  for (const auto &effect : effects.ordered)
    if (const auto *work = std::get_if<step::WatchWork>(&effect))
      changed.push_back(work->source);
  for (auto fact : changed) {
    auto rebound = install::refresh_dependencies(b.subscriptions, b.links, b.values, fact,
                                                 input.step, input.hz, {});
    for (auto &dirty : rebound.dirty)
      effects.ordered.emplace_back(std::move(dirty));
    for (auto &timer : rebound.timers)
      effects.ordered.emplace_back(std::move(timer));
  }
  reconcile_allocations(b.values, b.allocation, effects, input.step, input.hz);
  for (auto h : objects) {
    auto installed = install::installed_scripts(b.models, b.instances, b.values.identity, h);
    react::retire_subscriptions(b.subscriptions, h);
    react::retire_allocations(b.allocation.allocations, b.allocation.meters, h);
    react::retire_priorities(b.allocation.priorities, h);
    react::retire_object(b.properties, b.processes, b.states, b.random, h, installed);
    std::erase_if(b.states, [&](const auto &row) { return row.first.object == h; });
    for (auto &column : b.settings.columns)
      column.erase(h.slot());
    b.instances.erase(h.slot());
    for (auto &[term, trigger] : b.triggers) {
      (void)term;
      trigger.by_instance.erase(h.bits);
    }
  }
}
} // namespace amber::bindings
