#include "common.hpp"

namespace amber::bindings::editing {
namespace {
void registry_editor(const control::Profiles &profiles, const drive::NativeCall &call) {
  const auto player = detail::player(call);
  if (!detail::role(profiles, player, "architect") && !detail::role(profiles, player, "elaborator"))
    throw std::invalid_argument("registry edits require architect or elaborator role");
}
react::Term proposal(const react::Terms &terms, Json data, PlayerId author) {
  const auto short_name = data.at("name").get<std::string>();
  if (short_name.empty() ||
      short_name.find_first_not_of(
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
    throw std::invalid_argument(
        "supply a short unqualified name; namespace is assigned by the server");
  data["name"] = "p" + std::to_string(author) + "." + short_name;
  for (auto field : {"along", "paired", "priority"})
    if (data.contains(field) && data[field] != 0)
      data[field] = detail::term(terms, data[field]);
  if (data.contains("inputs"))
    for (auto &input : data["inputs"])
      if (input.contains("term") && input["term"] != 0)
        input["term"] = detail::term(terms, input["term"]);
  auto result = catalog::term_from_json(data);
  result.owner = author;
  return result;
}
bool equivalent(const react::Term &a, const react::Term &b) {
  return a.kind == b.kind && a.source == b.source && a.combine == b.combine &&
         a.dimensions == b.dimensions;
}
} // namespace
drive::NativeCompletion define_term(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<RegistryEditing *>(local);
  try {
    registry_editor(binding.profiles, call);
    auto term = proposal(binding.terms, call.arguments.at("proposal"), detail::player(call));
    Json duplicates = Json::array();
    for (const auto &existing : binding.terms.rows)
      if (existing.id && equivalent(existing, term))
        duplicates.push_back(catalog::term_json(existing));
    if (!call.arguments.contains("choice"))
      return detail::result(false, {{"proposed", catalog::term_json(term)},
                                    {"duplicates", std::move(duplicates)},
                                    {"requires_choice", true}});
    const auto &choice = call.arguments.at("choice");
    catalog::DuplicateChoice selected;
    if (choice.contains("same_as")) {
      selected.same_as = detail::term(binding.terms, choice.at("same_as"));
      if (!equivalent(binding.terms.rows[selected.same_as], term))
        throw std::invalid_argument("same_as must select a returned equivalent term");
      return detail::result(
          false, {{"term", selected.same_as},
                  {"name", binding.terms.rows[selected.same_as].name},
                  {"definition", catalog::term_json(binding.terms.rows[selected.same_as])},
                  {"same_as", true}});
    }
    selected.none_fits = choice.at("none_fits").get<std::string>();
    if (selected.none_fits.empty())
      throw std::invalid_argument("none_fits needs a distinction");
    if (!binding.commit.publish)
      throw std::runtime_error("registry publication is unavailable");
    const auto id = catalog::append_term(binding.terms, {term}, selected);
    catalog::Hash pin;
    try {
      pin = binding.commit.publish(binding.commit.local, std::span{&id, 1}, {});
    } catch (...) {
      binding.terms.by_name.erase(term.name);
      binding.terms.rows.pop_back();
      throw;
    }
    return detail::result(true, {{"term", id},
                                 {"name", binding.terms.rows[id].name},
                                 {"definition", catalog::term_json(binding.terms.rows[id])},
                                 {"registry", pin},
                                 {"created", true}});
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
drive::NativeCompletion define_material(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<RegistryEditing *>(local);
  try {
    registry_editor(binding.profiles, call);
    const auto material = detail::term(binding.values.terms, call.arguments.at("material"));
    if (binding.values.terms.rows[material].kind != react::Kind::Material)
      throw std::invalid_argument("material must be a registered Material term");
    react::Values after;
    for (const auto &[name, value] : call.arguments.at("values").items()) {
      const auto id = detail::term(binding.values.terms, name);
      const auto &term = binding.values.terms.rows[id];
      if (term.source != react::Source::Intrinsic ||
          (term.kind != react::Kind::Quantity && term.kind != react::Kind::Flag))
        throw std::invalid_argument("material properties must be intrinsic quantities or flags");
      after.emplace(id, detail::scalar(value, term));
    }
    const auto existing = binding.materials.rows.find(material);
    const bool existed = existing != binding.materials.rows.end();
    const auto before = existed ? existing->second : react::Values{};
    if (before == after && existed)
      return detail::result(false);
    if (!binding.commit.publish)
      throw std::runtime_error("registry publication is unavailable");
    std::set<TermId> affected;
    for (const auto &[id, value] : before) {
      (void)value;
      affected.insert(id);
    }
    for (const auto &[id, value] : after) {
      (void)value;
      affected.insert(id);
    }
    if (affected.contains(binding.values.density))
      for (const auto &term : binding.values.terms.rows)
        if (term.kind == react::Kind::Quantity && term.source == react::Source::Measured &&
            term.measurement == react::MeasurementRole::Mass)
          affected.insert(term.id);
    std::vector<Handle> objects;
    std::unordered_map<FactKey, react::Answer, FactKeyHash> old_values;
    for (std::uint32_t slot = 1; slot < binding.values.identity.model.size(); ++slot)
      if (binding.values.identity.model[slot] &&
          binding.values.identity.material[slot] == material) {
        const auto object = binding.values.identity.at(slot);
        objects.push_back(object);
        for (auto id : affected)
          old_values[{object, id}] = react::read(binding.values, object, id, call.step, call.hz);
      }
    catalog::set_material(binding.materials, binding.values.terms, material, after);
    catalog::Hash pin;
    try {
      pin = binding.commit.publish(binding.commit.local, {}, std::span{&material, 1});
    } catch (...) {
      if (existed)
        binding.materials.rows[material] = before;
      else
        binding.materials.rows.erase(material);
      throw;
    }
    auto out = detail::result(true, {{"material", material},
                                     {"name", binding.values.terms.rows[material].name},
                                     {"registry", pin}});
    out.effects.changed = true;
    for (const auto &[fact, old] : old_values)
      if (!(old == react::read(binding.values, fact.subject, fact.term, call.step, call.hz)))
        out.effects.ordered.emplace_back(step::WatchWork{fact, {}});
    metadata_changed(binding.changes, objects);
    return out;
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
} // namespace amber::bindings::editing
