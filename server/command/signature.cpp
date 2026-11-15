#include "command.hpp"

#include <algorithm>

namespace amber::command {
namespace {
using Json = nlohmann::json;
std::string joined(const std::vector<std::string> &values, std::string_view separator) {
  std::string result;
  for (const auto &value : values) {
    if (!result.empty())
      result += separator;
    result += value;
  }
  return result;
}
std::string schema_type(const Json &schema) {
  if (schema.contains("enum")) {
    std::vector<std::string> choices;
    for (const auto &value : schema["enum"])
      choices.push_back(value.dump());
    return joined(choices, "|");
  }
  if (schema.contains("const"))
    return schema["const"].dump();
  for (auto union_name : {"oneOf", "anyOf", "type"})
    if (schema.contains(union_name) && schema[union_name].is_array()) {
      std::vector<std::string> choices;
      for (const auto &branch : schema[union_name]) {
        auto type = branch.is_string() ? branch.get<std::string>() : schema_type(branch);
        if (std::find(choices.begin(), choices.end(), type) == choices.end())
          choices.push_back(std::move(type));
      }
      return joined(choices, "|");
    }
  const auto type = schema.value("type", std::string("value"));
  return type == "array" && schema.contains("items") ? "array<" + schema_type(schema["items"]) + ">"
                                                     : type;
}
std::string native_signature(const react::Term &action, const Json &schema) {
  if (!schema.contains("properties"))
    return action.name + "()";
  const auto &properties = schema.at("properties");
  const auto required = schema.value("required", std::vector<std::string>{});
  std::vector<std::string> order = required, inputs;
  for (const auto &[name, value] : properties.items()) {
    (void)value;
    if (std::find(order.begin(), order.end(), name) == order.end())
      order.push_back(name);
  }
  for (const auto &name : order) {
    const auto &property = properties.at(name);
    const bool optional = std::find(required.begin(), required.end(), name) == required.end();
    auto value = name + (optional ? "?: " : ": ") + schema_type(property);
    if (property.contains("unit"))
      value += " [" + property.at("unit").get<std::string>() + "]";
    inputs.push_back(std::move(value));
  }
  return action.name + "(" + joined(inputs, ", ") + ")";
}
} // namespace
std::string format_unit(const std::array<std::int8_t, 8> &dimensions) {
  constexpr const char *bases[]{"m", "kg", "s", "A", "K", "mol", "cd", "u7"};
  std::string result;
  for (std::size_t i = 0; i < dimensions.size(); ++i)
    if (dimensions[i]) {
      if (!result.empty())
        result += '*';
      result += bases[i];
      if (dimensions[i] != 1)
        result += '^' + std::to_string(int(dimensions[i]));
    }
  return result.empty() ? "1" : result;
}
std::string action_signature(const react::Terms &terms, const NativeActions &native, TermId id) {
  const auto &action = terms.rows.at(id);
  if (const auto found = native.rows.find(id); found != native.rows.end())
    return native_signature(action, found->second.inputs_schema);
  constexpr const char *types[]{"number", "integer", "boolean", "handle",
                                "vector", "string",  "term"};
  std::vector<std::string> inputs;
  for (const auto &input : action.inputs) {
    auto value =
        input.name + (input.optional ? "?: " : ": ") + types[static_cast<unsigned>(input.type)];
    if (input.term)
      value += " [" + format_unit(terms.rows.at(input.term).dimensions) + "]";
    inputs.push_back(std::move(value));
  }
  return action.name + "(" + joined(inputs, ", ") + ")";
}
} // namespace amber::command
