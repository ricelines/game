#include "editing_private.hpp"

namespace amber::step::editing {
namespace {
using Changes = std::vector<std::pair<TermId, std::optional<Value>>>;
Changes settings(const react::Terms &terms, const Json &values) {
  Changes changed;
  for (const auto &[name, value] : values.items()) {
    const auto id = detail::term(terms, name);
    const auto &term = terms.rows[id];
    if (term.source != react::Source::Setting ||
        (term.kind != react::Kind::Quantity && term.kind != react::Kind::Flag) ||
        term.combine != react::Combine::None)
      throw std::invalid_argument("only authored setting terms may be changed here");
    changed.emplace_back(id, value.is_null() ? std::nullopt
                                             : std::optional{detail::scalar(value, term)});
  }
  return changed;
}
void dirty(step::NativeCompletion &out, Handle object, TermId term) {
  out.effects.ordered.emplace_back(step::WatchWork{{object, term}, {}});
}
} // namespace
step::NativeCompletion set_setting(const step::NativeCall &call,
                                   react::InstanceSettings &instance_settings,
                                   const react::ValueRead &values) {
  try {
    const auto changes = settings(values.terms, call.arguments.at("values"));
    const auto object = call.request.target;
    auto out = detail::result(false);
    for (const auto &[term, value] : changes) {
      const auto before = react::read(values, object, term, call.step, call.hz);
      const auto *existing = term < instance_settings.columns.size()
                                 ? instance_settings.columns[term].get(object.slot())
                                 : nullptr;
      if ((!value && !existing) || (value && existing && *value == *existing))
        continue;
      out.changed = true;
      if (value) {
        if (instance_settings.columns.size() <= term)
          instance_settings.columns.resize(term + 1);
        instance_settings.columns[term].set(object.slot(), *value);
      } else
        instance_settings.columns[term].erase(object.slot());
      if (!(before == react::read(values, object, term, call.step, call.hz)))
        dirty(out, object, term);
    }
    out.effects.changed = out.changed;
    out.result = out.changed ? record::Result::Applied : record::Result::NoEffect;
    out.reply["effect"] = out.changed ? "applied" : "none";
    if (out.changed)
      out.changed_objects.push_back(object);
    return out;
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
step::NativeCompletion model_setting(const step::NativeCall &call,
                                     react::ModelSettings &model_settings,
                                     const react::ValueRead &values,
                                     const catalog::Models &models) {
  try {
    const auto model = detail::model(models, call);
    const auto changes = settings(values.terms, call.arguments.at("values"));
    const auto instances = detail::instances(values.identity, model);
    auto out = detail::result(false);
    for (const auto &[term, value] : changes) {
      auto existing = model_settings.rows.find(model);
      const Value *stored = nullptr;
      if (existing != model_settings.rows.end())
        if (const auto found = existing->second.find(term); found != existing->second.end())
          stored = &found->second;
      if ((!value && !stored) || (value && stored && *stored == *value))
        continue;
      std::vector<react::Answer> before;
      before.reserve(instances.size());
      for (auto object : instances)
        before.push_back(react::read(values, object, term, call.step, call.hz));
      if (value)
        model_settings.rows[model][term] = *value;
      else
        existing->second.erase(term);
      out.changed = true;
      for (std::size_t index = 0; index < instances.size(); ++index)
        if (!(before[index] == react::read(values, instances[index], term, call.step, call.hz)))
          dirty(out, instances[index], term);
    }
    out.effects.changed = out.changed;
    out.result = out.changed ? record::Result::Applied : record::Result::NoEffect;
    out.reply["effect"] = out.changed ? "applied" : "none";
    if (out.changed)
      out.changed_objects = instances;
    if (out.changed)
      out.changed_models.push_back(model);
    return out;
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
step::NativeCompletion patch_state(const step::NativeCall &call, react::Properties &properties,
                                   react::Processes &processes, const react::ValueRead &values,
                                   std::span<const std::string> roles) {
  try {
    detail::player(call);
    if (!detail::role(roles, "architect") && !detail::role(roles, "elaborator"))
      throw detail::PolicyDenied("architect_or_elaborator", to_string(call.request.target),
                                 "state repair requires architect or elaborator role",
                                 "have an architect or elaborator perform this repair");
    std::vector<std::pair<TermId, Value>> changes;
    for (const auto &[name, value] : call.arguments.at("values").items()) {
      const auto id = detail::term(values.terms, name);
      const auto &term = values.terms.rows[id];
      if (term.source != react::Source::State || term.combine != react::Combine::None ||
          (term.kind != react::Kind::Quantity && term.kind != react::Kind::Flag))
        throw std::invalid_argument("only non-shared state may be repaired");
      changes.emplace_back(id, detail::scalar(value, term));
    }
    auto out = detail::result(false);
    for (const auto &[id, value] : changes)
      react::set_state(properties, processes, values.terms.rows[id], call.request.target, value,
                       call.step, call.hz, out.effects);
    out.changed = out.effects.changed;
    out.result = out.changed ? record::Result::Applied : record::Result::NoEffect;
    out.reply["effect"] = out.changed ? "applied" : "none";
    if (out.changed)
      out.changed_objects.push_back(call.request.target);
    return out;
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
step::NativeCompletion relate(const step::NativeCall &call, react::AuthoredRelations &relations,
                              react::RelationsRead &projection, const react::ValueRead &values,
                              std::array<TermId, 4> mechanical_relations) {
  try {
    const auto term = detail::term(values.terms, call.arguments.at("term"));
    if (values.terms.rows[term].kind != react::Kind::Relation)
      throw std::invalid_argument("term is not a relation");
    if (std::find(mechanical_relations.begin(), mechanical_relations.end(), term) !=
        mechanical_relations.end())
      throw detail::PolicyDenied("physical_action", values.terms.rows[term].name,
                                 "mechanical relation is owned by Space",
                                 "perform the checked physical action that changes this relation");
    const auto other = detail::handle(call.arguments.at("other"), values.identity);
    const auto present = call.arguments.at("present").get<bool>();
    if (!present && !relations.contains(term))
      return detail::result(false);
    auto &relation = relations[term];
    projection.columns[term] = &relation;
    auto out = detail::result(false);
    out.changed =
        react::set_relation(relation, term, call.request.target, other, present, out.effects);
    out.result = out.changed ? record::Result::Applied : record::Result::NoEffect;
    out.reply["effect"] = out.changed ? "applied" : "none";
    if (out.changed) {
      out.changed_objects = {call.request.target, other};
    }
    return out;
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
} // namespace amber::step::editing
