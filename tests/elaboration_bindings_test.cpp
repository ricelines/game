#include "bindings/elaboration/elaboration.hpp"

#include <iostream>
#include <stdexcept>

using namespace amber;
namespace e = amber::bindings::elaboration;
using Json = nlohmann::json;
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class F> void rejects(F action, const char *message) {
  bool rejected = false;
  try {
    action();
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, message);
}
Json tests() {
  return Json::array({{{"given", {{"subject", "target"}}},
                       {"when", {{"steps", 1}}},
                       {"expect", {{"subject", "target"}, {"term", "x.charge"}, {"equals", 7}}}}});
}
struct Activation {
  unsigned calls = 0;
  e::RuleUpdate last = e::RuleUpdate::Disable;
  record::Origin origin;
  static void apply(void *p, ScriptId id, e::RuleUpdate update, Step, double,
                    const record::Origin &origin, react::DeferredEffects &effects) {
    auto &self = *static_cast<Activation *>(p);
    require(id == 1, "activation retains immutable script identity");
    ++self.calls;
    self.last = update;
    self.origin = origin;
    effects.changed = true;
  }
  e::RuleActivation hook() { return {this, apply}; }
};
struct Fixture {
  elaborate::Requests requests;
  elaborate::GapCounts gaps;
  elaborate::LastGaps last;
  elaborate::CaptureRefs captures;
  elaborate::PendingFixtures pending;
  control::Profiles profiles;
  catalog::Scripts scripts;
  catalog::AuthoredTexts texts;
  drive::Inputs inputs;
  Activation activation;
  Handle target = Handle::make(21, 9), body = Handle::make(22, 9);
  Fixture() {
    control::put_profile(profiles, {1, "Requester", body, {}, control::all_modes, {}});
    control::put_profile(profiles, {2, "Elaborator", {}, {"elaborator"}, control::all_modes, {}});
    control::put_profile(profiles, {3, "Other", {}, {"elaborator"}, control::all_modes, {}});
    elaborate::note_gap(gaps, last, requests, {8, {9, 10}, target, 1, 11, std::string(64, 'a'), {}},
                        0);
    elaborate::complete_capture(captures, {std::string(64, 'a'), "normal", {}});
    catalog::Script script;
    script.id = 1;
    script.artifact = std::string(64, 'b');
    script.registry_pin = std::string(64, 'c');
    script.declaration.kind = react::ScriptDeclaration::Kind::Rule;
    script.tests = tests();
    scripts.rows.push_back(script);
  }
  drive::NativeCall call(PlayerId who, Json args) {
    drive::NativeCall out;
    out.request.caller = who;
    out.arguments = std::move(args);
    out.step = 20;
    out.hz = 20;
    out.origin.cause = 77;
    return out;
  }
  elaborate::RequestId request() {
    e::RequestBinding binding{requests, last};
    auto result = e::request_model(&binding, call(1, {{"note", "Model this behavior"}}));
    require(result.changed && result.records[0].things[0] == target,
            "native request records actual historical target");
    return static_cast<elaborate::RequestId>(requests.rows.size() - 1);
  }
  void claim(elaborate::RequestId id) {
    e::ClaimBinding binding{requests, profiles};
    e::claim(&binding, call(2, {{"request", "request:" + std::to_string(id)}}));
  }
  std::pair<elaborate::FixtureJob, elaborate::FixtureResult> pending_job(elaborate::RequestId id) {
    claim(id);
    elaborate::RuleCandidate candidate{
        scripts.rows[1].artifact, scripts.rows[1].registry_pin, 1, "runtime", tests(),
        {{"body", body}}};
    auto job = elaborate::begin_rule_answer(pending, requests, captures, {2, true}, id, candidate,
                                            {"30", 4}, {1000, 64, 65536});
    drive::Input input;
    input.caller = PlayerId{2};
    input.session = 30;
    input.sequence = 4;
    input.request = 98;
    input.origin.cause = 123;
    const auto work = drive::enqueue_input(inputs, input);
    inputs.rows.at(work.id).awaiting = true;
    elaborate::FixtureResult result;
    result.job = job.id;
    result.request = id;
    result.capture = job.capture;
    result.candidate = job.candidate.artifact;
    result.runtime = "runtime";
    result.status = elaborate::FixtureStatus::Completed;
    result.tests = {{{true, 7, {}}, {false, 0, {}}}};
    return {job, result};
  }
};
void synchronous_workflow() {
  Fixture f;
  const auto id = f.request();
  e::RequestsBinding list{f.requests, f.gaps, f.profiles, 10};
  rejects([&] { e::requests(&list, f.call(1, Json::object())); }, "listing requires current role");
  require(e::requests(&list, f.call(2, Json::object())).reply["requests"].size() == 1,
          "native request list exposes exact unresolved rows");
  f.claim(id);
  e::DetailBinding detail{f.requests, f.texts, f.profiles, {}};
  const Json answer{{"request", "request:1"}, {"kind", "detail"}, {"text", "Authored explanation"}};
  rejects([&] { e::answer_detail(&detail, f.call(3, answer)); }, "foreign claim cannot close");
  require(f.texts.rows.size() == 1, "failed authorization cannot append orphan authored text");
  const auto out = e::answer_detail(&detail, f.call(2, answer));
  require(out.changed && f.texts.rows.size() == 2 && f.texts.rows[1].author == 2 &&
              f.texts.rows[1].target == f.target &&
              f.requests.rows[id].status == elaborate::Status::Answered,
          "detail closes request and appends independently attributed target text");
  require(!e::validate_answer(
               {{"request", "request:1"}, {"kind", "rule"}, {"script", "s:1"}, {"tests", tests()}})
               .empty(),
          "caller supplied fixture expectations are rejected");
  require(!e::validate_answer({{"request", "request:1"},
                               {"kind", "rule"},
                               {"script", "s:1"},
                               {"subjects", {{"body", to_string(f.target)}}}})
               .empty(),
          "caller cannot override original requester's captured body");
}
void fixture_completion() {
  Fixture f;
  const auto id = f.request();
  const auto [job, result] = f.pending_job(id);
  (void)job;
  e::CompletionBinding binding{f.requests, f.pending,  f.inputs,
                               f.scripts,  f.profiles, f.activation.hook()};
  auto completed = e::complete(binding, result, 40, 20);
  require(completed.input && completed.input->session == 30 && completed.input->sequence == 4 &&
              completed.input->request == 98 && completed.input->fields["ok"] == true &&
              completed.notification->requester == 1 && f.activation.calls == 1 &&
              f.activation.last == e::RuleUpdate::Validated && f.activation.origin.cause == 123 &&
              f.requests.rows[id].status == elaborate::Status::Answered && f.inputs.rows.empty(),
          "verified completion activates once with original input cause and exact reply identity");
  require(!e::complete(binding, result, 41, 20).input && f.activation.calls == 1,
          "duplicate worker completion cannot reactivate or send another reply");
  for (int mode = 0; mode < 4; ++mode) {
    Fixture bad;
    const auto request = bad.request();
    auto [unused, report] = bad.pending_job(request);
    (void)unused;
    if (mode == 0)
      report.tests[0].baseline.passed = true;
    if (mode == 1)
      bad.profiles.rows[2].roles.clear();
    if (mode == 2)
      bad.requests.rows[request].claimed_by = 3;
    if (mode == 3)
      bad.scripts.rows[1].tests = Json::array();
    e::CompletionBinding b{bad.requests, bad.pending,  bad.inputs,
                           bad.scripts,  bad.profiles, bad.activation.hook()};
    const auto denied = e::complete(b, report, 50, 20);
    require(denied.input && denied.input->fields["ok"] == false && bad.activation.calls == 0 &&
                bad.requests.rows[request].status == elaborate::Status::Claimed &&
                bad.pending.rows.empty(),
            "vacuous test, revoked role, changed claim or candidate cannot activate or close");
  }
}
void async_submission(const std::filesystem::path &executable, const std::filesystem::path &dir) {
  Fixture f;
  const auto id = f.request();
  f.claim(id);
  elaborate::FixtureWorker worker(executable, dir, 1);
  e::RuleBinding binding{f.requests, f.pending, f.captures,        f.scripts,
                         f.profiles, worker,    "transport-probe", {1000, 64, 65536}};
  auto call = f.call(2, {{"request", "request:1"}, {"kind", "rule"}, {"script", "s:1"}});
  rejects([&] { e::answer_rule(&binding, call); }, "rule answer requires explicit ordered input");
  call.order = drive::InputOrder{9, 30, 4, 98};
  const auto started = e::answer_rule(&binding, call);
  const auto &job = f.pending.rows.begin()->second;
  require(started.deferred && !started.changed && job.candidate.tests == f.scripts.rows[1].tests &&
              job.candidate.subjects.at("target") == f.target &&
              job.candidate.subjects.at("body") == f.body,
          "async worker receives immutable verified tests and historical target/requester body");
  worker.finish();
  const auto reports = worker.take();
  require(reports.size() == 1 && reports[0].status == elaborate::FixtureStatus::Completed,
          "copied fixture subprocess protocol completes (transport probe only)");
  e::ActivationBinding activate{f.scripts, f.profiles, f.activation.hook()};
  auto change = f.call(2, {{"script", "s:1"}, {"active", true}});
  rejects([&] { e::rule_activation(&activate, change); }, "manual enable cannot approve new code");
  f.scripts.rows[1].declaration.approved = true;
  require(e::rule_activation(&activate, change).changed &&
              f.activation.last == e::RuleUpdate::Enable,
          "approved historical rule enable uses explicit registry activation path");
  change.arguments["active"] = false;
  e::rule_activation(&activate, change);
  require(f.activation.last == e::RuleUpdate::Disable, "disable is separate from fixture approval");
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const auto directory = std::filesystem::current_path() / "elaboration-bindings-jobs";
  std::filesystem::remove_all(directory);
  synchronous_workflow();
  fixture_completion();
  async_submission(argv[1], directory);
  std::filesystem::remove_all(directory);
  std::cout
      << "native elaboration, immutable fixture admission and ordered completion checks passed\n";
}
