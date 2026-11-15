#include "private.hpp"

namespace amber::bindings::elaboration {
drive::NativeCompletion answer_rule(void *local, const drive::NativeCall &call) {
  auto &b = *static_cast<RuleBinding *>(local);
  if (!call.order || !call.order->input || !call.order->session)
    throw std::invalid_argument("rule answer requires a direct ordered input");
  const auto role = detail::access(b.profiles, call);
  const auto id = detail::request_id(call.arguments);
  const auto script_id = authority::identifier(call.arguments.at("script"), "s:");
  const auto &script = detail::script(b.scripts, script_id);
  elaborate::RuleCandidate candidate{script.artifact, script.registry_pin, script.id,
                                     b.runtime,       script.tests,        {}};
  if (call.arguments.contains("subjects"))
    for (const auto &[name, value] : call.arguments.at("subjects").items()) {
      const auto h = parse_handle(value.get<std::string>());
      if (!h)
        throw std::invalid_argument("fixture aliases need complete object handles");
      candidate.subjects.emplace(name, *h);
    }
  const auto &request = detail::request(b.requests, id);
  if (request.requester && request.requester < b.profiles.rows.size() &&
      b.profiles.rows[request.requester].body)
    candidate.subjects["body"] = b.profiles.rows[request.requester].body;
  const auto job = elaborate::begin_rule_answer(
      b.pending, b.requests, b.captures, role, id, std::move(candidate),
      {std::to_string(call.order->session), call.order->sequence}, b.limits);
  try {
    b.worker.submit(job);
  } catch (...) {
    elaborate::discard_fixture(b.pending, job.id);
    throw;
  }
  auto out = detail::result(false, {{"request", "request:" + std::to_string(id)},
                                    {"effect", "started"},
                                    {"fixture_job", job.id}});
  out.result = record::Result::None;
  out.deferred = true;
  return out;
}
Completion complete(CompletionBinding &b, const elaborate::FixtureResult &result, Step now,
                    double hz) {
  Completion out;
  const auto found = b.pending.rows.find(result.job);
  if (found == b.pending.rows.end())
    return out;
  const auto job = found->second;
  auto input = std::find_if(b.inputs.rows.begin(), b.inputs.rows.end(), [&](const auto &entry) {
    return entry.second.awaiting && std::to_string(entry.second.session) == job.order.session &&
           entry.second.sequence == job.order.sequence;
  });
  if (input == b.inputs.rows.end()) {
    elaborate::discard_fixture(b.pending, job.id);
    return out;
  }
  const auto input_id = input->first;
  out.origin = input->second.origin;
  std::string error;
  auto check = elaborate::check_fixture_completion(
      b.pending, b.requests, {job.actor, detail::role(b.profiles, job.actor, "elaborator")},
      result);
  if (!check.accepted)
    error = check.error;
  try {
    const auto *player = std::get_if<PlayerId>(&input->second.caller);
    if (!player || *player != job.actor)
      throw std::invalid_argument("fixture input actor changed");
    const auto &script = detail::script(b.scripts, job.candidate.script);
    if (script.artifact != job.candidate.artifact ||
        script.registry_pin != job.candidate.registry || script.tests != job.candidate.tests)
      throw std::invalid_argument("immutable candidate identity or tests changed");
    if (error.empty()) {
      if (!b.activation.apply)
        throw std::invalid_argument("rule activation is unavailable");
      b.activation.apply(b.activation.local, script.id, RuleUpdate::Validated, now, hz, out.origin,
                         out.effects);
      out.notification = elaborate::close_rule_answer(b.requests, b.pending, check, now);
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
    elaborate::discard_fixture(b.pending, job.id);
  }
  record::Event event;
  event.step = now;
  event.kind = record::Kind::System;
  event.actor = record::Actor::player(job.actor);
  event.name = error.empty() ? "rule_answer" : "fixture_rejected";
  event.changed = out.effects.changed;
  event.result = event.changed ? record::Result::Applied : record::Result::Denied;
  event.things = {detail::request(b.requests, job.request).target};
  event.detail = {{"request", job.request}, {"script", job.candidate.script}, {"response", reply}};
  out.effects.ordered.emplace_back(std::move(event));
  out.input = drive::complete_input(b.inputs, input_id, std::move(reply));
  return out;
}
} // namespace amber::bindings::elaboration
