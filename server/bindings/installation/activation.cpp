#include "installation.hpp"

#include <algorithm>
#include <unordered_set>

namespace amber::bindings {
void update_rule(RuleActivation &b, ScriptId id, RuleMode mode, Step now, double hz,
                 const record::Origin &origin, react::DeferredEffects &effects) {
  if (!id || id >= b.scripts.rows.size() || b.scripts.rows[id].id != id ||
      b.scripts.rows[id].declaration.kind != react::ScriptDeclaration::Kind::Rule)
    throw std::invalid_argument("registered world rule required");
  auto &declaration = b.scripts.rows[id].declaration;
  if (mode != RuleMode::Validated && !declaration.approved)
    throw std::invalid_argument("rule has not passed fixture approval");
  const bool enable = mode != RuleMode::Disable;
  const auto previous = install::latest_rules(b.rules);
  const auto selector = b.values.terms.by_name.at(declaration.selector);
  const bool flag = b.values.terms.rows.at(selector).kind == react::Kind::Flag;
  const bool already_current =
      std::all_of(declaration.handlers.begin(), declaration.handlers.end(), [&](TermId trigger) {
        return std::any_of(previous.begin(), previous.end(), [&](const auto &rule) {
          return rule.trigger == trigger && rule.selector == selector &&
                 rule.flag_selector == flag && rule.handler.script == id &&
                 rule.handler.enabled == enable;
        });
      });
  if (already_current && declaration.approved && declaration.active == enable)
    return;
  // The immutable module is already loaded on this writer; only its authorized
  // cached execution metadata changes here. No top-level code is evaluated.
  if (!react::set_activation(b.vm, id, true, enable))
    throw std::invalid_argument("world rule is not loaded on the writer");
  declaration.approved = true;
  declaration.active = enable;
  install::RuleChange change;
  if (enable)
    change = install::add_rule(b.rules, b.scripts, b.values.terms, id);
  else
    for (const auto &rule : previous)
      if (rule.handler.script == id && rule.handler.enabled) {
        auto retired = install::set_rule_enabled(
            b.rules, {rule.trigger, rule.selector, rule.flag_selector}, false);
        change.retired.insert(change.retired.end(), retired.retired.begin(), retired.retired.end());
      }
  install::project_rules(b.triggers, b.rules);
  std::unordered_set<ScriptId> affected;
  for (const auto &retired : change.retired) {
    install::cancel_catch_up(b.queues, retired);
    affected.insert(retired.handler.script);
  }
  affected.insert(id);
  const auto current = install::latest_rules(b.rules);
  for (auto script : affected) {
    const bool active = std::any_of(current.begin(), current.end(), [&](const auto &rule) {
      return rule.handler.script == script && rule.handler.enabled;
    });
    if (!active && b.scripts.rows.at(script).declaration.active) {
      b.scripts.rows[script].declaration.active = false;
      react::set_activation(b.vm, script, b.scripts.rows[script].declaration.approved, false);
    }
    std::vector<ScriptKey> consumers;
    for (const auto &[key, link] : b.links.rows) {
      (void)link;
      if (key.consumer.script == script &&
          std::find(consumers.begin(), consumers.end(), key.consumer) == consumers.end())
        consumers.push_back(key.consumer);
    }
    for (auto consumer : consumers) {
      const auto allowed =
          install::rule_watch_terms(b.rules, b.values, consumer.object, script, now, hz);
      if (allowed.empty()) {
        install::remove_watches(b.subscriptions, b.links, consumer);
        continue;
      }
      auto changes =
          install::sync_watches(b.subscriptions, b.links, b.values, b.allocations, consumer,
                                b.scripts.rows.at(script).declaration, now, hz, origin,
                                install::SyncMode::Change, std::span<const TermId>(allowed));
      for (auto &dirty : changes.dirty)
        effects.ordered.emplace_back(std::move(dirty));
      for (auto &timer : changes.timers)
        effects.ordered.emplace_back(std::move(timer));
    }
  }
  for (const auto &active : change.active)
    for (auto &item : install::catch_up(b.values.shared, b.values.identity, b.scripts,
                                        b.values.terms, active, origin))
      effects.background.push_back(std::move(item));
  effects.changed = true;
  if (b.publication.publish)
    b.publication.publish(b.publication.local, id, true, enable, now, hz, origin, effects);
}
} // namespace amber::bindings
