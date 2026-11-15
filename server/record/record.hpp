#pragma once

#include "store/store.hpp"

#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>

namespace amber::record {

// Carried by queued work and timers; no later record lookup is needed.
struct Origin {
  std::uint64_t cause = 0;
  std::uint64_t root = 0;
  std::vector<PlayerId> authors;
  std::optional<double> oldest;
  bool more = false;
  bool operator==(const Origin &) const = default;
};

struct Credit {
  PlayerId author = 0;
  std::optional<double> created_s;
  bool seed = false;
};

enum class Kind {
  Action,
  Handler,
  Gap,
  Denial,
  Speech,
  Observation,
  Create,
  Destroy,
  Registry,
  System
};
enum class Result { None, Applied, NoEffect, NoHandler, NotModeled, Denied };

struct Actor {
  enum class Kind { System, Player, Script } kind = Kind::System;
  std::uint32_t id = 0;
  static Actor player(PlayerId id) { return {Kind::Player, id}; }
  static Actor script(ScriptId id) { return {Kind::Script, id}; }
};

struct Event {
  Step step = 0;
  Kind kind = Kind::System;
  Actor actor;
  std::string name;
  Result result = Result::None;
  bool changed = false;
  std::vector<Handle> things;
  std::optional<PlayerId> script_author;
  nlohmann::json detail = nlohmann::json::object();
  std::vector<Credit> credits{};
};

// The writer receives completed event values, never game tables.
class Journal {
public:
  Journal(std::size_t authors_max, std::vector<PlayerId> seed_actors);
  Origin append(Event event, const Origin &inherited = {}, std::span<const Credit> credits = {});
  std::vector<nlohmann::json> take();
  std::uint64_t next_id() const { return next_id_; }
  void restore_next_id(std::uint64_t next_id);

private:
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
