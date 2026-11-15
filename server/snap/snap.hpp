#pragma once

#include "store/store.hpp"

#include <array>
#include <bitsery/adapter/buffer.h>
#include <bitsery/bitsery.h>
#include <bitsery/traits/array.h>
#include <bitsery/traits/string.h>
#include <bitsery/traits/vector.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
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
void section(Output &, Tag, const std::function<void(Output &)> &);
void finish(Output &, Buffer &);
struct SectionView {
  Tag tag;
  std::span<const std::uint8_t> bytes;
};
struct View {
  Header header;
  std::vector<SectionView> sections;
};
using SectionWriters = std::array<std::function<void(Output &)>, 13>;
using SectionReaders = std::array<std::function<void(Input &)>, 13>;
void encode(Buffer &, const Header &, const SectionWriters &);
void decode(const View &, const SectionReaders &);
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
