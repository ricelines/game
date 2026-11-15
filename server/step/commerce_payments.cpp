#include "commerce_private.hpp"

namespace amber::step {
step::NativeCompletion pay(perm::Ledger &ledger, perm::Grants &grants, const perm::Players &players,
                           IdentityRead identity, AuthorityRead authority,
                           const perm::Groups &groups, const step::NativeCall &call) {
  const auto from =
      call.arguments.contains("from")
          ? step::account(call.arguments.at("from"), identity, authority, players, groups)
          : commerce_detail::default_account(call, identity, authority, players, groups);
  const auto to =
      step::account(call.arguments.at("to"), identity, authority, players, groups).account;
  const auto transfer = perm::pay(ledger, grants, commerce_detail::actor(call), from, to,
                                  call.arguments.at("amount").get<std::int64_t>(), call.step);
  if (!transfer)
    return commerce_detail::denied(transfer.reason, "spend", perm::account_name(from.account),
                                   from.controller);
  auto out =
      commerce_detail::result(transfer.amount != 0, {{"amount", transfer.amount},
                                                     {"from", perm::account_name(from.account)},
                                                     {"to", perm::account_name(to)}});
  if (transfer.amount)
    commerce_detail::record(out, call, "payment", {}, out.reply);
  return out;
}
step::NativeCompletion balance(const perm::Ledger &ledger, const perm::Players &players,
                               IdentityRead identity, AuthorityRead authority,
                               const perm::Groups &groups, const step::NativeCall &call) {
  const auto account =
      call.arguments.contains("account")
          ? step::account(call.arguments.at("account"), identity, authority, players, groups)
          : commerce_detail::default_account(call, identity, authority, players, groups);
  return commerce_detail::result(false, {{"account", perm::account_name(account.account)},
                                         {"balance", perm::balance(ledger, account.account)}});
}
step::NativeCompletion inventory(const perm::Players &players, const step::NativeCall &call) {
  const auto id = commerce_detail::player(call);
  if (id >= players.rows.size())
    return commerce_detail::denied(perm::Reason::UnknownHolder, "account",
                                   "p:" + std::to_string(id));
  const auto &player = players.rows[id];
  return commerce_detail::result(false, {{"inventory", commerce_detail::handles(player.inventory)},
                                         {"mailbox", commerce_detail::handles(player.mailbox)}});
}
} // namespace amber::step
