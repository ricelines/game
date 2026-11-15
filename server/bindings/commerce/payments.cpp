#include "private.hpp"

namespace amber::bindings::commerce {
drive::NativeCompletion pay(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<PaymentBinding *>(local);
  const authority::AccountRead accounts{binding.identity, binding.authority, binding.players,
                                        binding.groups};
  const auto from = call.arguments.contains("from")
                        ? authority::account(call.arguments.at("from"), accounts)
                        : detail::default_account(call, accounts);
  const auto to = authority::account(call.arguments.at("to"), accounts).account;
  const auto transfer = perm::pay(binding.ledger, binding.grants, detail::actor(call), from, to,
                                  call.arguments.at("amount").get<std::int64_t>(), call.step);
  if (!transfer)
    return detail::denied(transfer.reason, "spend", perm::account_name(from.account),
                          from.controller);
  auto out = detail::result(transfer.amount != 0, {{"amount", transfer.amount},
                                                   {"from", perm::account_name(from.account)},
                                                   {"to", perm::account_name(to)}});
  if (transfer.amount)
    detail::record(out, call, "payment", {}, out.reply);
  return out;
}
drive::NativeCompletion balance(void *local, const drive::NativeCall &call) {
  const auto &binding = *static_cast<const BalanceBinding *>(local);
  const authority::AccountRead accounts{binding.identity, binding.authority, binding.players,
                                        binding.groups};
  const auto account = call.arguments.contains("account")
                           ? authority::account(call.arguments.at("account"), accounts)
                           : detail::default_account(call, accounts);
  return detail::result(false, {{"account", perm::account_name(account.account)},
                                {"balance", perm::balance(binding.ledger, account.account)}});
}
drive::NativeCompletion inventory(void *local, const drive::NativeCall &call) {
  const auto &binding = *static_cast<const InventoryBinding *>(local);
  const auto id = detail::player(call);
  if (id >= binding.players.rows.size())
    return detail::denied(perm::Reason::UnknownHolder, "account", "p:" + std::to_string(id));
  const auto &player = binding.players.rows[id];
  return detail::result(false, {{"inventory", detail::handles(player.inventory)},
                                {"mailbox", detail::handles(player.mailbox)}});
}
} // namespace amber::bindings::commerce
