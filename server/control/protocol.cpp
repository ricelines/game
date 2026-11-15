#include "control.hpp"

#include <algorithm>
#include <initializer_list>

namespace amber::control {
std::string_view mode_name(Mode mode) {
  switch (mode) {
  case Mode::Play:
    return "play";
  case Mode::Watch:
    return "watch";
  case Mode::Debug:
    return "debug";
  }
  throw std::runtime_error("invalid connection mode");
}
namespace {
bool integer(const Json &value, std::uint64_t minimum) {
  if (!value.is_number_integer())
    return false;
  return value.is_number_unsigned() ? value.get<std::uint64_t>() >= minimum
                                    : value.get<std::int64_t>() >= 0 &&
                                          std::uint64_t(value.get<std::int64_t>()) >= minimum;
}
std::string fields(const Json &row, std::initializer_list<std::string_view> required,
                   std::initializer_list<std::string_view> optional = {}) {
  for (auto key : required)
    if (!row.contains(key))
      return std::string(key) + " is required";
  for (auto entry = row.begin(); entry != row.end(); ++entry)
    if (std::find(required.begin(), required.end(), entry.key()) == required.end() &&
        std::find(optional.begin(), optional.end(), entry.key()) == optional.end())
      return "unknown field: " + entry.key();
  return {};
}
std::string string(const Json &row, std::string_view key) {
  return row.contains(key) && !row[key].is_string() ? std::string(key) + " must be a string" : "";
}
std::string number(const Json &row, std::string_view key, std::uint64_t minimum) {
  return row.contains(key) && !integer(row[key], minimum)
             ? std::string(key) + " must be an integer >= " + std::to_string(minimum)
             : "";
}
std::string object(const Json &row, std::string_view key) {
  return row.contains(key) && !row[key].is_object() ? std::string(key) + " must be an object" : "";
}
std::string first(std::initializer_list<std::string> errors) {
  for (const auto &error : errors)
    if (!error.empty())
      return error;
  return {};
}
std::string position(const Json &row, std::string_view key) {
  if (!row.contains(key))
    return {};
  const auto &value = row[key];
  if (!value.is_array() || value.size() != 3)
    return std::string(key) + " must contain three numbers";
  for (const auto &element : value)
    if (!element.is_number() || !std::isfinite(element.get<double>()))
      return std::string(key) + " must contain finite numbers";
  return {};
}
} // namespace
Parsed parse(std::string_view text) {
  Parsed out;
  auto row = Json::parse(text, nullptr, false);
  if (!row.is_object()) {
    out.error = "expected a JSON object";
    return out;
  }
  if (row.contains("id") && integer(row["id"], 1))
    out.id = row["id"].get<std::uint64_t>();
  if (!row.contains("t") || !row["t"].is_string()) {
    out.error = "t must be a string";
    return out;
  }
  const auto operation = row["t"].get<std::string>();
  if (operation == "hello") {
    out.error = first({fields(row, {"t", "version", "key", "mode"}, {"binary"}), string(row, "key"),
                       string(row, "mode")});
    if (!out.error.empty())
      return out;
    if (!integer(row["version"], 1) || row["version"] != 1) {
      out.error = "unsupported protocol version";
      return out;
    }
    if (row.contains("binary") && !row["binary"].is_boolean()) {
      out.error = "binary must be a boolean";
      return out;
    }
    const auto mode = row["mode"].get<std::string>();
    if (mode != "play" && mode != "watch" && mode != "debug") {
      out.error = "mode must be play, watch or debug";
      return out;
    }
    out.message = Hello{row["key"],
                        mode == "play"    ? Mode::Play
                        : mode == "debug" ? Mode::Debug
                                          : Mode::Watch,
                        row.value("binary", true)};
    return out;
  }
  if (!out.id && (operation != "view" || row.contains("id"))) {
    out.error = "id must be a positive integer";
    return out;
  }
  row.erase("t");
  row.erase("id");
  out.message = Request{out.id, operation, std::move(row)};
  return out;
}
std::string validate_look(const Json &row) {
  return first({fields(row, {}, {"limit", "events_limit", "full"}), number(row, "limit", 1),
                number(row, "events_limit", 1),
                row.contains("full") && !row["full"].is_boolean() ? "full must be a boolean" : ""});
}
std::string validate_inspect(const Json &row) {
  return first({fields(row, {"target"}, {"events_limit"}), string(row, "target"),
                number(row, "events_limit", 1)});
}
std::string validate_actions(const Json &row) {
  return first({fields(row, {}, {"target", "name", "page", "events_limit"}), string(row, "target"),
                string(row, "name"), number(row, "page", 0), number(row, "events_limit", 1)});
}
std::string validate_do(const Json &row) {
  return first({fields(row, {"action"}, {"target", "args", "events_limit"}), string(row, "action"),
                string(row, "target"), object(row, "args"), number(row, "events_limit", 1)});
}
std::string validate_cancel(const Json &row) {
  return first({fields(row, {}, {"events_limit"}), number(row, "events_limit", 1)});
}
std::string validate_register_model(const Json &row) {
  auto error =
      first({fields(row, {"job", "asset", "canonical_params", "material", "text", "manifest"},
                    {"variants"}),
             string(row, "job"), string(row, "asset"), string(row, "canonical_params"),
             string(row, "material"), object(row, "text"), object(row, "manifest")});
  if (!error.empty())
    return error;
  if (row.contains("variants")) {
    if (!row["variants"].is_array())
      return "variants must be an array";
    for (const auto &variant : row["variants"]) {
      error = first({fields(variant, {"asset", "canonical_params", "manifest"}),
                     string(variant, "asset"), string(variant, "canonical_params"),
                     object(variant, "manifest")});
      if (!error.empty())
        return error;
    }
  }
  const auto &text = row["text"];
  return first({fields(text, {"name", "description"}, {"writing"}), string(text, "name"),
                string(text, "description"), string(text, "writing")});
}
std::string validate_register_script(const Json &row) {
  return first({fields(row, {"source"}), string(row, "source")});
}
std::string validate_register_variant(const Json &row) {
  return first(
      {fields(row, {"model", "expected_source_hash", "asset", "canonical_params", "manifest"}),
       string(row, "model"), string(row, "expected_source_hash"), string(row, "asset"),
       string(row, "canonical_params"), object(row, "manifest")});
}
std::string validate_authoring_failed(const Json &row) {
  const auto error =
      first({fields(row, {"job", "operation", "source_hash", "errors"}), string(row, "job"),
             string(row, "operation"), string(row, "source_hash")});
  if (!error.empty())
    return error;
  if (row["job"].get_ref<const std::string &>().empty())
    return "job must be nonempty";
  const auto operation = row["operation"].get_ref<const std::string &>();
  if (operation != "model" && operation != "variant" && operation != "edit_model")
    return "operation must be model, variant or edit_model";
  const auto &hash = row["source_hash"].get_ref<const std::string &>();
  if (hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      }))
    return "source_hash must be a SHA256 hex string";
  if (!row["errors"].is_array() || row["errors"].empty() ||
      !std::all_of(row["errors"].begin(), row["errors"].end(),
                   [](const Json &value) { return value.is_string(); }))
    return "errors must be a nonempty string array";
  return {};
}
std::string validate_events(const Json &row) {
  return first(
      {fields(row, {}, {"since", "limit", "root", "thing", "detail"}), number(row, "since", 0),
       number(row, "limit", 1), number(row, "root", 0), string(row, "thing"),
       row.contains("detail") && !row["detail"].is_boolean() ? "detail must be a boolean" : ""});
}
std::string validate_debug(const Json &row) {
  return first({fields(row, {"kind"}, {"around", "radius"}), string(row, "kind"),
                position(row, "around"),
                row.contains("radius") && (!row["radius"].is_number() ||
                                           !std::isfinite(row["radius"].get<double>()) ||
                                           row["radius"].get<double>() < 0)
                    ? "radius must be a nonnegative finite number"
                    : ""});
}
} // namespace amber::control
