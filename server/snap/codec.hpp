#pragma once

#include "snap.hpp"

#include <bitsery/traits/deque.h>
#include <deque>
#include <stdexcept>

namespace amber {
template <class S> void serialize(S &s, Handle &v) {
  s.value8b(v.bits);
}
template <class S> void serialize(S &s, Vec3 &v) {
  s.value4b(v.x);
  s.value4b(v.y);
  s.value4b(v.z);
}
template <class S> void serialize(S &s, Quat &v) {
  s.value4b(v.x);
  s.value4b(v.y);
  s.value4b(v.z);
  s.value4b(v.w);
}
template <class S> void serialize(S &s, Value &v) {
  s.value1b(v.kind);
  switch (v.kind) {
  case Value::Kind::None:
    break;
  case Value::Kind::Number:
    s.value8b(v.number);
    break;
  case Value::Kind::Integer:
    s.value8b(v.integer);
    break;
  case Value::Kind::Boolean:
    s.boolValue(v.boolean);
    break;
  case Value::Kind::Object:
    s.object(v.object);
    break;
  default:
    throw std::runtime_error("invalid snapshot value kind");
  }
}
} // namespace amber

namespace amber::snap {
inline constexpr std::size_t max_collection() {
  return std::numeric_limits<std::uint32_t>::max();
}
template <class T, class F> void write_sparse(Output &s, const SparseColumn<T> &column, F field) {
  std::size_t index = 0;
  s.container(column.value, max_collection(), [&](Output &out, T &value) {
    out.value4b(column.slot_of[index++]);
    field(out, value);
  });
}
template <class T, class F> void read_sparse(Input &s, SparseColumn<T> &column, F field) {
  column.clear();
  s.container(column.value, max_collection(), [&](Input &in, T &value) {
    std::uint32_t slot = 0;
    in.value4b(slot);
    column.slot_of.push_back(slot);
    field(in, value);
  });
}
template <class S, class T> void object_field(S &s, T &value) {
  s.object(value);
}

template <class T> inline constexpr bool variant_type = false;
template <class... T> inline constexpr bool variant_type<std::variant<T...>> = true;
template <class T> inline constexpr bool optional_type = false;
template <class T> inline constexpr bool optional_type<std::optional<T>> = true;
template <class T> inline constexpr bool sequence_type = false;
template <class T> inline constexpr bool sequence_type<std::vector<T>> = true;
template <class T> inline constexpr bool sequence_type<std::deque<T>> = true;
template <class S, class T> void field(S &, T &);
template <std::size_t I = 0, class S, class T>
void variant_field(S &s, T &value, std::uint8_t index) {
  if constexpr (I == std::variant_size_v<T>)
    throw std::runtime_error("invalid snapshot variant tag");
  else if (index == I) {
    if constexpr (std::is_same_v<S, Input>)
      value.template emplace<I>();
    field(s, std::get<I>(value));
  } else
    variant_field<I + 1>(s, value, index);
}
template <class S, class T> void field(S &s, T &value) {
  if constexpr (std::is_same_v<T, std::string>)
    s.text1b(value, max_collection());
  else if constexpr (std::is_same_v<T, bool>)
    s.boolValue(value);
  else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
    s.template value<sizeof(T)>(value);
  else if constexpr (variant_type<T>) {
    auto index = static_cast<std::uint8_t>(value.index());
    s.value1b(index);
    variant_field(s, value, index);
  } else if constexpr (optional_type<T>) {
    bool present = value.has_value();
    s.boolValue(present);
    if constexpr (std::is_same_v<S, Input>) {
      if (present)
        value.emplace();
      else
        value.reset();
    }
    if (present)
      field(s, *value);
  } else if constexpr (sequence_type<T>) {
    s.container(value, max_collection(), [](S &in, auto &item) { field(in, item); });
  } else
    s.object(value);
}
} // namespace amber::snap

namespace amber {
template <class S> void serialize(S &s, TermRef &v) {
  s.value4b(v.id);
}
template <class S> void serialize(S &s, EventField &v) {
  snap::field(s, v.name);
  snap::field(s, v.value);
}
template <class S> void serialize(S &s, FactKey &v) {
  s.object(v.subject);
  s.value4b(v.term);
}
template <class S> void serialize(S &s, ScriptKey &v) {
  s.object(v.object);
  s.value4b(v.script);
}
template <class S> void serialize(S &s, ScriptHolder &v) {
  s.value4b(v.script);
  s.object(v.object);
}
template <class S> void serialize(S &s, ActionRequest &v) {
  snap::field(s, v.caller);
  s.value4b(v.action);
  s.object(v.target);
  snap::field(s, v.args);
}
} // namespace amber
