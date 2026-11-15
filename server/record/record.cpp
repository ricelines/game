#include "record.hpp"

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <zstd.h>

namespace amber::record {
namespace {
const char *name(Kind kind) {
  switch (kind) {
  case Kind::Action:
    return "action";
  case Kind::Handler:
    return "handler";
  case Kind::Gap:
    return "gap";
  case Kind::Denial:
    return "denial";
  case Kind::Speech:
    return "speech";
  case Kind::Observation:
    return "observation";
  case Kind::Create:
    return "create";
  case Kind::Destroy:
    return "destroy";
  case Kind::Registry:
    return "registry";
  case Kind::System:
    return "system";
  }
  std::terminate();
}

const char *name(Result result) {
  switch (result) {
  case Result::None:
    return "";
  case Result::Applied:
    return "applied";
  case Result::NoEffect:
    return "no_effect";
  case Result::NoHandler:
    return "no_handler";
  case Result::NotModeled:
    return "not_modeled";
  case Result::Denied:
    return "denied";
  }
  std::terminate();
}

std::string name(Actor actor) {
  switch (actor.kind) {
  case Actor::Kind::System:
    return "system";
  case Actor::Kind::Player:
    return "p:" + std::to_string(actor.id);
  case Actor::Kind::Script:
    return "s:" + std::to_string(actor.id);
  }
  std::terminate();
}

void check_zstd(std::size_t result) {
  if (ZSTD_isError(result))
    throw std::runtime_error(ZSTD_getErrorName(result));
}
} // namespace

Journal::Journal(std::size_t authors_max, std::vector<PlayerId> seed_actors)
    : authors_max_(authors_max), seed_actors_(std::move(seed_actors)) {
  std::sort(seed_actors_.begin(), seed_actors_.end());
}

Origin Journal::append(Event event, const Origin &inherited, std::span<const Credit> credits) {
  const auto id = next_id_++;
  Origin origin = inherited;
  origin.cause = id;
  origin.root = inherited.root ? inherited.root : id;
  auto add_author = [&](PlayerId author) {
    if (author == 0 || std::binary_search(seed_actors_.begin(), seed_actors_.end(), author) ||
        std::find(origin.authors.begin(), origin.authors.end(), author) != origin.authors.end())
      return;
    if (origin.authors.size() < authors_max_)
      origin.authors.push_back(author);
    else
      origin.more = true;
  };
  if (event.actor.kind == Actor::Kind::Player)
    add_author(event.actor.id);
  if (event.script_author)
    add_author(*event.script_author);
  auto include_credit = [&](const Credit &credit) {
    if (credit.seed || std::binary_search(seed_actors_.begin(), seed_actors_.end(), credit.author))
      return;
    add_author(credit.author);
    if (credit.created_s && (!origin.oldest || *credit.created_s < *origin.oldest))
      origin.oldest = credit.created_s;
  };
  for (const auto &credit : credits)
    include_credit(credit);
  for (const auto &credit : event.credits)
    include_credit(credit);
  nlohmann::json value = {
      {"id", id},
      {"step", event.step},
      {"kind", name(event.kind)},
      {"actor", name(event.actor)},
      {"name", event.name},
      {"changed", event.changed},
      {"cause", inherited.cause ? nlohmann::json(inherited.cause) : nlohmann::json(nullptr)},
      {"root", origin.root},
      {"authors", nlohmann::json::array()},
      {"oldest", origin.oldest ? nlohmann::json(*origin.oldest) : nlohmann::json(nullptr)},
      {"things", nlohmann::json::array()}};
  for (const auto author : origin.authors)
    value["authors"].push_back("p:" + std::to_string(author));
  for (const auto thing : event.things)
    value["things"].push_back(to_string(thing));
  if (origin.more)
    value["more"] = true;
  if (event.script_author)
    value["author"] = "p:" + std::to_string(*event.script_author);
  if (event.result != Result::None)
    value["result"] = name(event.result);
  for (auto &[key, detail] : event.detail.items()) {
    assert(!value.contains(key));
    value.emplace(key, std::move(detail));
  }
  pending_.push_back(std::move(value));
  return origin;
}

std::vector<nlohmann::json> Journal::take() {
  std::vector<nlohmann::json> result;
  result.swap(pending_);
  return result;
}

void Journal::restore_next_id(std::uint64_t next_id) {
  assert(pending_.empty() && next_id != 0);
  next_id_ = next_id;
}

struct Writer::Impl {
  std::ofstream output;
  std::vector<std::vector<nlohmann::json>> ring;
  std::size_t read = 0;
  std::size_t write = 0;
  std::size_t queued = 0;
  int level;
  bool closing = false;
  std::exception_ptr failure;
  WriterStats counters;
  mutable std::mutex mutex;
  std::condition_variable readable;
  std::condition_variable writable;
  std::thread thread;

  Impl(const std::filesystem::path &path, nlohmann::json header, std::size_t capacity,
       int compression)
      : ring(capacity), level(compression) {
    if (capacity == 0)
      throw std::invalid_argument("event writer queue must hold at least one batch");
    output.open(path, std::ios::binary | std::ios::trunc);
    if (!output)
      throw std::runtime_error("cannot open event record: " + path.string());
    thread = std::thread([this, header = std::move(header)] { run(header); });
  }

  ~Impl() { stop(); }

  void stop() {
    {
      std::lock_guard lock(mutex);
      closing = true;
    }
    readable.notify_one();
    writable.notify_one();
    if (thread.joinable())
      thread.join();
  }

  void run(const nlohmann::json &header) {
    struct ContextDeleter {
      void operator()(ZSTD_CCtx *context) const { ZSTD_freeCCtx(context); }
    };
    try {
      const std::unique_ptr<ZSTD_CCtx, ContextDeleter> context(ZSTD_createCCtx());
      if (!context)
        throw std::bad_alloc();
      check_zstd(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_compressionLevel, level));
      std::vector<char> compressed(ZSTD_CStreamOutSize());
      WriterStats totals;
      auto compress = [&](std::string_view text, ZSTD_EndDirective end) {
        ZSTD_inBuffer input{text.data(), text.size(), 0};
        std::size_t remaining;
        do {
          ZSTD_outBuffer buffer{compressed.data(), compressed.size(), 0};
          remaining = ZSTD_compressStream2(context.get(), &buffer, &input, end);
          check_zstd(remaining);
          output.write(compressed.data(), static_cast<std::streamsize>(buffer.pos));
          if (!output)
            throw std::runtime_error("event record write failed");
          totals.compressed_bytes += buffer.pos;
        } while (input.pos != input.size || (end != ZSTD_e_continue && remaining != 0));
        totals.source_bytes += text.size();
      };
      compress(header.dump() + '\n', ZSTD_e_flush);
      for (;;) {
        std::vector<nlohmann::json> batch;
        {
          std::unique_lock lock(mutex);
          readable.wait(lock, [&] { return queued != 0 || closing; });
          if (queued == 0)
            break;
          batch.swap(ring[read]);
          read = (read + 1) % ring.size();
          --queued;
        }
        writable.notify_one();
        for (const auto &event : batch) {
          compress(event.dump() + '\n', ZSTD_e_continue);
          ++totals.records;
        }
        compress({}, ZSTD_e_flush);
        output.flush();
        if (!output)
          throw std::runtime_error("event record flush failed");
        {
          std::lock_guard lock(mutex);
          counters.records = totals.records;
          counters.source_bytes = totals.source_bytes;
          counters.compressed_bytes = totals.compressed_bytes;
        }
      }
      compress({}, ZSTD_e_end);
      output.close();
      if (!output)
        throw std::runtime_error("event record close failed");
      std::lock_guard lock(mutex);
      counters.records = totals.records;
      counters.source_bytes = totals.source_bytes;
      counters.compressed_bytes = totals.compressed_bytes;
    } catch (...) {
      {
        std::lock_guard lock(mutex);
        failure = std::current_exception();
      }
      writable.notify_one();
    }
  }
};

Writer::Writer(const std::filesystem::path &path, nlohmann::json header, std::size_t queue_batches,
               int compression_level)
    : impl_(std::make_unique<Impl>(path, std::move(header), queue_batches, compression_level)) {}
Writer::~Writer() = default;

void Writer::submit(std::vector<nlohmann::json> batch) {
  if (batch.empty())
    return;
  std::unique_lock lock(impl_->mutex);
  impl_->writable.wait(lock, [&] {
    return impl_->queued != impl_->ring.size() || impl_->closing || impl_->failure;
  });
  if (impl_->failure)
    std::rethrow_exception(impl_->failure);
  if (impl_->closing)
    throw std::logic_error("submit after event writer finish");
  impl_->ring[impl_->write] = std::move(batch);
  impl_->write = (impl_->write + 1) % impl_->ring.size();
  ++impl_->queued;
  impl_->counters.queue_high_water = std::max(impl_->counters.queue_high_water, impl_->queued);
  lock.unlock();
  impl_->readable.notify_one();
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

} // namespace amber::record
