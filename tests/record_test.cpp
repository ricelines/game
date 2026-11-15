#include "record/record.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <zstd.h>

using namespace amber;

namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

std::string decompress(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(file)), {});
  auto *context = ZSTD_createDCtx();
  std::vector<char> buffer(ZSTD_DStreamOutSize());
  ZSTD_inBuffer input{bytes.data(), bytes.size(), 0};
  std::string text;
  std::size_t remaining = 1;
  while (input.pos < input.size) {
    ZSTD_outBuffer output{buffer.data(), buffer.size(), 0};
    remaining = ZSTD_decompressStream(context, &output, &input);
    if (ZSTD_isError(remaining)) {
      ZSTD_freeDCtx(context);
      throw std::runtime_error("invalid compressed record");
    }
    text.append(buffer.data(), output.pos);
  }
  ZSTD_freeDCtx(context);
  require(remaining == 0, "record has no complete zstd frame");
  return text;
}

void causes_are_carried_without_history_and_seed_is_excluded() {
  record::Journal journal(3, {1});
  record::Event action;
  action.step = 100;
  action.kind = record::Kind::Action;
  action.actor = record::Actor::player(2);
  action.name = "example.switch";
  action.result = record::Result::Applied;
  action.changed = true;
  action.things = {Handle::make(4, 1)};
  const std::vector<record::Credit> credits = {{1, 0, true}, {1, 0.5, false}, {3, 2.5, false}};
  const auto cause = journal.append(action, {}, credits);
  journal.take(); // The original record is no longer retained by the producer.
  record::Event timer;
  timer.step = 700;
  timer.kind = record::Kind::Handler;
  timer.actor = record::Actor::script(12);
  timer.script_author = 4;
  timer.name = "example.due";
  timer.result = record::Result::Applied;
  timer.changed = true;
  const std::vector<record::Credit> later = {{5, 9.5, false}};
  const auto descendant = journal.append(timer, cause, later);
  const auto records = journal.take();
  require(descendant.root == 1 && descendant.cause == 2, "origin IDs were not advanced");
  require(descendant.authors == std::vector<PlayerId>({2, 3, 4}) && descendant.more,
          "seed removal or author overflow failed");
  require(descendant.oldest == 2.5, "seed credit affected age or descendant lost oldest object");
  require(records[0]["cause"] == 1 && records[0]["root"] == 1, "record lost causal origin");
  require(records[0]["authors"] == nlohmann::json({"p:2", "p:3", "p:4"}),
          "record author encoding is wrong");
  journal.restore_next_id(500);
  require(journal.append(timer).cause == 500, "snapshot ID continuation failed");
}

void writer_preserves_every_batch_in_order_and_finishes_stream() {
  const auto path = std::filesystem::current_path() / "ambergris-record-test.jsonl.zst";
  const nlohmann::json header = {
      {"run", "record-test"}, {"world", "test"}, {"step_hz", 20}, {"spec", 1}};
  record::Writer writer(path, header, 2, 3);
  for (int batch = 0; batch < 200; ++batch) {
    std::vector<nlohmann::json> events;
    for (int item = 0; item < 17; ++item)
      events.push_back({{"id", batch * 17 + item}, {"text", "record ✓"}});
    writer.submit(std::move(events));
  }
  writer.finish();
  const auto statistics = writer.stats();
  require(statistics.records == 3400 && statistics.queue_high_water <= 2,
          "writer lost records or exceeded its ring");
  std::istringstream lines(decompress(path));
  std::string line;
  std::getline(lines, line);
  require(nlohmann::json::parse(line) == header, "header differs");
  int count = 0;
  while (std::getline(lines, line)) {
    const auto event = nlohmann::json::parse(line);
    require(event["id"] == count && event["text"] == "record ✓", "batch order or UTF-8 changed");
    ++count;
  }
  require(count == 3400, "decompressed record count differs");
  std::filesystem::remove(path);
}
} // namespace

int main() {
  try {
    causes_are_carried_without_history_and_seed_is_excluded();
    writer_preserves_every_batch_in_order_and_finishes_stream();
    std::cout << "record tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
