#include "step.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace amber::step {
namespace {
bool matches_type(const Json &value, const Json &type) {
  if (type == "object")
    return value.is_object();
  if (type == "array")
    return value.is_array();
  if (type == "string")
    return value.is_string();
  if (type == "number")
    return value.is_number();
  if (type == "integer")
    return value.is_number_integer();
  if (type == "boolean")
    return value.is_boolean();
  return type == "null" && value.is_null();
}
bool less(const Json &a, const Json &b) {
  if (a.is_number_integer() && b.is_number_integer()) {
    if (a.is_number_unsigned())
      return b.is_number_unsigned() ? a.get<std::uint64_t>() < b.get<std::uint64_t>()
                                    : std::cmp_less(a.get<std::uint64_t>(), b.get<std::int64_t>());
    return b.is_number_unsigned() ? std::cmp_less(a.get<std::int64_t>(), b.get<std::uint64_t>())
                                  : a.get<std::int64_t>() < b.get<std::int64_t>();
  }
  return a.get<double>() < b.get<double>();
}
std::string check(const Json &schema, const Json &value, const std::string &path) {
  if (const auto type = schema.find("type"); type != schema.end()) {
    const bool matched =
        type->is_array()
            ? std::ranges::any_of(
                  *type, [&](const auto &alternative) { return matches_type(value, alternative); })
            : matches_type(value, *type);
    if (!matched)
      return path + " must have type " + type->dump();
  }
  if (value.is_number()) {
    if (!std::isfinite(value.get<double>()))
      return path + " must be finite";
    if (const auto bound = schema.find("minimum"); bound != schema.end() && less(value, *bound))
      return path + " must be >= " + bound->dump();
    if (const auto bound = schema.find("maximum"); bound != schema.end() && less(*bound, value))
      return path + " must be <= " + bound->dump();
  }
  if (const auto choices = schema.find("enum");
      choices != schema.end() && std::ranges::find(*choices, value) == choices->end())
    return path + " must be one of " + choices->dump();
  if (value.is_object()) {
    if (const auto required = schema.find("required"); required != schema.end())
      for (const auto &name : *required)
        if (!value.contains(name.get_ref<const std::string &>()))
          return path + "." + name.get_ref<const std::string &>() + " is required";
    const auto properties = schema.find("properties");
    const auto additional = schema.find("additionalProperties");
    for (const auto &[name, item] : value.items()) {
      const Json *input = nullptr;
      if (properties != schema.end()) {
        const auto found = properties->find(name);
        if (found != properties->end())
          input = &*found;
      }
      if (!input && additional != schema.end()) {
        if (*additional == false)
          return path + "." + name + " is not an allowed input";
        if (additional->is_object())
          input = &*additional;
      }
      if (input)
        if (auto error = check(*input, item, path + "." + name); !error.empty())
          return error;
    }
  }
  if (value.is_array()) {
    if (const auto bound = schema.find("minItems");
        bound != schema.end() && value.size() < bound->get<std::size_t>())
      return path + " must contain at least " + bound->dump() + " items";
    if (const auto bound = schema.find("maxItems");
        bound != schema.end() && value.size() > bound->get<std::size_t>())
      return path + " must contain at most " + bound->dump() + " items";
    if (const auto items = schema.find("items"); items != schema.end())
      for (std::size_t i = 0; i < value.size(); ++i)
        if (auto error = check(*items, value[i], path + "[" + std::to_string(i) + "]");
            !error.empty())
          return error;
  }
  for (const auto *rule : {"oneOf", "anyOf"}) {
    if (const auto alternatives = schema.find(rule); alternatives != schema.end()) {
      unsigned matches = 0;
      const bool one = std::string_view(rule) == "oneOf";
      for (const auto &alternative : *alternatives)
        if (check(alternative, value, path).empty()) {
          ++matches;
          if (!one)
            break;
        }
      if (matches != 1)
        return path + (one ? " must match exactly one allowed input form"
                           : " must match an allowed input form");
    }
  }
  return {};
}
} // namespace
std::string check_input(const Json &schema, const Json &arguments) {
  return check(schema, arguments, "arguments");
}
} // namespace amber::step
