#include "common.hpp"

namespace amber::bindings::editing {
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
void dirty(drive::NativeCompletion &out, Handle object, TermId term) {
  out.effects.ordered.emplace_back(step::WatchWork{{object, term}, {}});
}
} // namespace
drive::NativeCompletion set_setting(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<PropertyEditing *>(local);
  try {
    detail::edit(binding.access, call);
    const auto changes = settings(binding.values.terms, call.arguments.at("values"));
    const auto object = call.request.target;
    auto out = detail::result(false);
    for (const auto &[term, value] : changes) {
      const auto before = react::read(binding.values, object, term, call.step, call.hz);
      const auto *existing = term < binding.instance_settings.columns.size()
                                 ? binding.instance_settings.columns[term].get(object.slot())
                                 : nullptr;
      if ((!value && !existing) || (value && existing && *value == *existing))
        continue;
      out.changed = true;
      if (value) {
        if (binding.instance_settings.columns.size() <= term)
          binding.instance_settings.columns.resize(term + 1);
        binding.instance_settings.columns[term].set(object.slot(), *value);
      } else
        binding.instance_settings.columns[term].erase(object.slot());
      if (!(before == react::read(binding.values, object, term, call.step, call.hz)))
        dirty(out, object, term);
    }
    out.effects.changed = out.changed;
    out.result = out.changed ? record::Result::Applied : record::Result::NoEffect;
    out.reply["effect"] = out.changed ? "applied" : "none";
    if (out.changed)
      metadata_changed(binding.changes, std::span{&object, 1});
    return out;
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
drive::NativeCompletion model_setting(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<PropertyEditing *>(local);
  try {
    const auto model = detail::model(binding.models, call);
    const auto changes = settings(binding.values.terms, call.arguments.at("values"));
    const auto instances = detail::instances(binding.values.identity, model);
    auto out = detail::result(false);
    for (const auto &[term, value] : changes) {
      auto existing = binding.model_settings.rows.find(model);
      const Value *stored = nullptr;
      if (existing != binding.model_settings.rows.end())
        if (const auto found = existing->second.find(term); found != existing->second.end())
          stored = &found->second;
      if ((!value && !stored) || (value && stored && *stored == *value))
        continue;
      std::vector<react::Answer> before;
      before.reserve(instances.size());
      for (auto object : instances)
        before.push_back(react::read(binding.values, object, term, call.step, call.hz));
      if (value)
        binding.model_settings.rows[model][term] = *value;
      else
        existing->second.erase(term);
      out.changed = true;
      for (std::size_t index = 0; index < instances.size(); ++index)
        if (!(before[index] ==
              react::read(binding.values, instances[index], term, call.step, call.hz)))
          dirty(out, instances[index], term);
    }
    out.effects.changed = out.changed;
    out.result = out.changed ? record::Result::Applied : record::Result::NoEffect;
    out.reply["effect"] = out.changed ? "applied" : "none";
    if (out.changed)
      metadata_changed(binding.changes, instances, std::span{&model, 1});
    return out;
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
drive::NativeCompletion patch_state(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<PropertyEditing *>(local);
  try {
    const auto player = detail::player(call);
    if (!detail::role(binding.profiles, player, "architect") &&
        !detail::role(binding.profiles, player, "elaborator"))
      throw std::invalid_argument("state repair requires architect or elaborator role");
    detail::edit(binding.access, call);
    std::vector<std::pair<TermId, Value>> changes;
    for (const auto &[name, value] : call.arguments.at("values").items()) {
      const auto id = detail::term(binding.values.terms, name);
      const auto &term = binding.values.terms.rows[id];
      if (term.source != react::Source::State || term.combine != react::Combine::None ||
          (term.kind != react::Kind::Quantity && term.kind != react::Kind::Flag))
        throw std::invalid_argument("only non-shared state may be repaired");
      changes.emplace_back(id, detail::scalar(value, term));
    }
    auto out = detail::result(false);
    for (const auto &[id, value] : changes)
      react::set_state(binding.properties, binding.processes, binding.values.terms.rows[id],
                       call.request.target, value, call.step, call.hz, out.effects);
    out.changed = out.effects.changed;
    out.result = out.changed ? record::Result::Applied : record::Result::NoEffect;
    out.reply["effect"] = out.changed ? "applied" : "none";
    if (out.changed)
      metadata_changed(binding.changes, std::span{&call.request.target, 1});
    return out;
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
drive::NativeCompletion relate(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<PropertyEditing *>(local);
  try {
    detail::edit(binding.access, call);
    const auto term = detail::term(binding.values.terms, call.arguments.at("term"));
    if (binding.values.terms.rows[term].kind != react::Kind::Relation)
      throw std::invalid_argument("term is not a relation");
    if (std::find(binding.mechanical_relations.begin(), binding.mechanical_relations.end(), term) !=
        binding.mechanical_relations.end())
      throw std::invalid_argument("mechanical relation is owned by Space");
    const auto other = detail::handle(call.arguments.at("other"), binding.access.identity);
    const auto present = call.arguments.at("present").get<bool>();
    if (!present && !binding.relations.contains(term))
      return detail::result(false);
    auto &relation = binding.relations[term];
    binding.projection.columns[term] = &relation;
    auto out = detail::result(false);
    out.changed =
        react::set_relation(relation, term, call.request.target, other, present, out.effects);
    out.result = out.changed ? record::Result::Applied : record::Result::NoEffect;
    out.reply["effect"] = out.changed ? "applied" : "none";
    if (out.changed) {
      const std::array objects{call.request.target, other};
      metadata_changed(binding.changes, objects);
    }
    return out;
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
} // namespace amber::bindings::editing
