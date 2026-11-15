#include "catalog/snapshot.hpp"
#include "elaborate/snapshot.hpp"

#include <iostream>
#include <stdexcept>

using namespace amber;
using namespace amber::elaborate;
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class F> void rejects(F action, const char *message) {
  bool failed = false;
  try {
    action();
  } catch (const std::invalid_argument &) {
    failed = true;
  }
  require(failed, message);
}
nlohmann::json suite() {
  return nlohmann::json::array(
      {{{"given", {{"subject", "target"}, {"contribute", {{"x.exposure", 2.0}}}}},
        {"when", {{"steps", 200}}},
        {"expect", {{"subject", "target"}, {"term", "x.integrity"}, {"below", 1.0}}}}});
}
FixtureResult passing(const FixtureJob &job) {
  FixtureResult result;
  result.job = job.id;
  result.request = job.request;
  result.capture = job.capture;
  result.candidate = job.candidate.artifact;
  result.runtime = job.candidate.runtime;
  result.status = FixtureStatus::Completed;
  result.tests = {{{true, 0.5, {}}, {false, 1.0, {}}}};
  return result;
}
void lifecycle_and_fixture_acceptance() {
  Requests table;
  GapCounts counts;
  LastGaps last;
  const auto target = Handle::make(4, 7);
  GapInput input{10, {20, 30}, target, 2, 50, std::string(64, 'a'), {}};
  for (unsigned i = 1; i <= 7; ++i) {
    auto outcome = note_gap(counts, last, table, input, 3);
    require(outcome.count == i && bool(outcome.automatic_request) == (i <= 3),
            "exact first-three automatic requests");
    require(outcome.capture_required, "originating player retains historical last-gap capture");
  }
  require(table.rows.size() == 4 && table.rows[1].requester == 0, "automatic requester is zero");
  const auto explicit_id = request_model(table, last, 2, "please model it", 55);
  const auto repeated = request_model(table, last, 2, "another explanation", 56);
  require(table.rows[explicit_id].capture == input.capture && table.rows[repeated].target == target,
          "explicit requests reuse historical capture");
  GapInput other{11, {21, 31}, Handle::make(5, 7), 0, 60, std::string(64, 'b'), {}};
  auto next = note_gap(counts, last, table, other, 3);
  require(next.capture_required, "first anonymous gap captures");
  other.originating_player = 0;
  for (unsigned i = 0; i < 3; ++i)
    next = note_gap(counts, last, table, other, 3);
  require(!next.capture_required, "late anonymous gap only increments count");
  const RoleAccess elaborator{3, true};
  auto listed = requests(table, counts, elaborator, 0, 100);
  require(listed[0].id == explicit_id && listed[1].id == repeated && listed[2].gap == input.gap,
          "explicit requests first then descending gap frequency");
  auto page = requests(table, counts, elaborator, 1, 2);
  require(page.size() == 2 && page[0].id == listed[2].id, "stable pagination");
  rejects([&] { requests(table, counts, {3, false}, 0, 10); }, "listing requires role");
  claim(table, elaborator, explicit_id);
  claim(table, elaborator, explicit_id);
  rejects([&] { claim(table, {4, true}, explicit_id); }, "claim excludes other elaborator");
  rejects([&] { answer_patch(table, {4, true}, explicit_id, "edited", 60); },
          "other claim cannot be closed");
  CaptureRefs captures;
  complete_capture(captures, {input.capture, "capture.amb.zst", {}});
  RuleCandidate candidate{
      std::string(64, 'c'), std::string(64, 'd'), 4, "test-runtime", suite(), {}};
  PendingFixtures pending;
  auto job = begin_rule_answer(pending, table, captures, elaborator, explicit_id, candidate,
                               {"one", 7}, {1000, 1024, 65536});
  require(job.candidate.subjects.at("target") == target && session_pending(pending, "one") &&
              !session_pending(pending, "two"),
          "job retains subject and per-session order");
  rejects(
      [&] {
        begin_rule_answer(pending, table, captures, elaborator, repeated, candidate, {"one", 8},
                          {1000, 1024, 65536});
      },
      "one pending input per session");
  auto result = passing(job);
  require(check_fixture_completion(pending, table, elaborator, result).accepted,
          "candidate passes and baseline demonstrates gap");
  result.tests[0].baseline.passed = true;
  require(!check_fixture_completion(pending, table, elaborator, result).accepted,
          "vacuous baseline rejected");
  result.tests[0].baseline.passed = false;
  result.tests[0].baseline.error = "runtime failure";
  require(!check_fixture_completion(pending, table, elaborator, result).accepted,
          "baseline error is not evidence of difference");
  result = passing(job);
  result.tests[0].candidate.passed = false;
  require(!check_fixture_completion(pending, table, elaborator, result).accepted,
          "failing candidate rejected");
  result = passing(job);
  result.capture = "wrong";
  require(!check_fixture_completion(pending, table, elaborator, result).accepted,
          "mismatched report rejected");
  result = passing(job);
  require(!check_fixture_completion(pending, table, {3, false}, result).accepted,
          "role rechecked after worker");
  auto check = check_fixture_completion(pending, table, elaborator, result);
  auto closed = close_rule_answer(table, pending, check, 100);
  require(closed.requester == 2 && closed.kind == "answered" &&
              table.rows[explicit_id].status == Status::Answered &&
              !session_pending(pending, "one"),
          "rule completion closes and notifies original requester");
  rejects([&] { refuse(table, elaborator, explicit_id, 1, "no", 101); },
          "closed request cannot close twice");
  auto detail = answer_detail(table, elaborator, repeated, 5, "new explanation", 101);
  require(detail.text == "new explanation" &&
              table.rows[repeated].answer.kind == AnswerKind::Detail,
          "detail answer keeps separate text identity");
  auto refusal = refuse(table, elaborator, 1, 6, "out of scope", 102);
  require(!refusal.requester && table.rows[1].status == Status::Refused,
          "anonymous refusal closes without inventing requester");
  rejects([&] { validate_tests(nlohmann::json::array()); }, "empty suite rejected");
  auto malformed = suite();
  malformed[0]["when"]["action"] = {{"term", "x.touch"}};
  rejects([&] { validate_tests(malformed); }, "ambiguous when rejected");
  malformed = suite();
  malformed[0]["given"]["contribute"]["x.exposure"] = "unsafe";
  rejects([&] { validate_tests(malformed); }, "nonnumeric contribution rejected");
  auto handles = suite();
  handles[0]["given"] = {{"subject", "target"}, {"state", {{"x.link", {{"handle", "other"}}}}}};
  handles[0]["expect"] = {
      {"subject", "target"}, {"term", "x.link"}, {"equals", {{"handle", "other"}}}};
  validate_tests(handles);
  handles[0]["given"]["state"]["x.link"] = "ordinary string";
  rejects([&] { validate_tests(handles); }, "property state is typed scalar or subject handle");
}
void snapshot_and_attributed_text() {
  Requests table;
  GapCounts counts;
  LastGaps last;
  CaptureRefs captures;
  PendingFixtures pending;
  const auto target = Handle::make(1, 9), outside = Handle::make(2, 9);
  auto outcome =
      note_gap(counts, last, table, {1, {2, 3}, target, 4, 50, {}, {8, 7, {6}, 1.0, true}}, 3);
  attach_capture(table, last, outcome, 4, std::string(64, 'a'));
  const auto explicit_id = request_model(table, last, 4, "note", 51);
  note_gap(counts, last, table, {1, {5, 3}, outside, 0, 52, std::string(64, 'b'), {}}, 3);
  claim(table, {6, true}, explicit_id);
  complete_capture(captures, {std::string(64, 'a'), "inside.zst", {}});
  complete_capture(captures, {std::string(64, 'b'), "outside.zst", "failed write"});
  RuleCandidate candidate{
      std::string(64, 'c'), std::string(64, 'd'), 4, "test-runtime", suite(), {}};
  auto job = begin_rule_answer(pending, table, captures, {6, true}, explicit_id, candidate,
                               {"session", 42}, {1000, 128, 65536});
  const std::array<Handle, 1> handles{target};
  const std::array<GapKey, 1> gaps{{{2, 3}}};
  const std::array<Hash, 1> hashes{std::string(64, 'a')};
  snap::Buffer bytes;
  snap::Output out{bytes};
  write_requests(out, table, std::span<const Handle>(handles));
  write_gaps(out, counts, std::span<const GapKey>(gaps));
  write_last_gaps(out, last, std::span<const Handle>(handles));
  write_capture_refs(out, captures, std::span<const Hash>(hashes));
  out.adapter().flush();
  bytes.resize(out.adapter().writtenBytesCount());
  Requests loaded;
  GapCounts loaded_counts;
  LastGaps loaded_last;
  CaptureRefs loaded_refs;
  PendingFixtures loaded_jobs;
  snap::Input in{bytes.data(), bytes.size()};
  read_requests(in, loaded);
  read_gaps(in, loaded_counts);
  read_last_gaps(in, loaded_last);
  read_capture_refs(in, loaded_refs);
  snap::check_complete(in);
  require(loaded.rows.size() == table.rows.size() && !loaded.rows.back().id &&
              loaded.rows[explicit_id].claimed_by == 6,
          "selective capture retains high-water and claim");
  require(loaded_counts.rows.size() == 1 &&
              loaded_last.rows.at(4).origin == last.rows.at(4).origin &&
              loaded_refs.rows.size() == 1,
          "only selected gaps and full causal provenance restored");
  require(loaded_jobs.rows.empty() && loaded_jobs.next_id == 1 &&
              !session_pending(loaded_jobs, "session"),
          "a restored process has no active fixture subprocess or copied job");
  require(!check_fixture_completion(loaded_jobs, loaded, {6, true}, passing(job)).accepted,
          "a report from a previous process cannot finish a restored request");
  auto retried = begin_rule_answer(loaded_jobs, loaded, loaded_refs, {6, true}, explicit_id,
                                   candidate, {"new-session", 1}, {1000, 128, 65536});
  discard_fixture(loaded_jobs, retried.id);
  require(!session_pending(loaded_jobs, "new-session") &&
              loaded.rows[explicit_id].status == Status::Claimed,
          "the elaborator can explicitly answer the retained claim again after restart");
  catalog::ModelTexts maker;
  maker.rows[1] = {"maker name", "maker description", "maker writing"};
  catalog::AuthoredTexts texts;
  auto detail = catalog::append_authored_text(texts, target, 6, 12.5,
                                              catalog::AuthoredTextKind::Detail, "new detail");
  catalog::append_authored_text(texts, outside, 7, 13, catalog::AuthoredTextKind::Refusal,
                                "not modeled");
  require(maker.rows[1].description == "maker description" && texts.rows[detail].author == 6 &&
              texts.details_by_target.at(target.bits) == std::vector<catalog::TextId>{detail},
          "elaboration attribution leaves maker text separate");
  snap::Buffer text_bytes;
  snap::Output text_out{text_bytes};
  catalog::write_authored_texts(text_out, texts, std::span<const Handle>(handles));
  text_out.adapter().flush();
  text_bytes.resize(text_out.adapter().writtenBytesCount());
  catalog::AuthoredTexts restored;
  snap::Input text_in{text_bytes.data(), text_bytes.size()};
  catalog::read_authored_texts(text_in, restored);
  snap::check_complete(text_in);
  require(restored.rows.size() == texts.rows.size() && !restored.rows.back().id &&
              restored.details_by_target.at(target.bits).front() == detail &&
              restored.rows[detail].created_s == 12.5,
          "selected attributed text and index restore");
}
} // namespace
int main() {
  lifecycle_and_fixture_acceptance();
  snapshot_and_attributed_text();
  std::cout << "elaboration lifecycle, fixture acceptance and selective snapshot checks passed\n";
}
