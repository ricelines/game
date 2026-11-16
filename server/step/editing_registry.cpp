#include "editing_private.hpp"

#include <cstdio>
#include <cstdlib>

namespace amber::step::editing {
namespace {
void registry_editor(std::span<const std::string> roles, const step::NativeCall &call) {
  detail::player(call);
  if (!detail::role(roles, "architect") && !detail::role(roles, "elaborator"))
    throw detail::PolicyDenied("architect_or_elaborator", "registry",
                               "registry edits require architect or elaborator role",
                               "have an architect or elaborator make this registry change");
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
catalog::Hash persist_registry(catalog::RegistryDocument &document,
                               const std::filesystem::path &directory, const react::Terms &terms,
                               const react::Materials &materials, const react::RuleVersions &rules,
                               const catalog::MechanicalBindings &mechanical) {
  try {
    const auto hash = catalog::save_registry(directory, terms, materials, rules, mechanical);
    document = catalog::read_registry(directory, hash);
    return hash;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "Fatal registry persistence failure: %s\n", error.what());
    std::abort();
  }
}
step::NativeCompletion define_term(const step::NativeCall &call, std::span<const std::string> roles,
                                   react::Terms &terms, const react::Materials &materials,
                                   const react::RuleVersions &rules,
                                   const catalog::MechanicalBindings &mechanical,
                                   catalog::RegistryDocument &document,
                                   const std::filesystem::path &directory) {
  try {
    registry_editor(roles, call);
    auto term = proposal(terms, call.arguments.at("proposal"), detail::player(call));
    Json duplicates = Json::array();
    for (const auto &existing : terms.rows)
      if (existing.id && equivalent(existing, term))
        duplicates.push_back(catalog::term_json(existing));
    if (!call.arguments.contains("choice"))
      return detail::result(false, {{"proposed", catalog::term_json(term)},
                                    {"duplicates", std::move(duplicates)},
                                    {"requires_choice", true}});
    const auto &choice = call.arguments.at("choice");
    catalog::DuplicateChoice selected;
    if (choice.contains("same_as")) {
      selected.same_as = detail::term(terms, choice.at("same_as"));
      if (!equivalent(terms.rows[selected.same_as], term))
        throw std::invalid_argument("same_as must select a returned equivalent term");
      return detail::result(false,
                            {{"term", selected.same_as},
                             {"name", terms.rows[selected.same_as].name},
                             {"definition", catalog::term_json(terms.rows[selected.same_as])},
                             {"same_as", true}});
    }
    selected.none_fits = choice.at("none_fits").get<std::string>();
    if (selected.none_fits.empty())
      throw std::invalid_argument("none_fits needs a distinction");
    const auto id = catalog::append_term(terms, {term}, selected);
    const auto pin = persist_registry(document, directory, terms, materials, rules, mechanical);
    auto out = detail::result(true, {{"term", id},
                                     {"name", terms.rows[id].name},
                                     {"definition", catalog::term_json(terms.rows[id])},
                                     {"registry", pin},
                                     {"created", true}});
    out.changed_terms.push_back(id);
    return out;
  } catch (const std::exception &error) {
    return detail::failed(error);
  }
}
step::NativeCompletion
define_material(const step::NativeCall &call, std::span<const std::string> roles,
                amber::react::State &react_state, const react::ValueRead &values,
                amber::catalog::State &catalog_state, const GeometryAssets &geometry,
                space::Space &space, PhysicsRoles physical_roles, PhysicsDefaults defaults,
                const std::filesystem::path &directory) {
  auto &materials = react_state.materials;

  auto &rules = react_state.rules;
  auto &mechanical = catalog_state.mechanical;
  auto &document = catalog_state.registry;

  TermId changed_material = 0;
  std::vector<Handle> changed_objects;
  std::unordered_map<FactKey, react::Answer, FactKeyHash> old_values;
  try {
    registry_editor(roles, call);
    const auto material = detail::term(values.terms, call.arguments.at("material"));
    if (values.terms.rows[material].kind != react::Kind::Material)
      throw std::invalid_argument("material must be a registered Material term");
    react::Values after;
    for (const auto &[name, value] : call.arguments.at("values").items()) {
      const auto id = detail::term(values.terms, name);
      const auto &term = values.terms.rows[id];
      if (term.source != react::Source::Intrinsic ||
          (term.kind != react::Kind::Quantity && term.kind != react::Kind::Flag))
        throw std::invalid_argument("material properties must be intrinsic quantities or flags");
      after.emplace(id, detail::scalar(value, term));
    }
    const auto existing = materials.rows.find(material);
    const bool existed = existing != materials.rows.end();
    const auto before = existed ? existing->second : react::Values{};
    if (before == after && existed)
      return detail::result(false);
    std::set<TermId> affected;
    for (const auto &[id, value] : before) {
      (void)value;
      affected.insert(id);
    }
    for (const auto &[id, value] : after) {
      (void)value;
      affected.insert(id);
    }
    if (affected.contains(values.density))
      for (const auto &term : values.terms.rows)
        if (term.kind == react::Kind::Quantity && term.source == react::Source::Measured &&
            term.measurement == react::MeasurementRole::Mass)
          affected.insert(term.id);
    std::vector<Handle> objects;
    for (std::uint32_t slot = 1; slot < values.identity.model.size(); ++slot)
      if (values.identity.model[slot] && values.identity.material[slot] == material) {
        const auto object = values.identity.at(slot);
        objects.push_back(object);
        for (auto id : affected)
          old_values[{object, id}] = react::read(values, object, id, call.step, call.hz);
      }
    catalog::set_material(materials, values.terms, material, after);
    changed_material = material;
    changed_objects = objects;
    const auto pin =
        persist_registry(document, directory, values.terms, materials, rules, mechanical);
    refresh_material(space, geometry, catalog_state, values, physical_roles, defaults,
                     std::span(&material, 1), amber::step::Clock{.step = call.step, .hz = call.hz});
    auto out = detail::result(
        true,
        {{"material", material}, {"name", values.terms.rows[material].name}, {"registry", pin}});
    out.effects.changed = true;
    for (const auto &[fact, old] : old_values)
      if (!(old == react::read(values, fact.subject, fact.term, call.step, call.hz)))
        out.effects.ordered.emplace_back(step::WatchWork{fact, {}});
    out.changed_objects = std::move(objects);
    out.changed_materials.push_back(material);
    return out;
  } catch (const std::exception &error) {
    auto out = detail::failed(error);
    if (changed_material) {
      out.changed = true;
      out.changed_materials.push_back(changed_material);
      out.changed_objects = std::move(changed_objects);
      for (const auto &[fact, before] : old_values) {
        (void)before;
        out.effects.ordered.emplace_back(step::WatchWork{fact, {}});
      }
    }
    return out;
  }
}
} // namespace amber::step::editing
