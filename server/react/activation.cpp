#include "react.hpp"

#include <algorithm>
#include <unordered_set>

namespace amber::react {
bool update_rule(catalog::Scripts &scripts, amber::react::State &react_state,
                 step::WorkQueues &queues, const ValueRead &values, ScriptId id, RuleMode mode,
                 const amber::step::Clock &clock, const record::Origin &origin,
                 DeferredEffects &effects) {
  auto &vm = react_state.vm;

  const auto now = clock.step;
  const auto hz = clock.hz;

  auto &rules = react_state.rules;
  auto &triggers = react_state.triggers;
  auto &subscriptions = react_state.subscriptions;

  if (!id || id >= scripts.rows.size() || scripts.rows[id].id != id ||
      scripts.rows[id].declaration.kind != react::ScriptDeclaration::Kind::Rule)
    throw std::invalid_argument("registered world rule required");
  auto &declaration = scripts.rows[id].declaration;
  if (mode != RuleMode::Validated && !declaration.approved)
    throw std::invalid_argument("rule has not passed fixture approval");
  const bool enable = mode != RuleMode::Disable;
  const auto previous = react::latest_rules(rules);
  const auto selector = values.terms.by_name.at(declaration.selector);
  const bool flag = values.terms.rows.at(selector).kind == react::Kind::Flag;
  const bool already_current =
      std::all_of(declaration.handlers.begin(), declaration.handlers.end(), [&](TermId trigger) {
        return std::any_of(previous.begin(), previous.end(), [&](const auto &rule) {
          return rule.trigger == trigger && rule.selector == selector &&
                 rule.flag_selector == flag && rule.handler.script == id &&
                 rule.handler.enabled == enable;
        });
      });
  if (already_current && declaration.approved && declaration.active == enable)
    return false;
  // The immutable module is already loaded on this writer; only its authorized
  // cached execution metadata changes here. No top-level code is evaluated.
  if (!react::set_activation(vm, id, true, enable))
    throw std::invalid_argument("world rule is not loaded on the writer");
  declaration.approved = true;
  declaration.active = enable;
  react::RuleChange change;
  if (enable)
    change = react::add_rule(rules, scripts, values.terms, id);
  else
    for (const auto &rule : previous)
      if (rule.handler.script == id && rule.handler.enabled) {
        auto retired = react::set_rule_enabled(
            rules, {rule.trigger, rule.selector, rule.flag_selector}, false);
        change.retired.insert(change.retired.end(), retired.retired.begin(), retired.retired.end());
      }
  react::project_rules(triggers, rules);
  std::unordered_set<ScriptId> affected;
  for (const auto &retired : change.retired) {
    react::cancel_catch_up(queues, retired);
    affected.insert(retired.handler.script);
  }
  affected.insert(id);
  const auto current = react::latest_rules(rules);
  for (auto script : affected) {
    const bool active = std::any_of(current.begin(), current.end(), [&](const auto &rule) {
      return rule.handler.script == script && rule.handler.enabled;
    });
    if (!active && scripts.rows.at(script).declaration.active) {
      scripts.rows[script].declaration.active = false;
      react::set_activation(vm, script, scripts.rows[script].declaration.approved, false);
    }
    std::vector<ScriptKey> consumers;
    for (const auto &[source, bindings] : subscriptions.rows) {
      (void)source;
      for (const auto &binding : bindings)
        if (binding.consumer.script == script &&
            std::find(consumers.begin(), consumers.end(), binding.consumer) == consumers.end())
          consumers.push_back(binding.consumer);
    }
    for (auto consumer : consumers) {
      const auto allowed = react::rule_watch_terms(rules, values, consumer.object, script, now, hz);
      if (allowed.empty()) {
        react::remove_watches(subscriptions, consumer);
        continue;
      }
      auto changes = react::sync_watches(react_state, values, consumer,
                                         scripts.rows.at(script).declaration, clock, origin,
                                         react::SyncMode::Change, std::span<const TermId>(allowed));
      for (auto &dirty : changes.dirty)
        effects.ordered.emplace_back(std::move(dirty));
      for (auto &timer : changes.timers)
        effects.ordered.emplace_back(std::move(timer));
    }
  }
  for (const auto &active : change.active)
    for (auto &item :
         react::catch_up(values.shared, values.identity, scripts, values.terms, active, origin))
      effects.background.push_back(std::move(item));
  effects.changed = true;
  return true;
}
} // namespace amber::react
