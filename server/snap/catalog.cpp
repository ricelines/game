#include "snap.hpp"

#include <algorithm>

namespace amber::catalog {
namespace {
template <class T> bool chosen(const std::optional<std::span<const T>> &selected, T id) {
  return !selected || std::find(selected->begin(), selected->end(), id) != selected->end();
}
template <class S> void text(S &s, Text &value) {
  snap::field(s, value.name);
  snap::field(s, value.description);
  snap::field(s, value.writing);
}
template <class S> void authored_text(S &s, AuthoredText &value) {
  snap::field(s, value.id);
  snap::field(s, value.target);
  snap::field(s, value.author);
  snap::field(s, value.created_s);
  snap::field(s, value.kind);
  snap::field(s, value.text);
}
template <class S> void model(S &s, Model &value) {
  snap::field(s, value.id);
  snap::field(s, value.asset);
  snap::field(s, value.source_hash);
  snap::field(s, value.part_name);
  snap::field(s, value.canonical_params);
  snap::field(s, value.material);
  snap::field(s, value.author);
  snap::field(s, value.group);
  snap::field(s, value.created_s);
  snap::field(s, value.script);
  std::vector<std::pair<std::string, ModelId>> parts;
  if constexpr (std::is_same_v<S, snap::Output>)
    for (const auto &entry : value.parts)
      parts.push_back(entry);
  s.container(parts, snap::max_collection(), [](S &stream, auto &entry) {
    snap::field(stream, entry.first);
    snap::field(stream, entry.second);
  });
  if constexpr (std::is_same_v<S, snap::Input>) {
    value.parts.clear();
    for (auto &entry : parts)
      value.parts.emplace(std::move(entry.first), entry.second);
  }
}
template <class S> void parameter(S &s, ParameterBlock &value) {
  snap::field(s, value.id);
  snap::field(s, value.model);
  snap::field(s, value.asset);
  snap::field(s, value.canonical_params);
  std::vector<std::pair<std::string, ModelId>> parts;
  if constexpr (std::is_same_v<S, snap::Output>)
    for (const auto &entry : value.parts)
      parts.push_back(entry);
  s.container(parts, snap::max_collection(), [](S &stream, auto &entry) {
    snap::field(stream, entry.first);
    snap::field(stream, entry.second);
  });
  if constexpr (std::is_same_v<S, snap::Input>) {
    value.parts.clear();
    for (auto &entry : parts)
      value.parts.emplace(std::move(entry.first), entry.second);
  }
}
template <class Rows, class Selection, class Field>
void write_rows(snap::Output &out, const Rows &rows, Selection selected, Field field) {
  auto size = static_cast<std::uint32_t>(rows.size());
  out.value4b(size);
  std::vector<typename Rows::value_type> included;
  for (const auto &row : rows)
    if (row.id && chosen(selected, row.id))
      included.push_back(row);
  out.container(included, snap::max_collection(), field);
}
template <class Rows, class Field> void read_rows(snap::Input &in, Rows &rows, Field field) {
  std::uint32_t size = 0;
  in.value4b(size);
  rows.clear();
  rows.resize(size);
  std::vector<typename Rows::value_type> included;
  in.container(included, snap::max_collection(), field);
  for (auto &row : included) {
    if (!row.id || row.id >= size || rows[row.id].id)
      throw std::runtime_error("invalid catalog snapshot identity");
    rows[row.id] = std::move(row);
  }
}
} // namespace
void write_models(snap::Output &out, const Models &models, ModelSelection selected) {
  write_rows(out, models.rows, selected, model<snap::Output>);
}
void read_models(snap::Input &in, Models &models) {
  read_rows(in, models.rows, model<snap::Input>);
}
void write_parameters(snap::Output &out, const ParameterBlocks &parameters,
                      ParameterSelection selected) {
  write_rows(out, parameters.rows, selected, parameter<snap::Output>);
}
void read_parameters(snap::Input &in, ParameterBlocks &parameters) {
  read_rows(in, parameters.rows, parameter<snap::Input>);
}
void write_text(snap::Output &out, const ModelTexts &models, const InstanceTexts &instances,
                ModelSelection selected_models, ObjectSelection selected_objects) {
  std::vector<std::pair<ModelId, Text>> model_rows;
  for (const auto &[id, value] : models.rows)
    if (chosen(selected_models, id))
      model_rows.emplace_back(id, value);
  out.container(model_rows, snap::max_collection(), [](snap::Output &s, auto &entry) {
    snap::field(s, entry.first);
    text(s, entry.second);
  });
  std::vector<std::pair<std::uint64_t, Text>> instance_rows;
  for (const auto &[id, value] : instances.rows)
    if (chosen(selected_objects, Handle{id}))
      instance_rows.emplace_back(id, value);
  out.container(instance_rows, snap::max_collection(), [](snap::Output &s, auto &entry) {
    snap::field(s, entry.first);
    text(s, entry.second);
  });
}
void read_text(snap::Input &in, ModelTexts &models, InstanceTexts &instances) {
  models.rows.clear();
  instances.rows.clear();
  std::vector<std::pair<ModelId, Text>> model_rows;
  std::vector<std::pair<std::uint64_t, Text>> instance_rows;
  in.container(model_rows, snap::max_collection(), [](snap::Input &s, auto &entry) {
    snap::field(s, entry.first);
    text(s, entry.second);
  });
  in.container(instance_rows, snap::max_collection(), [](snap::Input &s, auto &entry) {
    snap::field(s, entry.first);
    text(s, entry.second);
  });
  for (auto &entry : model_rows)
    models.rows.emplace(entry.first, std::move(entry.second));
  for (auto &entry : instance_rows)
    instances.rows.emplace(entry.first, std::move(entry.second));
}
void write_authored_texts(snap::Output &out, const AuthoredTexts &texts, ObjectSelection selected) {
  auto size = static_cast<std::uint32_t>(texts.rows.size());
  out.value4b(size);
  std::vector<AuthoredText> included;
  for (const auto &row : texts.rows)
    if (row.id && chosen(selected, row.target))
      included.push_back(row);
  out.container(included, snap::max_collection(), authored_text<snap::Output>);
}
void read_authored_texts(snap::Input &in, AuthoredTexts &texts) {
  read_rows(in, texts.rows, authored_text<snap::Input>);
}
} // namespace amber::catalog
