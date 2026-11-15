#include "install.hpp"

#include <algorithm>
#include <stdexcept>

namespace amber::install {
namespace {
const catalog::Script &script(const catalog::Scripts &scripts, ScriptId id) {
  if (!id || id >= scripts.rows.size() || scripts.rows[id].id != id)
    throw std::invalid_argument("unknown script installation");
  return scripts.rows[id];
}
react::HandlerRef handler(const catalog::Script &source, const react::Terms &terms, TermId term) {
  return {source.id, terms.rows.at(term).name, source.author, source.declaration.version,
          source.declaration.active};
}
RuleKey key(const react::RuleVersion &rule) {
  return {rule.trigger, rule.selector, rule.flag_selector};
}
bool matches(const react::RuleVersion &rule, const react::ValueRead &values, Handle object,
             Step now, double hz) {
  if (!values.identity.valid(object))
    return false;
  if (!rule.flag_selector)
    return values.identity.material[object.slot()] == rule.selector;
  const auto value = react::read(values, object, rule.selector, now, hz);
  return value.present && (value.value.kind == Value::Kind::Boolean
                               ? value.value.boolean
                               : value.value.is_numeric() && value.value.numeric() != 0);
}
} // namespace
void set_instances(InstanceScripts &instances, Handle object, std::span<const ScriptId> ids,
                   const catalog::Scripts &scripts) {
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (script(scripts, ids[i]).declaration.kind == react::ScriptDeclaration::Kind::Rule)
      throw std::invalid_argument("world rules cannot be installed as ordinary scripts");
    if (std::find(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(i), ids[i]) !=
        ids.begin() + static_cast<std::ptrdiff_t>(i))
      throw std::invalid_argument("duplicate instance script");
  }
  if (ids.empty())
    instances.erase(object.slot());
  else
    instances.set(object.slot(), std::vector<ScriptId>(ids.begin(), ids.end()));
}
std::vector<ScriptId> installed_scripts(const catalog::Models &models,
                                        const InstanceScripts &instances, IdentityRead identity,
                                        Handle object) {
  std::vector<ScriptId> ids;
  if (!identity.valid(object))
    return ids;
  const auto model = identity.model[object.slot()];
  if (model < models.rows.size() && models.rows[model].script)
    ids.push_back(models.rows[model].script);
  if (const auto *extra = instances.get(object.slot()))
    for (auto id : *extra)
      if (std::find(ids.begin(), ids.end(), id) == ids.end())
        ids.push_back(id);
  return ids;
}
void project_model(react::Triggers &triggers, const catalog::Models &models,
                   const catalog::Scripts &scripts, const react::Terms &terms, ModelId id) {
  for (auto &[term, table] : triggers) {
    (void)term;
    table.by_model.erase(id);
  }
  const auto &model = models.rows.at(id);
  if (!model.script)
    return;
  const auto &source = script(scripts, model.script);
  for (auto term : source.declaration.handlers)
    triggers[term].by_model[id] = handler(source, terms, term);
}
void project_instance(react::Triggers &triggers, const InstanceScripts &instances,
                      const catalog::Scripts &scripts, const react::Terms &terms, Handle object) {
  for (auto &[term, table] : triggers) {
    (void)term;
    table.by_instance.erase(object.bits);
  }
  if (const auto *installed = instances.get(object.slot()))
    for (auto id : *installed) {
      const auto &source = script(scripts, id);
      for (auto term : source.declaration.handlers)
        triggers[term].by_instance[object.bits].push_back(handler(source, terms, term));
    }
}
std::vector<react::RuleVersion> latest_rules(const react::RuleVersions &versions) {
  std::map<RuleKey, react::RuleVersion> current;
  for (const auto &row : versions.history)
    current[key(row)] = row;
  std::vector<react::RuleVersion> rows;
  rows.reserve(current.size());
  for (const auto &[key, row] : current) {
    (void)key;
    rows.push_back(row);
  }
  return rows;
}
RuleChange add_rule(react::RuleVersions &versions, const catalog::Scripts &scripts,
                    const react::Terms &terms, ScriptId id) {
  const auto &source = script(scripts, id);
  const auto &declaration = source.declaration;
  if (declaration.kind != react::ScriptDeclaration::Kind::Rule || !declaration.approved)
    throw std::invalid_argument("approved world rule required");
  auto selector = terms.by_name.find(declaration.selector);
  if (selector == terms.by_name.end())
    throw std::invalid_argument("unknown rule selector");
  const auto kind = terms.rows[selector->second].kind;
  if (kind != react::Kind::Flag && kind != react::Kind::Material)
    throw std::invalid_argument("rule selector needs a flag or material");
  const auto existing = latest_rules(versions);
  RuleChange change;
  for (auto trigger : declaration.handlers) {
    const RuleKey desired{trigger, selector->second, kind == react::Kind::Flag};
    std::uint32_t version = 1;
    for (const auto &old : existing)
      if (key(old) == desired) {
        version = old.handler.version + 1;
        change.retired.push_back(old);
        break;
      }
    react::RuleVersion row{
        {id, terms.rows.at(trigger).name, source.author, version, declaration.active},
        trigger,
        selector->second,
        kind == react::Kind::Flag};
    versions.history.push_back(row);
    if (row.handler.enabled)
      change.active.push_back(std::move(row));
  }
  return change;
}
RuleChange set_rule_enabled(react::RuleVersions &versions, RuleKey desired, bool enabled) {
  for (auto it = versions.history.rbegin(); it != versions.history.rend(); ++it)
    if (key(*it) == desired) {
      RuleChange change;
      if (it->handler.enabled == enabled)
        return change;
      if (it->handler.enabled)
        change.retired.push_back(*it);
      it->handler.enabled = enabled;
      if (enabled)
        change.active.push_back(*it);
      return change;
    }
  throw std::invalid_argument("unknown rule selector/trigger");
}
void project_rules(react::Triggers &triggers, const react::RuleVersions &versions) {
  for (auto &[term, table] : triggers) {
    (void)term;
    table.by_material.clear();
    table.by_flag.clear();
  }
  for (const auto &rule : latest_rules(versions)) {
    auto &table = triggers[rule.trigger];
    if (rule.flag_selector)
      table.by_flag.emplace_back(rule.selector, rule.handler);
    else
      table.by_material[rule.selector] = rule.handler;
  }
}
std::vector<ScriptId> rule_scripts(const react::RuleVersions &versions,
                                   const react::ValueRead &values, Handle object, Step now,
                                   double hz) {
  std::vector<ScriptId> ids;
  for (const auto &rule : latest_rules(versions))
    if (rule.handler.enabled && matches(rule, values, object, now, hz) &&
        std::find(ids.begin(), ids.end(), rule.handler.script) == ids.end())
      ids.push_back(rule.handler.script);
  return ids;
}
std::vector<TermId> rule_watch_terms(const react::RuleVersions &versions,
                                     const react::ValueRead &values, Handle object, ScriptId script,
                                     Step now, double hz) {
  std::vector<TermId> terms;
  for (const auto &rule : latest_rules(versions))
    if (rule.handler.enabled && rule.handler.script == script &&
        matches(rule, values, object, now, hz))
      terms.push_back(rule.trigger);
  return terms;
}
std::vector<step::BackgroundItem>
catch_up(const react::SharedColumns &shared, IdentityRead identity, const catalog::Scripts &scripts,
         const react::Terms &terms, const react::RuleVersion &rule, const record::Origin &origin) {
  std::vector<step::BackgroundItem> items;
  const auto &declaration = scripts.rows.at(rule.handler.script).declaration;
  const bool watched = std::any_of(declaration.watches.begin(), declaration.watches.end(),
                                   [&](const auto &watch) { return watch.term == rule.trigger; });
  if (!rule.handler.enabled || !watched ||
      terms.rows.at(rule.trigger).kind != react::Kind::Quantity || rule.trigger >= shared.size())
    return items;
  for (auto slot : shared[rule.trigger].slot_of) {
    const auto object = identity.at(slot);
    if (identity.valid(object))
      items.push_back({object, rule.trigger, rule.handler.script, origin, rule.handler.version});
  }
  return items;
}
bool catch_up_valid(const step::BackgroundItem &item, const react::RuleVersions &versions,
                    const react::ValueRead &values, Step now, double hz) {
  (void)now;
  (void)hz;
  if (!values.identity.valid(item.receiver) || item.trigger >= values.shared.size() ||
      !values.shared[item.trigger].get(item.receiver.slot()))
    return false;
  for (auto it = versions.history.rbegin(); it != versions.history.rend(); ++it)
    if (it->trigger == item.trigger && it->handler.script == item.script &&
        it->handler.version == item.version) {
      if (!it->handler.enabled)
        return false;
      const auto selected = key(*it);
      return std::none_of(versions.history.rbegin(), it,
                          [&](const auto &later) { return key(later) == selected; });
    }
  return false;
}
void cancel_catch_up(step::WorkQueues &queues, const react::RuleVersion &rule) {
  std::erase_if(queues.background, [&](const auto &item) {
    return item.trigger == rule.trigger && item.script == rule.handler.script &&
           item.version == rule.handler.version;
  });
}
void write_instances(snap::Output &out, const InstanceScripts &instances, IdentityRead identity,
                     ObjectSelection selected) {
  auto field = [](snap::Output &stream, auto &value) { snap::field(stream, value); };
  if (!selected) {
    snap::write_sparse(out, instances, field);
    return;
  }
  InstanceScripts rows;
  if (selected) {
    for (auto object : *selected)
      if (identity.valid(object))
        if (const auto *ids = instances.get(object.slot()))
          rows.set(object.slot(), *ids);
  }
  snap::write_sparse(out, rows, field);
}
void read_instances(snap::Input &in, InstanceScripts &instances, std::size_t slots) {
  snap::read_sparse(in, instances,
                    [](snap::Input &stream, auto &value) { snap::field(stream, value); });
  instances.rebuild_positions(slots);
}
} // namespace amber::install
