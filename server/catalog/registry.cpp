#include "catalog.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>

namespace amber::catalog {
namespace {
template <class E, std::size_t N>
std::string name(E value, const std::array<const char *, N> &names) {
  return names.at(static_cast<std::size_t>(value));
}
template <class E, std::size_t N>
E enumeration(const std::string &value, const std::array<const char *, N> &names) {
  auto it = std::find(names.begin(), names.end(), value);
  if (it == names.end())
    throw std::runtime_error("unknown registry enum: " + value);
  return static_cast<E>(it - names.begin());
}
constexpr std::array<const char *, 6> kinds{"quantity", "flag",   "event",
                                            "relation", "action", "material"};
constexpr std::array<const char *, 4> sources{"intrinsic", "measured", "state", "setting"};
constexpr std::array<const char *, 6> combines{"none", "sum",         "max",
                                               "any",  "share_equal", "share_priority"};
constexpr std::array<const char *, 4> measurements{"baked", "volume", "mass", "runtime"};
constexpr std::array<const char *, 7> inputs{"number", "integer", "boolean", "handle",
                                             "vector", "string",  "term"};
nlohmann::json encoded(Value value) {
  switch (value.kind) {
  case Value::Kind::Number:
    return {{"kind", "number"}, {"value", value.number}};
  case Value::Kind::Integer:
    return {{"kind", "integer"}, {"value", value.integer}};
  case Value::Kind::Boolean:
    return {{"kind", "boolean"}, {"value", value.boolean}};
  case Value::Kind::Object:
    return {{"kind", "handle"}, {"value", value.object.bits}};
  case Value::Kind::None:
    throw std::runtime_error("registry value cannot be absent");
  }
  throw std::runtime_error("invalid value kind");
}
Value decoded(const nlohmann::json &value) {
  if (value.is_boolean())
    return Value(value.get<bool>());
  if (value.is_number_integer())
    return Value(value.get<std::int64_t>());
  if (value.is_number())
    return Value(value.get<double>());
  const auto kind = value.at("kind").get<std::string>();
  const auto &data = value.at("value");
  if (kind == "number")
    return Value(data.get<double>());
  if (kind == "integer")
    return Value(data.get<std::int64_t>());
  if (kind == "boolean")
    return Value(data.get<bool>());
  if (kind == "handle")
    return Value(Handle{data.get<std::uint64_t>()});
  throw std::runtime_error("invalid registry value kind");
}
} // namespace
nlohmann::json term_json(const react::Term &term) {
  nlohmann::json result = {{"id", term.id},
                           {"name", term.name},
                           {"kind", name(term.kind, kinds)},
                           {"source", name(term.source, sources)},
                           {"dimensions", term.dimensions},
                           {"conserved", term.conserved},
                           {"combine", name(term.combine, combines)},
                           {"along", term.along},
                           {"paired", term.paired},
                           {"priority", term.priority},
                           {"owner", term.owner},
                           {"description", term.description},
                           {"measurement", name(term.measurement, measurements)},
                           {"compared", term.compared},
                           {"distinction", term.distinction}};
  result["inputs"] = nlohmann::json::array();
  for (const auto &input : term.inputs)
    result["inputs"].push_back({{"name", input.name},
                                {"type", name(input.type, inputs)},
                                {"term", input.term},
                                {"optional", input.optional}});
  return result;
}
react::Term term_from_json(const nlohmann::json &data) {
  react::Term term;
  term.id = data.value("id", TermId{});
  term.name = data.at("name").get<std::string>();
  term.kind = enumeration<react::Kind>(data.at("kind"), kinds);
  term.source = enumeration<react::Source>(data.value("source", std::string("state")), sources);
  if (data.contains("dimensions")) {
    const auto &dims = data["dimensions"];
    if (!dims.is_array() || dims.size() != 8)
      throw std::runtime_error("term requires eight SI dimension exponents");
    for (std::size_t i = 0; i < 8; ++i) {
      const auto value = dims[i].get<int>();
      if (value < -128 || value > 127)
        throw std::runtime_error("dimension exponent outside int8 range");
      term.dimensions[i] = static_cast<std::int8_t>(value);
    }
  }
  term.conserved = data.value("conserved", false);
  term.combine = enumeration<react::Combine>(data.value("combine", std::string("none")), combines);
  term.along = data.value("along", TermId{});
  term.paired = data.value("paired", TermId{});
  term.priority = data.value("priority", TermId{});
  term.owner = data.value("owner", PlayerId{});
  term.description = data.value("description", std::string{});
  term.measurement = enumeration<react::MeasurementRole>(
      data.value("measurement", std::string("baked")), measurements);
  term.compared = data.value("compared", std::vector<TermId>{});
  term.distinction = data.value("distinction", std::string{});
  if (data.contains("inputs"))
    for (const auto &input : data["inputs"])
      term.inputs.push_back({input.at("name"),
                             enumeration<react::InputType>(input.at("type"), inputs),
                             input.value("term", TermId{}), input.value("optional", false)});
  return term;
}
nlohmann::json declaration_json(const react::ScriptDeclaration &declaration) {
  nlohmann::json result = {
      {"id", declaration.id},
      {"author", declaration.author},
      {"version", declaration.version},
      {"kind", declaration.kind == react::ScriptDeclaration::Kind::Model      ? "model"
               : declaration.kind == react::ScriptDeclaration::Kind::Instance ? "instance"
               : declaration.kind == react::ScriptDeclaration::Kind::World    ? "world"
                                                                              : "rule"},
      {"approved", declaration.approved},
      {"active", declaration.active},
      {"contributes", declaration.contributes},
      {"sets", declaration.sets},
      {"events", declaration.events},
      {"handlers", declaration.handlers},
      {"slots", declaration.slots},
      {"selector", declaration.selector},
      {"tests", declaration.tests}};
  result["watches"] = nlohmann::json::array();
  for (const auto &watch : declaration.watches) {
    nlohmann::json value = {{"term", watch.term}, {"thresholds", nlohmann::json::array()}};
    for (const auto &threshold : watch.thresholds)
      value["thresholds"].push_back(
          {{"predicate", static_cast<unsigned>(threshold.predicate)}, {"value", threshold.value}});
    result["watches"].push_back(std::move(value));
  }
  result["actions"] = nlohmann::json::array();
  for (const auto &action : declaration.actions)
    result["actions"].push_back({{"term", action.action},
                                 {"right", action.right},
                                 {"function", action.function},
                                 {"reach", action.reach}});
  return result;
}
react::ScriptDeclaration declaration_from_json(const nlohmann::json &data) {
  react::ScriptDeclaration d;
  const auto kind = data.at("kind").get<std::string>();
  if (kind == "model")
    d.kind = react::ScriptDeclaration::Kind::Model;
  else if (kind == "instance")
    d.kind = react::ScriptDeclaration::Kind::Instance;
  else if (kind == "rule")
    d.kind = react::ScriptDeclaration::Kind::Rule;
  else if (kind == "world")
    d.kind = react::ScriptDeclaration::Kind::World;
  else
    throw std::runtime_error("unknown script kind");
  d.id = data.value("id", ScriptId{});
  d.author = data.value("author", PlayerId{});
  d.version = data.value("version", 1U);
  d.approved = data.value("approved", false);
  d.active = data.value("active", false);
  d.contributes = data.value("contributes", std::vector<TermId>{});
  d.sets = data.value("sets", std::vector<TermId>{});
  d.events = data.value("events", std::vector<TermId>{});
  d.handlers = data.value("handlers", std::vector<TermId>{});
  d.slots = data.value("slots", std::vector<std::string>{});
  d.selector = data.value("selector", std::string{});
  d.tests = data.value("tests", std::size_t{});
  if (data.contains("watches"))
    for (const auto &watch : data["watches"]) {
      react::WatchDeclaration w;
      w.term = watch.at("term");
      for (const auto &threshold : watch.at("thresholds")) {
        const auto p = threshold.at("predicate").get<unsigned>();
        if (p > 3)
          throw std::runtime_error("invalid threshold predicate");
        w.thresholds.push_back(
            {static_cast<react::Threshold::Predicate>(p), threshold.at("value").get<double>()});
      }
      d.watches.push_back(std::move(w));
    }
  if (data.contains("actions"))
    for (const auto &action : data["actions"])
      d.actions.push_back({action.at("term"), action.at("right"), action.at("function"),
                           action.value("reach", false)});
  return d;
}
TermId append_term(react::Terms &terms, const TermProposal &proposal,
                   const DuplicateChoice &choice) {
  auto term = proposal.term;
  if (term.name.find('.') == std::string::npos || terms.by_name.contains(term.name))
    throw std::runtime_error("term needs a new qualified name");
  if (term.kind != react::Kind::Quantity && term.kind != react::Kind::Flag &&
      term.combine != react::Combine::None)
    throw std::runtime_error("only quantity/flag terms combine");
  if (term.along &&
      (term.along >= terms.rows.size() || terms.rows[term.along].kind != react::Kind::Relation))
    throw std::runtime_error("along requires a relation term");
  if (term.conserved &&
      (term.kind != react::Kind::Quantity || term.source != react::Source::State ||
       term.combine != react::Combine::None))
    throw std::runtime_error("conserved transfer requires a non-shared state quantity");
  if (term.paired &&
      (term.paired >= terms.rows.size() || terms.rows[term.paired].kind != react::Kind::Quantity ||
       terms.rows[term.paired].dimensions != term.dimensions))
    throw std::runtime_error("paired requires a compatible quantity term");
  const bool sharing =
      term.combine == react::Combine::ShareEqual || term.combine == react::Combine::SharePriority;
  if (sharing) {
    if (term.kind != react::Kind::Quantity || !term.paired ||
        terms.rows[term.paired].combine == react::Combine::None ||
        terms.rows[term.paired].combine == react::Combine::ShareEqual ||
        terms.rows[term.paired].combine == react::Combine::SharePriority)
      throw std::runtime_error("shared demand requires a compatible combined supply");
    for (const auto &row : terms.rows)
      if ((row.combine == react::Combine::ShareEqual ||
           row.combine == react::Combine::SharePriority) &&
          row.paired == term.paired && row.id != choice.same_as)
        throw std::runtime_error("one supply term pairs with one demand term");
  }
  if (term.priority &&
      (term.combine != react::Combine::SharePriority || term.priority >= terms.rows.size() ||
       terms.rows[term.priority].kind != react::Kind::Quantity ||
       terms.rows[term.priority].source != react::Source::Setting))
    throw std::runtime_error("priority requires a quantity setting on priority sharing");
  if (term.kind != react::Kind::Action && !term.inputs.empty())
    throw std::runtime_error("only actions have inputs");
  std::set<std::string> names;
  for (const auto &input : term.inputs) {
    if (input.name.empty() || !names.insert(input.name).second)
      throw std::runtime_error("action inputs require unique nonempty names");
    if (input.term &&
        (input.term >= terms.rows.size() || terms.rows[input.term].kind != react::Kind::Quantity))
      throw std::runtime_error("input units require a quantity term");
  }
  term.compared.clear();
  bool duplicate = false;
  for (const auto &row : terms.rows)
    if (row.id && row.kind == term.kind && row.dimensions == term.dimensions &&
        row.combine == term.combine && row.source == term.source) {
      duplicate = true;
      term.compared.push_back(row.id);
      if (row.id == choice.same_as)
        return row.id;
    }
  if (choice.same_as)
    throw std::runtime_error("same_as is not an equivalent term");
  if (duplicate && choice.none_fits.empty())
    throw std::runtime_error("duplicate comparison needs same_as or none_fits");
  term.distinction = choice.none_fits;
  term.id = static_cast<TermId>(terms.rows.size());
  terms.by_name.emplace(term.name, term.id);
  terms.rows.push_back(std::move(term));
  return terms.rows.back().id;
}
void set_material(react::Materials &materials, const react::Terms &terms, TermId material,
                  const react::Values &values) {
  if (!material || material >= terms.rows.size() ||
      terms.rows[material].kind != react::Kind::Material)
    throw std::runtime_error("unknown material term");
  for (const auto &[id, value] : values)
    if (!id || id >= terms.rows.size() || terms.rows[id].source != react::Source::Intrinsic ||
        !value.is_numeric())
      throw std::runtime_error("material values require intrinsic numeric terms");
  materials.rows[material] = values;
}
Hash save_registry(const std::filesystem::path &root, const react::Terms &terms,
                   const react::Materials &materials, const react::RuleVersions &rules,
                   const MechanicalBindings &bindings) {
  nlohmann::json data = {{"version", 1},
                         {"terms", nlohmann::json::array()},
                         {"materials", nlohmann::json::array()},
                         {"rules", nlohmann::json::array()},
                         {"mechanical", bindings.values}};
  for (const auto &term : terms.rows)
    if (term.id)
      data["terms"].push_back(term_json(term));
  std::vector<TermId> material_ids;
  for (const auto &[id, values] : materials.rows) {
    (void)values;
    material_ids.push_back(id);
  }
  std::sort(material_ids.begin(), material_ids.end());
  for (auto id : material_ids) {
    nlohmann::json values = nlohmann::json::array();
    for (const auto &[term, value] : materials.rows.at(id))
      values.push_back({{"term", term}, {"value", encoded(value)}});
    data["materials"].push_back({{"id", id}, {"values", std::move(values)}});
  }
  for (const auto &rule : rules.history) {
    const auto &h = rule.handler;
    data["rules"].push_back({{"script", h.script},
                             {"function", h.function},
                             {"author", h.author},
                             {"version", h.version},
                             {"enabled", h.enabled},
                             {"trigger", rule.trigger},
                             {"selector", rule.selector},
                             {"flag_selector", rule.flag_selector}});
  }
  const auto serialized = data.dump();
  const auto hash = sha256(serialized);
  std::filesystem::create_directories(root);
  const auto path = root / (hash + ".json");
  if (!std::filesystem::exists(path)) {
    const auto temporary = root / (hash + ".tmp");
    {
      std::ofstream file(temporary, std::ios::binary);
      file << serialized;
      if (!file)
        throw std::runtime_error("registry write failed");
    }
    std::filesystem::rename(temporary, path);
  }
  return hash;
}
RegistryDocument read_registry(const std::filesystem::path &root, Hash hash) {
  if (!valid_hash(hash))
    throw std::runtime_error("invalid registry hash");
  std::ifstream file(root / (hash + ".json"), std::ios::binary);
  if (!file)
    throw std::runtime_error("registry missing");
  std::string text{std::istreambuf_iterator<char>(file), {}};
  if (sha256(text) != hash)
    throw std::runtime_error("registry digest mismatch");
  auto data = nlohmann::json::parse(text);
  if (data.at("version") != 1)
    throw std::runtime_error("unsupported registry version");
  return {std::move(hash), std::move(data)};
}
void load_terms(const RegistryDocument &document, react::Terms &terms) {
  react::Terms result;
  for (const auto &value : document.data.at("terms")) {
    auto term = term_from_json(value);
    if (term.id != result.rows.size() || result.by_name.contains(term.name))
      throw std::runtime_error("registry term IDs/names are not monotonic unique");
    result.by_name.emplace(term.name, term.id);
    result.rows.push_back(std::move(term));
  }
  for (const auto &term : result.rows)
    if (term.along &&
        (term.along >= result.rows.size() || result.rows[term.along].kind != react::Kind::Relation))
      throw std::runtime_error("invalid registry along reference");
  terms = std::move(result);
}
void load_materials(const RegistryDocument &document, const react::Terms &terms,
                    react::Materials &materials) {
  react::Materials result;
  for (const auto &material : document.data.at("materials")) {
    react::Values values;
    for (const auto &entry : material.at("values"))
      values.emplace(entry.at("term").get<TermId>(), decoded(entry.at("value")));
    set_material(result, terms, material.at("id"), values);
  }
  materials = std::move(result);
}
void load_rules(const RegistryDocument &document, const Scripts &scripts,
                react::RuleVersions &rules) {
  react::RuleVersions result;
  for (const auto &value : document.data.at("rules")) {
    const auto id = value.at("script").get<ScriptId>();
    if (!id || id >= scripts.rows.size() ||
        scripts.rows[id].declaration.kind != react::ScriptDeclaration::Kind::Rule)
      throw std::runtime_error("unknown rule script");
    react::HandlerRef h{id, value.at("function"), value.at("author"), value.at("version"),
                        value.at("enabled")};
    result.history.push_back(
        {h, value.at("trigger"), value.at("selector"), value.at("flag_selector")});
  }
  rules = std::move(result);
}
} // namespace amber::catalog
