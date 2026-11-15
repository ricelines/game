#pragma once

#include "store/store.hpp"

#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>

namespace amber::catalog {
struct Models;
struct Scripts;
} // namespace amber::catalog

namespace amber::record {

// Carried by queued work and timers; no later record lookup is needed.

std::vector<Credit> credits(IdentityRead, AttributionRead, const catalog::Models &,
                            const catalog::Scripts &, std::span<const Handle>, ScriptId, double hz);

// The writer receives completed event values, never game tables.
class Journal {
public:
  Journal(std::size_t authors_max, std::vector<PlayerId> seed_actors);
  Origin append(Event event, const Origin &inherited = {}, std::span<const Credit> credits = {});
  // Nested actions need their cause's id before the handler returns. The row
  // stays in the normal step buffer and receives its outcome before take().
  Origin finish(Origin, Result result, bool changed,
                nlohmann::json detail = nlohmann::json::object(),
                std::span<const Credit> credits = {});
  std::vector<nlohmann::json> take();
  std::uint64_t next_id() const { return next_id_; }
  void restore_next_id(std::uint64_t next_id);
  void include_credits(Origin &, std::span<const Credit>) const;

private:
  void include_author(Origin &, PlayerId) const;
  std::uint64_t next_id_ = 1;
  std::size_t authors_max_;
  std::vector<PlayerId> seed_actors_;
  std::vector<nlohmann::json> pending_;
};

struct WriterStats {
  std::uint64_t records = 0;
  std::uint64_t source_bytes = 0;
  std::uint64_t compressed_bytes = 0;
  std::size_t queue_high_water = 0;
};

class Writer {
public:
  Writer(const std::filesystem::path &path, nlohmann::json header, std::size_t queue_batches,
         int compression_level);
  ~Writer();
  Writer(const Writer &) = delete;
  Writer &operator=(const Writer &) = delete;
  void submit(std::vector<nlohmann::json> batch);
  void finish();
  WriterStats stats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace amber::record
