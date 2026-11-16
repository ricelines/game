#include "catalog/catalog.hpp"
#include "elaboration_private.hpp"

namespace amber::step {
Json elaboration_detail::request_json(const elaborate::Request &row,
                                      const elaborate::GapCounts &gaps) {
  const auto count = gaps.rows.find(row.gap);
  constexpr const char *status[]{"open", "claimed", "answered", "refused"};
  return {
      {"id", "request:" + std::to_string(row.id)},
      {"requester", row.requester ? Json("p:" + std::to_string(row.requester)) : Json(nullptr)},
      {"action", row.action},
      {"term", row.gap.term},
      {"material", row.gap.material},
      {"target", to_string(row.target)},
      {"capture", row.capture},
      {"status", status[static_cast<unsigned>(row.status)]},
      {"claimed_by", row.claimed_by ? Json("p:" + std::to_string(row.claimed_by)) : Json(nullptr)},
      {"created_step", row.created_step},
      {"gap_count", count == gaps.rows.end() ? 0 : count->second},
      {"note", row.note},
      {"display", row.display}};
}
step::NativeCompletion request_model(elaborate::Requests &requests, const elaborate::LastGaps &last,
                                     const step::NativeCall &call) {
  const auto id = elaborate::request_model(requests, last, elaboration_detail::player(call),
                                           call.arguments.at("note").get<std::string>(), call.step);
  auto out = elaboration_detail::result(true, {{"request", "request:" + std::to_string(id)}});
  elaboration_detail::record(out, call, "request_model", requests.rows[id]);
  return out;
}
step::NativeCompletion requests(const elaborate::Requests &requests,
                                const elaborate::GapCounts &gaps, bool is_elaborator,
                                std::uint32_t page_size, const step::NativeCall &call) {
  const auto rows = elaborate::requests(
      requests, gaps, elaboration_detail::access(is_elaborator, call),
      call.arguments.value("page", 0u), call.arguments.value("page_size", page_size));
  auto records = Json::array();
  for (const auto &row : rows)
    records.push_back(elaboration_detail::request_json(row, gaps));
  return elaboration_detail::result(false, {{"requests", std::move(records)}});
}
step::NativeCompletion claim(elaborate::Requests &requests, bool is_elaborator,
                             const step::NativeCall &call) {
  const auto id = elaboration_detail::request_id(call.arguments);
  const auto before = elaboration_detail::request(requests, id).status;
  elaborate::claim(requests, elaboration_detail::access(is_elaborator, call), id);
  auto out = elaboration_detail::result(before == elaborate::Status::Open,
                                        {{"request", "request:" + std::to_string(id)}});
  elaboration_detail::record(out, call, "claim", requests.rows[id]);
  return out;
}
AnswerCompletion answer_patch(elaborate::Requests &requests, bool is_elaborator,
                              const step::NativeCall &call) {
  const auto id = elaboration_detail::request_id(call.arguments);
  const auto closed =
      elaborate::answer_patch(requests, elaboration_detail::access(is_elaborator, call), id,
                              call.arguments.at("note").get<std::string>(), call.step);
  auto out = elaboration_detail::result(
      true,
      {{"request", "request:" + std::to_string(id)}, {"kind", "patch"}, {"text", closed.text}});
  elaboration_detail::record(out, call, "answer_patch", requests.rows[id]);
  return {std::move(out), closed};
}
AnswerCompletion answer_detail(elaborate::Requests &requests, catalog::AuthoredTexts &texts,
                               bool is_elaborator, const step::NativeCall &call) {
  const auto id = elaboration_detail::request_id(call.arguments);
  const auto role = elaboration_detail::access(is_elaborator, call);
  const auto text = call.arguments.at("text").get<std::string>();
  const auto next = catalog::TextId(texts.rows.size());
  const auto closed = elaborate::answer_detail(requests, role, id, next, text, call.step);
  catalog::append_authored_text(texts, requests.rows[id].target, role.player, call.step / call.hz,
                                catalog::AuthoredTextKind::Detail, text);
  auto out = elaboration_detail::result(
      true, {{"request", "request:" + std::to_string(id)}, {"kind", "detail"}, {"text", text}});
  elaboration_detail::record(out, call, "answer_detail", requests.rows[id]);
  return {std::move(out), closed};
}
AnswerCompletion refuse(elaborate::Requests &requests, catalog::AuthoredTexts &texts,
                        bool is_elaborator, const step::NativeCall &call) {
  const auto id = elaboration_detail::request_id(call.arguments);
  const auto role = elaboration_detail::access(is_elaborator, call);
  const auto text = call.arguments.at("text").get<std::string>();
  const auto next = catalog::TextId(texts.rows.size());
  const auto closed = elaborate::refuse(requests, role, id, next, text, call.step);
  catalog::append_authored_text(texts, requests.rows[id].target, role.player, call.step / call.hz,
                                catalog::AuthoredTextKind::Refusal, text);
  auto out = elaboration_detail::result(
      true, {{"request", "request:" + std::to_string(id)}, {"kind", "refusal"}, {"text", text}});
  elaboration_detail::record(out, call, "refuse", requests.rows[id]);
  return {std::move(out), closed};
}
step::NativeCompletion rule_activation(catalog::Scripts &scripts, amber::react::State &react_state,
                                       step::WorkQueues &queues, const react::ValueRead &values,
                                       bool may_edit_rules, const step::NativeCall &call) {

  elaboration_detail::player(call);
  if (!may_edit_rules)
    throw std::invalid_argument("registry editing requires elaborator or architect role");
  const auto id = step::identifier(call.arguments.at("script"), "s:");
  const auto &script = elaboration_detail::script(scripts, id);
  if (!script.declaration.approved)
    throw std::invalid_argument("rule has not passed fixture approval");
  auto out = elaboration_detail::result(false);
  const auto before = elaboration_detail::activation_state(scripts);
  const auto changed = react::update_rule(
      scripts, react_state, queues.background, values, id,
      call.arguments.at("active").get<bool>() ? react::RuleMode::Enable : react::RuleMode::Disable,
      call.step, call.hz, call.origin, out.effects);
  if (changed)
    out.changed_scripts = elaboration_detail::changed_activations(scripts, before);
  out.changed = out.effects.changed;
  out.result = out.changed ? record::Result::Applied : record::Result::NoEffect;
  out.reply = {{"ok", true},
               {"effect", out.changed ? "applied" : "none"},
               {"script", "s:" + std::to_string(id)},
               {"active", call.arguments.at("active")}};
  return out;
}
} // namespace amber::step
