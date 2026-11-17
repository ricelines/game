#include "catalog/catalog.hpp"
#include "react.hpp"
#include "snap/snap.hpp"

#include <algorithm>
#include <stdexcept>

namespace amber::react {
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
} // namespace
void set_instances(InstanceScripts &instances, Handle object, std::span<const ScriptId> ids,
                   const catalog::Scripts &scripts) {
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const auto kind = script(scripts, ids[i]).declaration.kind;
    if (kind == ScriptDeclaration::Kind::Rule || kind == ScriptDeclaration::Kind::World)
      throw std::invalid_argument("only model and instance scripts can be installed on objects");
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
  if (source.declaration.kind == ScriptDeclaration::Kind::Rule ||
      source.declaration.kind == ScriptDeclaration::Kind::World)
    throw std::invalid_argument("only model and instance scripts can be installed on models");
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
WorldScript *world_script(std::vector<WorldScript> &world, ScriptId id) {
  const auto found = std::ranges::find(world, id, &WorldScript::script);
  return found == world.end() ? nullptr : &*found;
}
const WorldScript *world_script(const std::vector<WorldScript> &world, ScriptId id) {
  const auto found = std::ranges::find(world, id, &WorldScript::script);
  return found == world.end() ? nullptr : &*found;
}
void project_world(State &state, const catalog::Scripts &scripts) {
  for (auto &[term, trigger] : state.triggers) {
    (void)term;
    trigger.by_world.clear();
  }
  for (const auto &installation : state.world_scripts) {
    const auto &source = script(scripts, installation.script);
    if (source.declaration.kind != ScriptDeclaration::Kind::World)
      throw std::invalid_argument("world installation requires a world script");
    for (auto term : source.declaration.handlers)
      state.triggers[term].by_world.push_back(handler(source, state.terms, term));
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
std::vector<ScriptId> rule_scripts(const Triggers &triggers, const ValueRead &values, Handle object,
                                   Step now, double hz) {
  std::vector<ScriptId> ids;
  if (!values.identity.valid(object))
    return ids;
  auto add = [&](const HandlerRef &handler) {
    if (handler.enabled && std::find(ids.begin(), ids.end(), handler.script) == ids.end())
      ids.push_back(handler.script);
  };
  for (const auto &[term, row] : triggers) {
    (void)term;
    if (const auto material = row.by_material.find(values.identity.material[object.slot()]);
        material != row.by_material.end())
      add(material->second);
    for (const auto &[flag, handler] : row.by_flag) {
      const auto value = read(values, object, flag, now, hz);
      if (value.present && (value.value.kind == Value::Kind::Boolean
                                ? value.value.boolean
                                : value.value.is_numeric() && value.value.numeric() != 0))
        add(handler);
    }
  }
  return ids;
}
namespace {
bool holds_quantity(const ValueRead &values, Handle object, TermId term) {
  const auto &definition = values.terms.rows.at(term);
  if (definition.combine != Combine::None)
    return term < values.shared.size() && values.shared[term].get(object.slot());
  if (definition.source == Source::State)
    return (term < values.properties.size() && values.properties[term].get(object.slot())) ||
           (term < values.processes.size() && values.processes[term].get(object.slot()));
  if (definition.source == Source::Measured && definition.measurement == MeasurementRole::Runtime)
    return values.runtime.values.contains({object, term});
  if (definition.source == Source::Setting)
    return term < values.instance_settings.columns.size() &&
           values.instance_settings.columns[term].get(object.slot());
  return false;
}
} // namespace
std::vector<BackgroundItem> catch_up(const ValueRead &values, const catalog::Scripts &scripts,
                                     const RuleVersion &rule, const amber::Origin &origin) {
  std::vector<BackgroundItem> items;
  const auto &declaration = scripts.rows.at(rule.handler.script).declaration;
  const auto &term = values.terms.rows.at(rule.trigger);
  if (!rule.handler.enabled || term.kind != Kind::Quantity ||
      std::none_of(declaration.watches.begin(), declaration.watches.end(),
                   [&](const auto &watch) { return watch.term == rule.trigger; }))
    return items;
  auto add = [&](Handle object) {
    if (values.identity.valid(object))
      items.push_back({object, rule.trigger, rule.handler.script, origin, rule.handler.version});
  };
  if (term.combine != Combine::None) {
    if (term.id < values.shared.size())
      for (auto slot : values.shared[term.id].slot_of)
        add(values.identity.at(slot));
  } else if (term.source == Source::State) {
    const auto *properties =
        term.id < values.properties.size() ? &values.properties[term.id] : nullptr;
    if (properties)
      for (auto slot : properties->slot_of)
        add(values.identity.at(slot));
    if (term.id < values.processes.size())
      for (auto slot : values.processes[term.id].slot_of)
        if (!properties || !properties->get(slot))
          add(values.identity.at(slot));
  } else if (term.source == Source::Measured && term.measurement == MeasurementRole::Runtime) {
    for (const auto &[fact, value] : values.runtime.values) {
      (void)value;
      if (fact.term == term.id)
        add(fact.subject);
    }
  } else if (term.source == Source::Setting && term.id < values.instance_settings.columns.size()) {
    for (auto slot : values.instance_settings.columns[term.id].slot_of)
      add(values.identity.at(slot));
  }
  return items;
}
bool catch_up_valid(const BackgroundItem &item, const Triggers &triggers,
                    const catalog::Scripts &scripts, const ValueRead &values) {
  if (!values.identity.valid(item.receiver) || !holds_quantity(values, item.receiver, item.trigger))
    return false;
  const auto found = triggers.find(item.trigger);
  if (found == triggers.end())
    return false;
  const auto &declaration = scripts.rows.at(item.script).declaration;
  const auto selector = values.terms.by_name.at(declaration.selector);
  auto current = [&](const HandlerRef &handler) {
    return handler.enabled && handler.script == item.script && handler.version == item.version;
  };
  if (values.terms.rows.at(selector).kind == Kind::Flag) {
    const auto &flags = found->second.by_flag;
    const auto flag = std::find_if(flags.begin(), flags.end(),
                                   [&](const auto &row) { return row.first == selector; });
    return flag != flags.end() && current(flag->second);
  }
  const auto material = found->second.by_material.find(selector);
  return material != found->second.by_material.end() && current(material->second);
}
void cancel_catch_up(std::deque<BackgroundItem> &background, const react::RuleVersion &rule) {
  std::erase_if(background, [&](const auto &item) {
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
} // namespace amber::react
