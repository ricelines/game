#include "catalog/catalog.hpp"
#include "runtime_fixture.hpp"
#include "step/step.hpp"

#include <iostream>

using namespace amber;
using Json = nlohmann::json;
using test_runtime::require;
namespace {
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
  return Json::array(
      {{{"given", {{"subject", "target"}}},
        {"when", {{"steps", 1}}},
        {"expect", {{"subject", "target"}, {"term", "novel.heat"}, {"equals", 7}}}}});
}
struct Fixture {
  amber::elaborate::State elaborate_state;
  amber::catalog::State catalog_state;
  amber::step::State step_state;

  test_runtime::Fixture reactive;
  elaborate::Requests &requests = elaborate_state.requests;
  elaborate::GapCounts &gaps = elaborate_state.gap_counts;
  elaborate::LastGaps &last = elaborate_state.last_gaps;
  elaborate::CaptureRefs &captures = elaborate_state.captures;
  elaborate::PendingFixtures &pending = elaborate_state.fixtures;
  catalog::AuthoredTexts &texts = catalog_state.authored_text;
  step::Inputs &inputs = step_state.inputs;
  step::WorkQueues &queues = step_state.queues;
  ScriptId candidate;
  Handle target = reactive.a, body = reactive.b;
  Fixture() {
    candidate = reactive.add(R"(return {kind='rule',applies_to={material='novel.mat'},
      on={['novel.cue']=function(self)end},tests={{given={subject='target'},when={steps=1},
      expect={subject='target',term='novel.heat',equals=7}}}})");
    auto &script = reactive.scripts.rows[candidate];
    script.declaration.approved = false;
    script.declaration.active = false;
    react::set_activation(reactive.vm, candidate, false, false);
    elaborate::note_gap(gaps, last, requests, {8, {9, 10}, target, 1, 11, std::string(64, 'a'), {}},
                        0);
    elaborate::complete_capture(captures, {std::string(64, 'a'), "normal", {}});
  }
  step::NativeCall call(PlayerId who, Json args) {
    step::NativeCall out;
    out.request.caller = who;
    out.arguments = std::move(args);
    out.step = 20;
    out.hz = 20;
    out.origin.cause = 77;
    return out;
  }
  elaborate::RequestId request() {
    const auto out =
        step::request_model(requests, last, call(1, {{"note", "Model this behavior"}}));
    require(out.changed && out.records[0].things[0] == target,
            "request did not retain actual historical target");
    return static_cast<elaborate::RequestId>(requests.rows.size() - 1);
  }
  void claim(elaborate::RequestId id) {
    step::claim(requests, true, call(2, {{"request", "request:" + std::to_string(id)}}));
  }
  std::pair<elaborate::FixtureJob, elaborate::FixtureResult> pending_job(elaborate::RequestId id) {
    claim(id);
    const auto &script = reactive.scripts.rows[candidate];
    elaborate::RuleCandidate declaration{script.artifact, script.registry_pin, candidate,
                                         "runtime",       script.tests,        {{"body", body}}};
    auto job = elaborate::begin_rule_answer(pending, requests, captures, {2, true}, id, declaration,
                                            {"30", 4}, {1000, 64, 65536});
    step::Input input;
    input.kind = net::ControlKind::Do;
    input.caller = PlayerId{2};
    input.session = 30;
    input.sequence = 4;
    input.request = 98;
    input.origin.cause = 123;
    const auto work = step::enqueue_input(inputs, input);
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
  step::FixtureCompletion complete(const elaborate::FixtureResult &result, Step now,
                                   bool has_role = true) {
    return step::complete_fixture(elaborate_state, step_state, reactive.scripts,
                                  reactive.react_state, reactive.values(), {2, has_role}, result,
                                  amber::step::Clock{.step = now, .hz = 20});
  }
};
void synchronous_workflow() {
  Fixture f;
  const auto id = f.request();
  rejects([&] { step::requests(f.requests, f.gaps, false, 10, f.call(1, Json::object())); },
          "listing requires current role");
  require(step::requests(f.requests, f.gaps, true, 10, f.call(2, Json::object()))
                  .reply["requests"]
                  .size() == 1,
          "request list did not expose exact unresolved rows");
  f.claim(id);
  const Json answer{{"request", "request:1"}, {"kind", "detail"}, {"text", "Authored explanation"}};
  rejects([&] { step::answer_detail(f.requests, f.texts, true, f.call(3, answer)); },
          "foreign claim cannot close");
  require(f.texts.rows.size() == 1, "failed authorization appended orphan authored text");
  const auto out = step::answer_detail(f.requests, f.texts, true, f.call(2, answer));
  require(out.action.changed && f.texts.rows.size() == 2 && f.texts.rows[1].author == 2 &&
              f.texts.rows[1].target == f.target &&
              f.requests.rows[id].status == elaborate::Status::Answered,
          "detail did not retain target attribution or close the actual request");
  react::NativeActions native;
  const auto declarations = step::declare_elaboration(f.reactive.terms, native);
  require(!step::check_input(
               native.rows.at(declarations.answer).inputs_schema,
               {{"request", "request:1"}, {"kind", "rule"}, {"script", "s:1"}, {"tests", tests()}})
               .empty(),
          "caller supplied fixture expectations were accepted");
}
void fixture_completion() {
  Fixture f;
  const auto id = f.request();
  const auto [job, result] = f.pending_job(id);
  (void)job;
  const auto completed = f.complete(result, 40);
  const auto &script = f.reactive.scripts.rows[f.candidate];
  require(completed.input && completed.input->session == 30 && completed.input->sequence == 4 &&
              completed.input->request == 98 && completed.input->fields["ok"] == true &&
              completed.notification->requester == 1 && script.declaration.approved &&
              script.declaration.active && completed.origin.cause == 123 &&
              f.requests.rows[id].status == elaborate::Status::Answered && f.inputs.rows.empty() &&
              completed.changed_scripts == std::vector<ScriptId>{f.candidate},
          "verified completion failed actual rule activation or exact original reply routing");
  require(!f.complete(result, 41).input,
          "duplicate subprocess completion reactivated or sent a second reply");
  for (int mode = 0; mode < 4; ++mode) {
    Fixture bad;
    const auto request = bad.request();
    auto [unused, report] = bad.pending_job(request);
    (void)unused;
    if (mode == 0)
      report.tests[0].baseline.passed = true;
    if (mode == 2)
      bad.requests.rows[request].claimed_by = 3;
    if (mode == 3)
      bad.reactive.scripts.rows[bad.candidate].tests = Json::array();
    const auto denied = bad.complete(report, 50, mode != 1);
    require(denied.input && denied.input->fields["ok"] == false &&
                !bad.reactive.scripts.rows[bad.candidate].declaration.approved &&
                bad.requests.rows[request].status == elaborate::Status::Claimed &&
                bad.pending.rows.empty(),
            "vacuous fixture, revoked role, changed claim or candidate activated or closed");
  }
}
void async_submission(const std::filesystem::path &executable, const std::filesystem::path &dir) {
  Fixture f;
  const auto id = f.request();
  f.claim(id);
  elaborate::FixtureWorker worker(executable, dir, 1, AMBER_CONFIG_PATH, "production");
  auto call = f.call(2, {{"request", "request:1"}, {"kind", "rule"}, {"script", "s:1"}});
  const auto answer = [&] {
    return step::answer_rule(f.elaborate_state, f.reactive.scripts, worker, "transport-probe",
                             {1000, 64, 65536}, true, f.body, call);
  };
  rejects(answer, "rule answer requires explicit ordered input");
  call.order = step::InputOrder{9, 30, 4, 98};
  for (const auto *alias : {"", "target", "body"}) {
    call.arguments["subjects"] = {{alias, to_string(f.target)}};
    rejects(answer, "rule answer accepted an empty or server-owned subject alias");
    require(f.pending.rows.empty(), "invalid subject alias started a fixture job");
  }
  call.arguments.erase("subjects");
  const auto started = answer();
  const auto &job = f.pending.rows.begin()->second;
  require(started.deferred && !started.changed &&
              job.candidate.tests == f.reactive.scripts.rows[f.candidate].tests &&
              job.candidate.subjects.at("target") == f.target &&
              job.candidate.subjects.at("body") == f.body,
          "async worker lost immutable verified tests or actual historical subjects");
  worker.finish();
  const auto reports = worker.take();
  require(reports.size() == 1 && reports[0].status == elaborate::FixtureStatus::Completed,
          "copied fixture subprocess protocol failed (transport probe only)");
  auto change = f.call(2, {{"script", "s:1"}, {"active", true}});
  const auto activate = [&] {
    return step::rule_activation(f.reactive.scripts, f.reactive.react_state, f.queues,
                                 f.reactive.values(), true, change);
  };
  rejects(activate, "manual enable approved untested code");
  f.reactive.scripts.rows[f.candidate].declaration.approved = true;
  require(activate().changed && f.reactive.scripts.rows[f.candidate].declaration.active,
          "approved historical rule enable did not update the actual registry/VM");
  change.arguments["active"] = false;
  activate();
  require(!f.reactive.scripts.rows[f.candidate].declaration.active,
          "disable failed to change the actual approved rule");
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    const auto directory = std::filesystem::current_path() / "elaboration-bindings-jobs";
    std::filesystem::remove_all(directory);
    synchronous_workflow();
    fixture_completion();
    async_submission(argv[1], directory);
    std::filesystem::remove_all(directory);
    std::cout
        << "Direct elaboration, actual rule activation and subprocess completion checks passed\n";
  } catch (const std::exception &problem) {
    std::cerr << problem.what() << '\n';
    return 1;
  }
}
