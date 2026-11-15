#include "snap.hpp"

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#define XXH_INLINE_ALL
#include <common/xxhash.h>
#include <zstd.h>

namespace amber::snap {
namespace {
constexpr std::array<char, 8> magic{'A', 'M', 'B', 'S', 'N', 'A', 'P', '1'};
void zcheck(std::size_t code) {
  if (ZSTD_isError(code))
    throw std::runtime_error(ZSTD_getErrorName(code));
}
template <class T> T little(std::span<const std::uint8_t> bytes, std::size_t &offset) {
  if (bytes.size() - offset < sizeof(T))
    throw std::runtime_error("truncated snapshot");
  T result = 0;
  for (std::size_t i = 0; i != sizeof(T); ++i)
    result |= static_cast<T>(bytes[offset++]) << (8 * i);
  return result;
}
} // namespace
void begin(Output &out, const Header &header) {
  out.container1b(magic);
  out.value4b(std::uint32_t{1});
  out.text1b(header.runtime_version, std::numeric_limits<std::uint32_t>::max());
  out.value8b(header.step);
  out.container1b(header.registry_hash);
  out.value4b(static_cast<std::uint32_t>(tags.size()));
}
std::size_t begin_section(Output &out, Tag tag) {
  out.container1b(tag);
  const auto length_at = out.adapter().currentWritePos();
  out.value8b(std::uint64_t{0});
  return length_at;
}
void end_section(Output &out, std::size_t length_at) {
  const auto end = out.adapter().currentWritePos();
  out.adapter().currentWritePos(length_at);
  out.value8b(static_cast<std::uint64_t>(end - length_at - sizeof(std::uint64_t)));
  out.adapter().currentWritePos(end);
}
void finish(Output &out, Buffer &buffer) {
  const auto end = out.adapter().currentWritePos();
  out.value8b(XXH64(buffer.data(), end, 0));
  buffer.resize(out.adapter().writtenBytesCount());
}
Input section_input(const View &view, Tag tag) {
  const auto found = std::find_if(view.sections.begin(), view.sections.end(),
                                  [tag](const SectionView &section) { return section.tag == tag; });
  if (found == view.sections.end())
    throw std::runtime_error("missing snapshot section");
  return Input{found->bytes.data(), found->bytes.size()};
}
View parse(std::span<const std::uint8_t> bytes, std::span<const std::string_view> loadable) {
  if (bytes.size() < magic.size() + 4 + 1 + 8 + 32 + 4 + 8)
    throw std::runtime_error("truncated snapshot header");
  if (!std::equal(magic.begin(), magic.end(), bytes.begin()))
    throw std::runtime_error("invalid snapshot magic");
  std::size_t checksum_at = bytes.size() - 8;
  const auto expected = little<std::uint64_t>(bytes, checksum_at);
  if (XXH64(bytes.data(), bytes.size() - 8, 0) != expected)
    throw std::runtime_error("snapshot checksum mismatch");
  Input in{bytes.data(), bytes.size() - 8};
  in.adapter().currentReadPos(magic.size());
  std::uint32_t format = 0, count = 0;
  in.value4b(format);
  if (format != 1)
    throw std::runtime_error("unsupported snapshot format");
  View view;
  in.text1b(view.header.runtime_version, bytes.size());
  in.value8b(view.header.step);
  in.container1b(view.header.registry_hash);
  in.value4b(count);
  if (in.adapter().error() != bitsery::ReaderError::NoError)
    throw std::runtime_error("invalid snapshot header");
  if (std::find(loadable.begin(), loadable.end(), view.header.runtime_version) == loadable.end())
    throw std::runtime_error("snapshot runtime is not loadable: " + view.header.runtime_version);
  if (count != tags.size())
    throw std::runtime_error("snapshot section count mismatch");
  auto offset = in.adapter().currentReadPos();
  const auto body = bytes.first(bytes.size() - 8);
  for (const auto &tag : tags) {
    if (body.size() - offset < 12 || !std::equal(tag.begin(), tag.end(), body.begin() + offset))
      throw std::runtime_error("snapshot section order mismatch");
    offset += tag.size();
    const auto length = little<std::uint64_t>(body, offset);
    if (length > body.size() - offset)
      throw std::runtime_error("snapshot section length exceeds file");
    view.sections.push_back({tag, body.subspan(offset, length)});
    offset += length;
  }
  if (offset != body.size())
    throw std::runtime_error("snapshot has trailing bytes");
  return view;
}
void check_complete(Input &in) {
  if (in.adapter().error() != bitsery::ReaderError::NoError ||
      !in.adapter().isCompletedSuccessfully())
    throw std::runtime_error("invalid or incomplete snapshot section");
}
Buffer read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input)
    throw std::runtime_error("cannot open snapshot: " + path.string());
  const auto size = input.tellg();
  if (size < 0)
    throw std::runtime_error("cannot size snapshot");
  Buffer compressed(static_cast<std::size_t>(size));
  input.seekg(0);
  input.read(reinterpret_cast<char *>(compressed.data()), size);
  if (!input)
    throw std::runtime_error("cannot read snapshot");
  const auto decoded = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
  if (decoded == ZSTD_CONTENTSIZE_ERROR || decoded == ZSTD_CONTENTSIZE_UNKNOWN ||
      decoded > std::numeric_limits<std::size_t>::max())
    throw std::runtime_error("snapshot must be a sized zstd frame");
  Buffer buffer(decoded);
  const auto written =
      ZSTD_decompress(buffer.data(), buffer.size(), compressed.data(), compressed.size());
  zcheck(written);
  if (written != buffer.size())
    throw std::runtime_error("snapshot decompressed length mismatch");
  return buffer;
}
struct Writer::Impl {
  struct Job {
    Buffer buffer;
    Step step;
  };
  std::filesystem::path directory;
  std::size_t keep;
  int level;
  mutable std::mutex mutex;
  std::condition_variable ready;
  std::deque<Buffer> free{Buffer{}, Buffer{}};
  std::deque<Job> jobs;
  std::thread worker;
  std::exception_ptr failure;
  bool closing = false;
  WriterStats counters;
  Impl(std::filesystem::path path, std::size_t retention, int compression)
      : directory(std::move(path)), keep(retention), level(compression) {
    if (!keep)
      throw std::invalid_argument("snapshot retention must be positive");
    std::filesystem::create_directories(directory);
    worker = std::thread([this] { run(); });
  }
  ~Impl() { stop(); }
  void stop() {
    {
      std::lock_guard lock(mutex);
      closing = true;
    }
    ready.notify_one();
    if (worker.joinable())
      worker.join();
  }
  void prune() {
    std::vector<std::pair<Step, std::filesystem::path>> snapshots;
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
      const auto name = entry.path().filename().string();
      if (!name.starts_with("snap-") || !name.ends_with(".amb.zst"))
        continue;
      const auto number = name.substr(5, name.size() - 13);
      Step step = 0;
      const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), step);
      if (error == std::errc{} && end == number.data() + number.size())
        snapshots.emplace_back(step, entry.path());
    }
    std::sort(snapshots.begin(), snapshots.end());
    for (std::size_t i = keep; i < snapshots.size(); ++i)
      std::filesystem::remove(snapshots[i - keep].second);
  }
  void run() {
    try {
      Buffer compressed;
      for (;;) {
        Job job;
        {
          std::unique_lock lock(mutex);
          ready.wait(lock, [&] { return closing || !jobs.empty(); });
          if (jobs.empty())
            return;
          job = std::move(jobs.front());
          jobs.pop_front();
        }
        const auto start = std::chrono::steady_clock::now();
        ZoneScopedN("snapshot compression and file publication");
        ZoneValue(job.buffer.size());
        compressed.resize(ZSTD_compressBound(job.buffer.size()));
        const auto size = ZSTD_compress(compressed.data(), compressed.size(), job.buffer.data(),
                                        job.buffer.size(), level);
        zcheck(size);
        const auto compressed_at = std::chrono::steady_clock::now();
        const auto final = directory / ("snap-" + std::to_string(job.step) + ".amb.zst");
        const auto temporary = final.string() + ".tmp";
        {
          std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
          out.write(reinterpret_cast<const char *>(compressed.data()), size);
          out.close();
          if (!out)
            throw std::runtime_error("cannot write snapshot: " + temporary);
        }
        std::filesystem::rename(temporary, final);
        prune();
        const auto end = std::chrono::steady_clock::now();
        std::lock_guard lock(mutex);
        ++counters.completed;
        counters.source_bytes += job.buffer.size();
        counters.compressed_bytes += size;
        counters.last_compress_ms =
            std::chrono::duration<double, std::milli>(compressed_at - start).count();
        counters.last_write_ms =
            std::chrono::duration<double, std::milli>(end - compressed_at).count();
        job.buffer.clear();
        free.push_back(std::move(job.buffer));
      }
    } catch (...) {
      std::lock_guard lock(mutex);
      failure = std::current_exception();
    }
  }
};
Writer::Writer(std::filesystem::path path, std::size_t keep, int level)
    : impl_(std::make_unique<Impl>(std::move(path), keep, level)) {}
Writer::~Writer() = default;
std::optional<Buffer> Writer::acquire() {
  std::lock_guard lock(impl_->mutex);
  if (impl_->failure)
    std::rethrow_exception(impl_->failure);
  if (impl_->free.empty())
    return {};
  auto buffer = std::move(impl_->free.front());
  impl_->free.pop_front();
  return buffer;
}
void Writer::submit(Buffer buffer, Step step) {
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->failure)
      std::rethrow_exception(impl_->failure);
    assert(!impl_->closing);
    impl_->jobs.push_back({std::move(buffer), step});
  }
  impl_->ready.notify_one();
}
void Writer::finish() {
  impl_->stop();
  std::lock_guard lock(impl_->mutex);
  if (impl_->failure)
    std::rethrow_exception(impl_->failure);
}
WriterStats Writer::stats() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->counters;
}
} // namespace amber::snap
