#pragma once

#include "store/store.hpp"

#include <array>
#include <bitsery/adapter/buffer.h>
#include <bitsery/bitsery.h>
#include <bitsery/traits/array.h>
#include <bitsery/traits/deque.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>
#include <deque>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_set>

namespace bitsery::traits {
template <> struct ContainerTraits<std::span<const std::uint8_t>> {
  using TValue = std::uint8_t;
  static constexpr bool isResizable = false, isContiguous = true;
  static std::size_t size(std::span<const std::uint8_t> bytes) { return bytes.size(); }
};
template <> struct BufferAdapterTraits<std::span<const std::uint8_t>> {
  using TValue = std::uint8_t;
  using TIterator = const std::uint8_t *;
  using TConstIterator = const std::uint8_t *;
};
} // namespace bitsery::traits

namespace amber::snap {
struct ObjectSelection {
  bool all = true;
  std::unordered_set<std::uint64_t> objects;
  std::unordered_set<std::uint32_t> slots;
  ObjectSelection() = default;
  explicit ObjectSelection(std::span<const Handle> selected) : all(false) {
    for (auto object : selected) {
      objects.insert(object.bits);
      slots.insert(object.slot());
    }
  }
  bool contains(Handle object) const { return all || objects.contains(object.bits); }
};

using Buffer = std::vector<std::uint8_t>;
using Output = bitsery::Serializer<bitsery::OutputBufferAdapter<Buffer>>;
using Input = bitsery::Deserializer<bitsery::InputBufferAdapter<std::span<const std::uint8_t>>>;
using Tag = std::array<char, 4>;
inline constexpr std::array<Tag, 13> tags{{{'O', 'B', 'J', 'S'},
                                           {'P', 'R', 'O', 'P'},
                                           {'R', 'E', 'L', 'S'},
                                           {'S', 'H', 'R', 'D'},
                                           {'T', 'E', 'X', 'T'},
                                           {'P', 'L', 'Y', 'R'},
                                           {'G', 'R', 'N', 'T'},
                                           {'S', 'C', 'S', 'T'},
                                           {'T', 'I', 'M', 'R'},
                                           {'Q', 'U', 'E', 'U'},
                                           {'R', 'E', 'Q', 'S'},
                                           {'R', 'A', 'N', 'D'},
                                           {'M', 'O', 'V', 'E'}}};
struct Header {
  std::string runtime_version;
  Step step = 0;
  std::array<std::uint8_t, 32> registry_hash{};
};
void begin(Output &, const Header &);
std::size_t begin_section(Output &, Tag);
void end_section(Output &, std::size_t length_marker);
void finish(Output &, Buffer &);
struct SectionView {
  Tag tag;
  std::span<const std::uint8_t> bytes;
};
struct View {
  Header header;
  std::vector<SectionView> sections;
};
Input section_input(const View &, Tag);

View parse(std::span<const std::uint8_t>, std::span<const std::string_view> loadable_versions);
Buffer read_file(const std::filesystem::path &);

void check_complete(Input &);

struct WriterStats {
  std::uint64_t completed = 0, source_bytes = 0, compressed_bytes = 0;
  double last_compress_ms = 0, last_write_ms = 0;
};
class Writer {
public:
  Writer(std::filesystem::path directory, std::size_t keep, int compression_level);
  ~Writer();
  Writer(const Writer &) = delete;
  Writer &operator=(const Writer &) = delete;
  std::optional<Buffer> acquire();
  void submit(Buffer, Step);
  void finish();
  WriterStats stats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace amber::snap

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

namespace amber::snap {
// nullopt is the entire world; an empty selection is an empty capture.
void write_objects(Output &, const Objects &,
                   std::optional<std::span<const Handle>> selection = std::nullopt);
void read_objects(Input &, Objects &);
} // namespace amber::snap

namespace amber::lifecycle {
void write_parts(snap::Output &, const Parts &, IdentityRead,
                 std::optional<std::span<const Handle>> selected = {});
void read_parts(snap::Input &, Parts &, IdentityRead, Relation &declared_parts);
} // namespace amber::lifecycle

namespace amber {
template <class S> void serialize(S &s, Origin &v) {
  s.value8b(v.cause);
  s.value8b(v.root);
  snap::field(s, v.authors);
  snap::field(s, v.oldest);
  s.boolValue(v.more);
}
} // namespace amber
