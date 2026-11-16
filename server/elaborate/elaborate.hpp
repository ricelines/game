#pragma once

#include "record/record.hpp"
#include "snap/snap.hpp"

#include <map>
#include <span>
#include <unordered_map>

namespace amber::elaborate {
using Hash = std::string;
using RequestId = std::uint32_t;
using JobId = std::uint64_t;
using TextRef = std::uint32_t;
struct GapKey {
  TermId term = 0, material = 0;
  auto operator<=>(const GapKey &) const = default;
};
enum class Status { Open, Claimed, Answered, Refused };
enum class AnswerKind { None, Rule, Patch, Detail, Refusal };
struct Answer {
  AnswerKind kind = AnswerKind::None;
  Hash rule;
  TextRef text = 0;
  std::string note;
};
struct Request {
  RequestId id = 0;
  PlayerId requester = 0;
  TermId action = 0;
  GapKey gap;
  Handle target;
  Hash capture;
  Status status = Status::Open;
  PlayerId claimed_by = 0;
  Step created_step = 0, answered_step = 0;
  Answer answer;
  std::string note, display;
};
struct Requests {
  std::vector<Request> rows{Request{}};
};
struct GapCounts {
  std::map<GapKey, std::uint64_t> rows;
};
struct LastGap {
  TermId action = 0;
  GapKey gap;
  Handle target;
  Step step = 0;
  Hash capture;
  amber::Origin origin;
};
struct LastGaps {
  std::unordered_map<PlayerId, LastGap> rows;
};
enum class CaptureStatus { Pending, Ready, Failed };
struct CaptureRef {
  Hash hash;
  std::filesystem::path path;
  CaptureStatus status = CaptureStatus::Pending;
  std::string error;
};
struct CaptureRefs {
  std::unordered_map<Hash, CaptureRef> rows;
};
struct RoleAccess {
  PlayerId player = 0;
  bool elaborator = false;
};
struct GapInput {
  TermId action = 0;
  GapKey gap;
  Handle target;
  PlayerId originating_player = 0;
  Step step = 0;
  Hash capture;
  amber::Origin origin;
};
struct GapOutcome {
  std::uint64_t count = 0;
  RequestId automatic_request = 0;
  bool capture_required = false;
};
struct CloseOutcome {
  RequestId request = 0;
  PlayerId requester = 0;
  std::string kind = "answered", text;
};
GapOutcome note_gap(GapCounts &, LastGaps &, Requests &, const GapInput &,
                    std::uint32_t gap_captures);
void attach_capture(Requests &, LastGaps &, const GapOutcome &, PlayerId, Hash);
RequestId request_model(Requests &, const LastGaps &, PlayerId, std::string note, Step);
std::vector<Request> requests(const Requests &, const GapCounts &, RoleAccess, std::uint32_t page,
                              std::uint32_t page_size);
void claim(Requests &, RoleAccess, RequestId);
CloseOutcome refuse(Requests &, RoleAccess, RequestId, TextRef, std::string display, Step);
CloseOutcome answer_patch(Requests &, RoleAccess, RequestId, std::string note, Step);
CloseOutcome answer_detail(Requests &, RoleAccess, RequestId, TextRef, std::string display, Step);

struct SessionOrder {
  std::string session;
  std::uint64_t sequence = 0;
};
struct FixtureLimits {
  std::uint64_t timeout_ms = 0;
  std::size_t diagnostic_bytes = 0, result_bytes = 0;
};
struct RuleCandidate {
  Hash artifact, registry;
  ScriptId script = 0;
  std::string runtime;
  nlohmann::json tests;
  std::map<std::string, Handle> subjects;
};
struct FixtureJob {
  JobId id = 0;
  RequestId request = 0;
  PlayerId actor = 0;
  Hash capture;
  std::filesystem::path capture_path;
  RuleCandidate candidate;
  SessionOrder order;
  FixtureLimits limits;
};
struct PendingFixtures {
  // Active subprocesses only; requests and accepted answers outlive the process.
  JobId next_id = 1;
  std::map<JobId, FixtureJob> rows;
};
enum class FixtureStatus { Completed, Error, Unavailable };
struct TestRun {
  bool passed = false;
  nlohmann::json observed;
  std::string error;
};
struct TestResult {
  TestRun candidate, baseline;
};
struct FixtureResult {
  JobId job = 0;
  RequestId request = 0;
  Hash capture, candidate;
  std::string runtime;
  FixtureStatus status = FixtureStatus::Error;
  std::vector<TestResult> tests;
  std::string error, diagnostics;
  double elapsed_ms = 0;
};
struct CompletionCheck {
  JobId job = 0;
  RequestId request = 0;
  PlayerId actor = 0;
  Hash candidate;
  bool accepted = false;
  std::string error;
};
void validate_tests(const nlohmann::json &);
FixtureJob begin_rule_answer(PendingFixtures &, const Requests &, const CaptureRefs &, RoleAccess,
                             RequestId, RuleCandidate, SessionOrder, FixtureLimits);
CompletionCheck check_fixture_completion(const PendingFixtures &, const Requests &, RoleAccess,
                                         const FixtureResult &);
CloseOutcome close_rule_answer(Requests &, PendingFixtures &, const CompletionCheck &, Step);
void discard_fixture(PendingFixtures &, JobId);
bool session_pending(const PendingFixtures &, std::string_view session);
nlohmann::json job_json(const FixtureJob &);
FixtureResult result_from_json(const nlohmann::json &);
nlohmann::json result_json(const FixtureResult &);

class FixtureWorker {
public:
  FixtureWorker(std::filesystem::path executable, std::filesystem::path directory,
                std::size_t concurrency);
  ~FixtureWorker();
  FixtureWorker(const FixtureWorker &) = delete;
  FixtureWorker &operator=(const FixtureWorker &) = delete;
  void submit(FixtureJob);
  std::vector<FixtureResult> take();
  void finish();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
struct CaptureWrite {
  Hash hash;
  std::filesystem::path path;
  std::string error;
};
// Hashing completed bytes occurs at submission; compression/file I/O are off-step.
class CaptureWriter {
public:
  CaptureWriter(std::filesystem::path directory, int compression_level);
  ~CaptureWriter();
  CaptureWriter(const CaptureWriter &) = delete;
  CaptureWriter &operator=(const CaptureWriter &) = delete;
  Hash submit(std::vector<std::uint8_t>);
  std::vector<CaptureWrite> take();
  void finish();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
void complete_capture(CaptureRefs &, const CaptureWrite &);

using ObjectSelection = std::optional<std::span<const Handle>>;
using GapSelection = std::optional<std::span<const GapKey>>;
using HashSelection = std::optional<std::span<const Hash>>;
void write_requests(snap::Output &, const Requests &, ObjectSelection = {});
void read_requests(snap::Input &, Requests &);
void write_gaps(snap::Output &, const GapCounts &, GapSelection = {});
void read_gaps(snap::Input &, GapCounts &);
void write_last_gaps(snap::Output &, const LastGaps &, ObjectSelection = {});
void read_last_gaps(snap::Input &, LastGaps &);
void write_capture_refs(snap::Output &, const CaptureRefs &, HashSelection = {});
void read_capture_refs(snap::Input &, CaptureRefs &);
} // namespace amber::elaborate

namespace amber::fixture {
struct Run {
  std::filesystem::path executable, catalog, work_directory;
  elaborate::FixtureJob job;
};
// Composition only: every TestRun is reported by a fresh real simulation child.
elaborate::FixtureResult evaluate(const Run &);
int command(int argc, char **argv, const std::filesystem::path &executable);
} // namespace amber::fixture

namespace amber::elaborate {
struct State {
  Requests requests;
  GapCounts gap_counts;
  LastGaps last_gaps;
  CaptureRefs captures;
  PendingFixtures fixtures;
};
} // namespace amber::elaborate
