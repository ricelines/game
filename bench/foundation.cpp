#include "record/record.hpp"
#include "store/store.hpp"

#include <chrono>
#include <iostream>

int main() {
  using Clock = std::chrono::steady_clock;
  constexpr std::uint32_t slots = 100000;
  constexpr std::uint32_t queries = 10000000;
  amber::SparseColumn<double> column;
  for (std::uint32_t slot = 0; slot < slots; slot += 2)
    column.set(slot, static_cast<double>(slot));
  double checksum = 0;
  std::uint32_t random = 17;
  const auto lookup_start = Clock::now();
  for (std::uint32_t query = 0; query < queries; ++query) {
    random = random * 1664525 + 1013904223;
    if (const auto *value = column.get(random % slots))
      checksum += *value;
  }
  const auto lookup_elapsed = Clock::now() - lookup_start;

  constexpr std::size_t batches = 2000;
  constexpr std::size_t batch_size = 50;
  amber::record::Journal journal(8, {1});
  const auto path = std::filesystem::current_path() / "foundation-benchmark.jsonl.zst";
  amber::record::Writer writer(path, {{"run", "foundation-benchmark"}, {"spec", 1}}, 128, 3);
  double append_seconds = 0;
  const auto record_start = Clock::now();
  for (std::size_t batch = 0; batch < batches; ++batch) {
    const auto append_start = Clock::now();
    for (std::size_t index = 0; index < batch_size; ++index) {
      amber::record::Event event;
      event.step = batch;
      event.kind = amber::record::Kind::Action;
      event.actor = amber::record::Actor::player(2 + index % 8);
      event.name = "benchmark.change";
      event.changed = true;
      event.result = amber::record::Result::Applied;
      event.things = {amber::Handle::make(1 + index, 1)};
      journal.append(std::move(event));
    }
    append_seconds += std::chrono::duration<double>(Clock::now() - append_start).count();
    writer.submit(journal.take());
  }
  writer.finish();
  const auto total_seconds = std::chrono::duration<double>(Clock::now() - record_start).count();
  const auto statistics = writer.stats();
  nlohmann::json report = {
      {"sparse_slots", slots},
      {"sparse_queries", queries},
      {"sparse_lookup_ns",
       std::chrono::duration<double, std::nano>(lookup_elapsed).count() / queries},
      {"checksum", checksum},
      {"record_events", batches * batch_size},
      {"record_append_ns", append_seconds * 1e9 / (batches * batch_size)},
      {"record_total_s", total_seconds},
      {"record_source_bytes", statistics.source_bytes},
      {"record_compressed_bytes", statistics.compressed_bytes},
      {"record_queue_high_water", statistics.queue_high_water},
      {"record_source_MiB_s", statistics.source_bytes / total_seconds / (1024 * 1024)}};
  std::cout << report.dump(2) << '\n';
  std::filesystem::remove(path);
}
