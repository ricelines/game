#include "private.hpp"

namespace amber::bindings::elaboration {
Json detail::request_json(const elaborate::Request &row, const elaborate::GapCounts &gaps) {
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
drive::NativeCompletion request_model(void *local, const drive::NativeCall &call) {
  auto &b = *static_cast<RequestBinding *>(local);
  const auto id = elaborate::request_model(b.requests, b.last, detail::player(call),
                                           call.arguments.at("note").get<std::string>(), call.step);
  auto out = detail::result(true, {{"request", "request:" + std::to_string(id)}});
  detail::record(out, call, "request_model", b.requests.rows[id]);
  return out;
}
drive::NativeCompletion requests(void *local, const drive::NativeCall &call) {
  const auto &b = *static_cast<const RequestsBinding *>(local);
  const auto rows = elaborate::requests(b.requests, b.gaps, detail::access(b.profiles, call),
                                        call.arguments.value("page", 0u),
                                        call.arguments.value("page_size", b.page_size));
  auto records = Json::array();
  for (const auto &row : rows)
    records.push_back(detail::request_json(row, b.gaps));
  return detail::result(false, {{"requests", std::move(records)}});
}
drive::NativeCompletion claim(void *local, const drive::NativeCall &call) {
  auto &b = *static_cast<ClaimBinding *>(local);
  const auto id = detail::request_id(call.arguments);
  const auto before = detail::request(b.requests, id).status;
  elaborate::claim(b.requests, detail::access(b.profiles, call), id);
  auto out = detail::result(before == elaborate::Status::Open,
                            {{"request", "request:" + std::to_string(id)}});
  detail::record(out, call, "claim", b.requests.rows[id]);
  return out;
}
drive::NativeCompletion answer(void *local, const drive::NativeCall &call) {
  const auto &b = *static_cast<const AnswerDispatch *>(local);
  const auto kind = call.arguments.at("kind").get<std::string>();
  const auto &operation = kind == "rule" ? b.rule : kind == "patch" ? b.patch : b.detail;
  if (!operation.run || !operation.validate)
    throw std::invalid_argument("answer kind is unavailable");
  const auto error = operation.validate(call.arguments);
  if (!error.empty())
    throw std::invalid_argument(error);
  return operation.run(operation.context, call);
}
drive::NativeCompletion answer_patch(void *local, const drive::NativeCall &call) {
  auto &b = *static_cast<PatchBinding *>(local);
  const auto id = detail::request_id(call.arguments);
  const auto closed =
      elaborate::answer_patch(b.requests, detail::access(b.profiles, call), id,
                              call.arguments.at("note").get<std::string>(), call.step);
  auto out = detail::result(
      true,
      {{"request", "request:" + std::to_string(id)}, {"kind", "patch"}, {"text", closed.text}});
  detail::record(out, call, "answer_patch", b.requests.rows[id]);
  detail::notify(b.notification, closed);
  return out;
}
drive::NativeCompletion answer_detail(void *local, const drive::NativeCall &call) {
  auto &b = *static_cast<DetailBinding *>(local);
  const auto id = detail::request_id(call.arguments);
  const auto role = detail::access(b.profiles, call);
  const auto text = call.arguments.at("text").get<std::string>();
  const auto next = catalog::TextId(b.texts.rows.size());
  const auto closed = elaborate::answer_detail(b.requests, role, id, next, text, call.step);
  catalog::append_authored_text(b.texts, b.requests.rows[id].target, role.player,
                                call.step / call.hz, catalog::AuthoredTextKind::Detail, text);
  auto out = detail::result(
      true, {{"request", "request:" + std::to_string(id)}, {"kind", "detail"}, {"text", text}});
  detail::record(out, call, "answer_detail", b.requests.rows[id]);
  detail::notify(b.notification, closed);
  return out;
}
drive::NativeCompletion refuse(void *local, const drive::NativeCall &call) {
  auto &b = *static_cast<DetailBinding *>(local);
  const auto id = detail::request_id(call.arguments);
  const auto role = detail::access(b.profiles, call);
  const auto text = call.arguments.at("text").get<std::string>();
  const auto next = catalog::TextId(b.texts.rows.size());
  const auto closed = elaborate::refuse(b.requests, role, id, next, text, call.step);
  catalog::append_authored_text(b.texts, b.requests.rows[id].target, role.player,
                                call.step / call.hz, catalog::AuthoredTextKind::Refusal, text);
  auto out = detail::result(
      true, {{"request", "request:" + std::to_string(id)}, {"kind", "refusal"}, {"text", text}});
  detail::record(out, call, "refuse", b.requests.rows[id]);
  detail::notify(b.notification, closed);
  return out;
}
drive::NativeCompletion rule_activation(void *local, const drive::NativeCall &call) {
  auto &b = *static_cast<ActivationBinding *>(local);
  const auto who = detail::player(call);
  if (!detail::role(b.profiles, who, "elaborator") && !detail::role(b.profiles, who, "architect"))
    throw std::invalid_argument("registry editing requires elaborator or architect role");
  const auto id = authority::identifier(call.arguments.at("script"), "s:");
  const auto &script = detail::script(b.scripts, id);
  if (!script.declaration.approved)
    throw std::invalid_argument("rule has not passed fixture approval");
  if (!b.activation.apply)
    throw std::invalid_argument("rule activation is unavailable");
  auto out = detail::result(false);
  b.activation.apply(b.activation.local, id,
                     call.arguments.at("active").get<bool>() ? RuleUpdate::Enable
                                                             : RuleUpdate::Disable,
                     call.step, call.hz, call.origin, out.effects);
  out.changed = out.effects.changed;
  out.result = out.changed ? record::Result::Applied : record::Result::NoEffect;
  out.reply = {{"ok", true},
               {"effect", out.changed ? "applied" : "none"},
               {"script", "s:" + std::to_string(id)},
               {"active", call.arguments.at("active")}};
  return out;
}
} // namespace amber::bindings::elaboration
