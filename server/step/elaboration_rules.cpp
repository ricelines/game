#include "elaboration_private.hpp"

namespace amber::step {
step::NativeCompletion answer_rule(amber::elaborate::State &elaborate_state,
                                   const catalog::Scripts &scripts,
                                   elaborate::FixtureWorker &worker, std::string runtime,
                                   elaborate::FixtureLimits limits, bool is_elaborator,
                                   Handle requester_body, const step::NativeCall &call) {
  auto &requests = elaborate_state.requests;
  auto &pending = elaborate_state.fixtures;
  auto &captures = elaborate_state.captures;

  if (!call.order || !call.order->input || !call.order->session)
    throw std::invalid_argument("rule answer requires a direct ordered input");
  const auto role = elaboration_detail::access(is_elaborator, call);
  const auto id = elaboration_detail::request_id(call.arguments);
  const auto script_id = step::identifier(call.arguments.at("script"), "s:");
  const auto &script = elaboration_detail::script(scripts, script_id);
  elaborate::RuleCandidate candidate{script.artifact, script.registry_pin, script.id,
                                     runtime,         script.tests,        {}};
  if (call.arguments.contains("subjects"))
    for (const auto &[name, value] : call.arguments.at("subjects").items()) {
      const auto h = parse_handle(value.get<std::string>());
      if (!h)
        throw std::invalid_argument("fixture aliases need complete object handles");
      candidate.subjects.emplace(name, *h);
    }
  const auto &request = elaboration_detail::request(requests, id);
  if (request.requester && requester_body)
    candidate.subjects["body"] = requester_body;
  const auto job = elaborate::begin_rule_answer(
      pending, requests, captures, role, id, std::move(candidate),
      {std::to_string(call.order->session), call.order->sequence}, limits);
  try {
    worker.submit(job);
  } catch (...) {
    elaborate::discard_fixture(pending, job.id);
    throw;
  }
  auto out = elaboration_detail::result(false, {{"request", "request:" + std::to_string(id)},
                                                {"effect", "started"},
                                                {"fixture_job", job.id}});
  out.result = record::Result::None;
  out.deferred = true;
  return out;
}
FixtureCompletion complete_fixture(amber::elaborate::State &elaborate_state,
                                   amber::step::State &step_state, catalog::Scripts &scripts,
                                   amber::react::State &react_state, const react::ValueRead &values,
                                   elaborate::RoleAccess current_role,
                                   const elaborate::FixtureResult &result,
                                   const amber::step::Clock &clock) {

  const auto now = clock.step;

  auto &requests = elaborate_state.requests;
  auto &pending = elaborate_state.fixtures;
  auto &inputs = step_state.inputs;

  auto &queues = step_state.queues;

  FixtureCompletion out;
  const auto found = pending.rows.find(result.job);
  if (found == pending.rows.end())
    return out;
  const auto job = found->second;
  auto input = std::find_if(inputs.rows.begin(), inputs.rows.end(), [&](const auto &entry) {
    return entry.second.awaiting && std::to_string(entry.second.session) == job.order.session &&
           entry.second.sequence == job.order.sequence;
  });
  if (input == inputs.rows.end()) {
    elaborate::discard_fixture(pending, job.id);
    return out;
  }
  const auto input_id = input->first;
  out.origin = input->second.origin;
  std::string error;
  auto check = elaborate::check_fixture_completion(
      pending, requests, {job.actor, current_role.player == job.actor && current_role.elaborator},
      result);
  if (!check.accepted)
    error = check.error;
  try {
    const auto *player = std::get_if<PlayerId>(&input->second.caller);
    if (!player || *player != job.actor)
      throw std::invalid_argument("fixture input actor changed");
    const auto &script = elaboration_detail::script(scripts, job.candidate.script);
    if (script.artifact != job.candidate.artifact ||
        script.registry_pin != job.candidate.registry || script.tests != job.candidate.tests)
      throw std::invalid_argument("immutable candidate identity or tests changed");
    if (error.empty()) {
      const auto before = elaboration_detail::activation_state(scripts);
      if (react::update_rule(scripts, react_state, queues, values, script.id,
                             react::RuleMode::Validated, clock, out.origin, out.effects))
        out.changed_scripts = elaboration_detail::changed_activations(scripts, before);
      out.notification = elaborate::close_rule_answer(requests, pending, check, now);
      out.effects.changed = true;
    }
  } catch (const std::exception &problem) {
    error = problem.what();
  }
  Json reply = {{"ok", error.empty()},
                {"effect", out.effects.changed ? "applied" : "none"},
                {"request", "request:" + std::to_string(job.request)},
                {"fixture", elaborate::result_json(result)}};
  if (!error.empty()) {
    reply["error"] = "fixture_rejected";
    reply["problem"] = error;
    elaborate::discard_fixture(pending, job.id);
  }
  record::Event event;
  event.step = now;
  event.kind = error.empty() ? record::Kind::Registry : record::Kind::Denial;
  event.actor = record::Actor::player(job.actor);
  event.name = error.empty() ? "rule_answer" : "fixture_rejected";
  event.changed = out.effects.changed;
  event.result = event.changed ? record::Result::Applied : record::Result::Denied;
  event.things = {elaboration_detail::request(requests, job.request).target};
  event.detail = {{"request", job.request},
                  {"script", job.candidate.script},
                  {"response", reply},
                  {"maker", "p:" + std::to_string(scripts.rows.at(job.candidate.script).author)},
                  {"created", scripts.rows.at(job.candidate.script).created_s}};
  out.effects.ordered.emplace_back(std::move(event));
  out.input = step::complete_input(inputs, input_id, std::move(reply));
  return out;
}
} // namespace amber::step
