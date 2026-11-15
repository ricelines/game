#include "elaborate.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace amber::elaborate {
namespace {
void require_role(RoleAccess role) {
  if (!role.player || !role.elaborator)
    throw std::invalid_argument("elaborator role required");
}
const Request &find(const Requests &table, RequestId id) {
  if (!id || id >= table.rows.size() || table.rows[id].id != id)
    throw std::invalid_argument("unknown request");
  return table.rows[id];
}
const Request &check_open(const Requests &table, RoleAccess role, RequestId id) {
  require_role(role);
  const auto &request = find(table, id);
  if (request.status != Status::Open && request.status != Status::Claimed)
    throw std::invalid_argument("request is already closed");
  if (request.status == Status::Claimed && request.claimed_by != role.player)
    throw std::invalid_argument("request claimed by another elaborator");
  return request;
}
CloseOutcome close(Requests &table, RoleAccess role, RequestId id, Answer answer,
                   std::string display, Step step, Status status = Status::Answered) {
  (void)check_open(table, role, id);
  auto &request = table.rows[id];
  request.answer = std::move(answer);
  request.display = std::move(display);
  request.answered_step = step;
  request.status = status;
  return {id, request.requester, "answered", request.display};
}
bool handle_value(const nlohmann::json &value) {
  return value.is_object() && value.size() == 1 && value.contains("handle") &&
         value["handle"].is_string() && !value["handle"].get<std::string>().empty();
}
void scalar_map(const nlohmann::json &values, bool numeric) {
  if (!values.is_object())
    throw std::invalid_argument("fixture state/contribute must be a term-value object");
  for (const auto &[name, value] : values.items()) {
    if (name.empty() ||
        (!value.is_number() && (numeric || (!value.is_boolean() && !handle_value(value)))) ||
        (value.is_number() && !std::isfinite(value.get<double>())))
      throw std::invalid_argument(
          "fixture value must be a finite number, state flag or subject handle");
  }
}
void given(const nlohmann::json &value) {
  if (!value.is_object() || !value.contains("subject") || !value["subject"].is_string() ||
      value["subject"].get<std::string>().empty())
    throw std::invalid_argument("fixture given needs a named subject");
  for (const auto &[name, entry] : value.items()) {
    if (name == "subject")
      continue;
    if (name != "state" && name != "contribute")
      throw std::invalid_argument("unknown fixture given field");
    scalar_map(entry, name == "contribute");
  }
}
} // namespace
GapOutcome note_gap(GapCounts &counts, LastGaps &last, Requests &table, const GapInput &input,
                    std::uint32_t limit) {
  const auto count = ++counts.rows[input.gap];
  if (input.originating_player)
    last.rows[input.originating_player] = {input.action, input.gap,     input.target,
                                           input.step,   input.capture, input.origin};
  RequestId id = 0;
  if (count <= limit) {
    id = static_cast<RequestId>(table.rows.size());
    Request request;
    request.id = id;
    request.action = input.action;
    request.gap = input.gap;
    request.target = input.target;
    request.capture = input.capture;
    request.created_step = input.step;
    table.rows.push_back(std::move(request));
  }
  return {count, id, count <= limit || input.originating_player != 0};
}
void attach_capture(Requests &table, LastGaps &last, const GapOutcome &outcome, PlayerId player,
                    Hash hash) {
  if (outcome.automatic_request)
    table.rows[outcome.automatic_request].capture = hash;
  if (player)
    last.rows.at(player).capture = std::move(hash);
}
RequestId request_model(Requests &table, const LastGaps &last, PlayerId player, std::string note,
                        Step step) {
  const auto found = last.rows.find(player);
  if (!player || found == last.rows.end())
    throw std::invalid_argument("no previous not_modeled result");
  if (found->second.capture.empty())
    throw std::invalid_argument("last modeling gap capture unavailable");
  const auto &gap = found->second;
  Request request;
  request.id = static_cast<RequestId>(table.rows.size());
  request.requester = player;
  request.action = gap.action;
  request.gap = gap.gap;
  request.target = gap.target;
  request.capture = gap.capture;
  request.created_step = step;
  request.note = std::move(note);
  table.rows.push_back(std::move(request));
  return table.rows.back().id;
}
std::vector<Request> requests(const Requests &table, const GapCounts &counts, RoleAccess role,
                              std::uint32_t page, std::uint32_t page_size) {
  require_role(role);
  struct Listed {
    const Request *request;
    std::uint64_t count;
  };
  std::vector<Listed> rows;
  for (const auto &request : table.rows)
    if (request.id && (request.status == Status::Open || request.status == Status::Claimed)) {
      auto found = counts.rows.find(request.gap);
      rows.push_back({&request, found == counts.rows.end() ? 0 : found->second});
    }
  std::sort(rows.begin(), rows.end(), [](const Listed &left, const Listed &right) {
    const auto *a = left.request, *b = right.request;
    if (bool(a->requester) != bool(b->requester))
      return bool(a->requester);
    if (!a->requester && left.count != right.count)
      return left.count > right.count;
    return std::tie(a->created_step, a->id) < std::tie(b->created_step, b->id);
  });
  const auto begin = std::min(rows.size(), std::size_t(page) * page_size);
  const auto end = std::min(rows.size(), begin + page_size);
  std::vector<Request> page_rows;
  page_rows.reserve(end - begin);
  for (auto i = begin; i < end; ++i)
    page_rows.push_back(*rows[i].request);
  return page_rows;
}
void claim(Requests &table, RoleAccess role, RequestId id) {
  (void)check_open(table, role, id);
  table.rows[id].status = Status::Claimed;
  table.rows[id].claimed_by = role.player;
}
CloseOutcome refuse(Requests &table, RoleAccess role, RequestId id, TextRef text,
                    std::string display, Step step) {
  if (!text)
    throw std::invalid_argument("refusal needs an attributed text id");
  return close(table, role, id, {AnswerKind::Refusal, {}, text, {}}, std::move(display), step,
               Status::Refused);
}
CloseOutcome answer_patch(Requests &table, RoleAccess role, RequestId id, std::string note,
                          Step step) {
  return close(table, role, id, {AnswerKind::Patch, {}, 0, note}, note, step);
}
CloseOutcome answer_detail(Requests &table, RoleAccess role, RequestId id, TextRef text,
                           std::string display, Step step) {
  if (!text)
    throw std::invalid_argument("detail needs an attributed text id");
  return close(table, role, id, {AnswerKind::Detail, {}, text, {}}, std::move(display), step);
}
void validate_tests(const nlohmann::json &tests) {
  if (!tests.is_array() || tests.empty())
    throw std::invalid_argument("rule needs a nonempty fixture test array");
  for (const auto &test : tests) {
    if (!test.is_object() || !test.contains("when") || !test.contains("expect"))
      throw std::invalid_argument("fixture test needs when and expect");
    if (test.contains("given")) {
      if (test["given"].is_array())
        for (const auto &entry : test["given"])
          given(entry);
      else
        given(test["given"]);
    }
    const auto &when = test["when"];
    if (!when.is_object() || when.size() != 1 ||
        (!when.contains("steps") && !when.contains("action")))
      throw std::invalid_argument("fixture when needs either steps or action");
    if (when.contains("steps")) {
      const auto &steps = when["steps"];
      bool valid = steps.is_number_unsigned();
      if (steps.is_number_integer() && !valid)
        valid = steps.get<std::int64_t>() >= 0;
      if (steps.is_number_float()) {
        const auto value = steps.get<double>();
        valid = std::isfinite(value) && value >= 0 && std::trunc(value) == value &&
                value < std::ldexp(1., std::numeric_limits<Step>::digits);
      }
      if (!valid)
        throw std::invalid_argument("fixture steps must be a nonnegative integer in Step range");
    }
    if (when.contains("action") &&
        (!when["action"].is_object() || !when["action"].contains("term") ||
         !when["action"]["term"].is_string()))
      throw std::invalid_argument("fixture action needs a term");
    const auto &expect = test["expect"];
    if (!expect.is_object() || !expect.contains("subject") || !expect["subject"].is_string() ||
        !expect.contains("term") || !expect["term"].is_string())
      throw std::invalid_argument("fixture expect needs subject and term");
    unsigned comparisons = 0;
    for (const char *name : {"equals", "above", "below"})
      if (expect.contains(name)) {
        ++comparisons;
        const auto &value = expect[name];
        if ((!value.is_number() && !(std::string_view(name) == "equals" &&
                                     (value.is_boolean() || handle_value(value)))) ||
            (value.is_number() && !std::isfinite(value.get<double>())))
          throw std::invalid_argument("fixture comparison needs a finite number or equals flag");
      }
    if (comparisons != 1)
      throw std::invalid_argument("fixture expect needs one comparison");
  }
}
FixtureJob begin_rule_answer(PendingFixtures &pending, const Requests &table,
                             const CaptureRefs &captures, RoleAccess role, RequestId request,
                             RuleCandidate candidate, SessionOrder order, FixtureLimits limits) {
  const auto &row = check_open(table, role, request);
  const auto capture = captures.rows.find(row.capture);
  if (capture == captures.rows.end() || capture->second.status != CaptureStatus::Ready)
    throw std::invalid_argument("request capture is not ready");
  if (!candidate.script || candidate.artifact.empty() || candidate.registry.empty())
    throw std::invalid_argument("validated candidate script and registry pin required");
  if (candidate.runtime.empty() || !limits.timeout_ms || !limits.result_bytes)
    throw std::invalid_argument("fixture runtime and nonzero execution limits required");
  validate_tests(candidate.tests);
  if (session_pending(pending, order.session))
    throw std::invalid_argument("session already has a pending fixture");
  candidate.subjects["target"] = row.target;
  FixtureJob job{
      pending.next_id++,    request,          role.player, row.capture, capture->second.path,
      std::move(candidate), std::move(order), limits};
  pending.rows.emplace(job.id, job);
  return job;
}
CompletionCheck check_fixture_completion(const PendingFixtures &pending, const Requests &table,
                                         RoleAccess role, const FixtureResult &result) {
  CompletionCheck check{result.job, result.request, role.player, result.candidate, false, {}};
  try {
    const auto it = pending.rows.find(result.job);
    if (it == pending.rows.end())
      throw std::invalid_argument("fixture job is no longer pending");
    const auto &job = it->second;
    const auto &request = check_open(table, role, job.request);
    if (job.actor != role.player || result.request != job.request ||
        result.capture != job.capture || request.capture != job.capture ||
        result.candidate != job.candidate.artifact || result.runtime != job.candidate.runtime)
      throw std::invalid_argument("fixture result identity mismatch");
    if (result.status != FixtureStatus::Completed)
      throw std::invalid_argument(result.error.empty() ? "fixture did not complete" : result.error);
    if (!result.error.empty())
      throw std::invalid_argument(result.error);
    if (result.tests.size() != job.candidate.tests.size())
      throw std::invalid_argument("fixture test count mismatch");
    bool baseline_failed = false;
    for (const auto &test : result.tests) {
      if (!test.candidate.error.empty() || !test.baseline.error.empty())
        throw std::invalid_argument("fixture execution error is not a comparison failure");
      if (!test.candidate.passed)
        throw std::invalid_argument("candidate fixture assertion failed");
      baseline_failed |= !test.baseline.passed;
    }
    if (!baseline_failed)
      throw std::invalid_argument("candidate adds no demonstrated behavior");
    check.accepted = true;
  } catch (const std::exception &error) {
    check.error = error.what();
  }
  return check;
}
CloseOutcome close_rule_answer(Requests &table, PendingFixtures &pending,
                               const CompletionCheck &check, Step step) {
  if (!check.accepted)
    throw std::invalid_argument("fixture was not accepted");
  const auto &job = pending.rows.at(check.job);
  if (job.actor != check.actor || job.request != check.request ||
      job.candidate.artifact != check.candidate)
    throw std::invalid_argument("accepted fixture identity mismatch");
  auto outcome =
      close(table, {check.actor, true}, check.request, {AnswerKind::Rule, check.candidate, 0, {}},
            "Rule added: " + check.candidate, step);
  pending.rows.erase(check.job);
  return outcome;
}
bool session_pending(const PendingFixtures &pending, std::string_view session) {
  for (const auto &[id, job] : pending.rows) {
    (void)id;
    if (job.order.session == session)
      return true;
  }
  return false;
}
void discard_fixture(PendingFixtures &pending, JobId job) {
  pending.rows.erase(job);
}
void complete_capture(CaptureRefs &refs, const CaptureWrite &completion) {
  refs.rows[completion.hash] = {
      completion.hash, completion.path,
      completion.error.empty() ? CaptureStatus::Ready : CaptureStatus::Failed, completion.error};
}
} // namespace amber::elaborate
