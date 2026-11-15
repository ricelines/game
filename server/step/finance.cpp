#include "step.hpp"

namespace amber::step {
std::string validate_issue(const nlohmann::json &input) {
  if (!input.is_object() || input.size() != 2 || !input.contains("to") ||
      !input.at("to").is_string() || !input.contains("amount") ||
      !input.at("amount").is_number_integer())
    return "expected destination account and whole amount";
  if ((input.at("amount").is_number_unsigned() &&
       input.at("amount").get<std::uint64_t>() > INT64_MAX) ||
      input.at("amount").get<std::int64_t>() < 0)
    return "amount must be nonnegative whole currency units";
  return {};
}
step::NativeCompletion issue(perm::Ledger &ledger, perm::Grants &grants,
                             const perm::Players &players, IdentityRead identity,
                             AuthorityRead authority, const perm::Groups &groups,
                             const step::NativeCall &call) {
  const auto to =
      step::account(call.arguments.at("to"), identity, authority, players, groups).account;
  const auto holder =
      std::holds_alternative<PlayerId>(call.request.caller)
          ? perm::Holder::player(std::get<PlayerId>(call.request.caller))
          : perm::Holder::script(std::get<ScriptHolder>(call.request.caller).script,
                                 std::get<ScriptHolder>(call.request.caller).object);
  const auto transfer = perm::issue(ledger, grants, holder, to,
                                    call.arguments.at("amount").get<std::int64_t>(), call.step);
  step::NativeCompletion out;
  out.changed = bool(transfer) && transfer.amount > 0;
  out.result = !transfer     ? record::Result::Denied
               : out.changed ? record::Result::Applied
                             : record::Result::NoEffect;
  out.reply = {{"ok", bool(transfer)},
               {"effect", out.changed ? "applied" : "none"},
               {"amount", transfer.amount},
               {"reason", perm::reason_text(transfer.reason)}};
  if (!transfer)
    out.reply["denied"] = {
        {"need", "issuance"},
        {"on", perm::account_name(to)},
        {"why", perm::reason_text(transfer.reason)},
        {"fix", "ask the architect for an unexpired issuance grant with enough budget"}};
  return out;
}
void declare_issuance_action(react::Terms &terms, react::NativeActions &native) {
  react::declare_native(
      native, terms, "engine.issue", "Issue currency using an explicit issuance grant.", "", false,
      {{"type", "object"},
       {"properties",
        {{"to", {{"type", "string"}}},
         {"amount", {{"type", "integer"}, {"minimum", 0}, {"maximum", INT64_MAX}}}}},
       {"required", {"to", "amount"}},
       {"additionalProperties", false}},
      react::ExchangeAction::Issue);
}
} // namespace amber::step
