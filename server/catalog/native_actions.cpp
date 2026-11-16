#include "catalog.hpp"

namespace amber::catalog {
TermId declare_native(react::NativeActions &native, react::Terms &terms, std::string name,
                      std::string description, std::string right, bool reach,
                      nlohmann::json inputs_schema, react::NativeOperation operation) {
  react::Term expected;
  expected.name = std::move(name);
  expected.kind = react::Kind::Action;
  expected.description = std::move(description);
  TermId id;
  if (const auto existing = terms.by_name.find(expected.name); existing != terms.by_name.end()) {
    id = existing->second;
    auto actual = catalog::term_json(terms.rows.at(id));
    auto desired = catalog::term_json(expected);
    for (auto key : {"id", "compared", "distinction"}) {
      actual.erase(key);
      desired.erase(key);
    }
    if (actual != desired)
      throw std::invalid_argument("reserved native action metadata mismatch: " + expected.name);
  } else
    id = catalog::append_term(
        terms, {expected},
        {0, "Reserved native administration operation with the described input schema."});
  native.rows[id] = {id, std::move(right), reach, std::move(inputs_schema), operation};
  return id;
}
} // namespace amber::catalog
