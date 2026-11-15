#include "elaborate/elaborate.hpp"

#include <chrono>
#include <iostream>

using namespace amber;
using namespace amber::elaborate;
int main() {
  constexpr std::uint32_t iterations = 100000;
  Requests table;
  GapCounts counts;
  LastGaps last;
  GapInput input{1, {2, 3}, Handle::make(1, 1), 0, 0, {}, {}};
  auto start = std::chrono::steady_clock::now();
  for (std::uint32_t i = 0; i < iterations; ++i)
    note_gap(counts, last, table, input, 0);
  auto elapsed =
      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
  std::cout << "gap_count_existing_pair iterations=" << iterations
            << " ns/op=" << elapsed / iterations << " count=" << counts.rows.at(input.gap) << '\n';
  for (std::uint32_t i = 0; i < 10000; ++i) {
    Request row;
    row.id = static_cast<RequestId>(table.rows.size());
    row.gap = {i % 97, 3};
    row.created_step = i;
    row.requester = i % 4 == 0 ? 1 : 0;
    row.note = std::string(256, 'n');
    table.rows.push_back(std::move(row));
    counts.rows[{i % 97, 3}] = i % 100;
  }
  start = std::chrono::steady_clock::now();
  std::size_t seen = 0;
  for (unsigned i = 0; i < 20; ++i)
    seen += requests(table, counts, {1, true}, i, 20).size();
  elapsed =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::cout << "request_order_page history=10000 pages=20 ms/page=" << elapsed / 20
            << " returned=" << seen << '\n';
  const auto directory = std::filesystem::current_path() / "elaborate-bench-captures";
  std::vector<std::uint8_t> bytes(100000);
  std::uint32_t random = 1;
  for (auto &byte : bytes) {
    random = random * 1664525 + 1013904223;
    byte = static_cast<std::uint8_t>(random >> 24);
  }
  CaptureWriter writer(directory, 3);
  start = std::chrono::steady_clock::now();
  auto hash = writer.submit(std::move(bytes));
  auto submitted = std::chrono::steady_clock::now();
  writer.finish();
  auto completed = std::chrono::steady_clock::now();
  std::cout << "capture_completed_bytes bytes=100000 hash_enqueue_ms="
            << std::chrono::duration<double, std::milli>(submitted - start).count()
            << " off_step_compress_write_ms="
            << std::chrono::duration<double, std::milli>(completed - submitted).count()
            << " completed=" << writer.take().size() << " hash=" << hash.substr(0, 8) << '\n';
  std::filesystem::remove_all(directory);
}
