#include "private.hpp"

namespace amber::bindings::commerce {
drive::NativeCompletion pay(void *local, const drive::NativeCall &call) {
  auto &binding = *static_cast<PaymentBinding *>(local);
  const auto from =
      call.arguments.contains("from")
          ? detail::player_id(call.arguments.at("from"), binding.players)
          : detail::default_wallet(call, binding.identity, binding.authority, binding.players);
  const auto to = detail::player_id(call.arguments.at("to"), binding.players);
  const auto transfer = perm::pay(binding.ledger, binding.grants, detail::actor(call), from, to,
                                  call.arguments.at("amount").get<std::int64_t>(), call.step);
  if (!transfer)
    return detail::denied(transfer.reason);
  auto out = detail::result(transfer.amount != 0, {{"amount", transfer.amount},
                                                   {"from", "p:" + std::to_string(from)},
                                                   {"to", "p:" + std::to_string(to)}});
  if (transfer.amount)
    detail::record(out, call, "payment", {}, out.reply);
  return out;
}
drive::NativeCompletion balance(void *local, const drive::NativeCall &call) {
  const auto &binding = *static_cast<const BalanceBinding *>(local);
  const auto account =
      call.arguments.contains("account")
          ? detail::player_id(call.arguments.at("account"), binding.players)
          : detail::default_wallet(call, binding.identity, binding.authority, binding.players);
  return detail::result(false, {{"account", "p:" + std::to_string(account)},
                                {"balance", perm::balance(binding.ledger, account)}});
}
drive::NativeCompletion inventory(void *local, const drive::NativeCall &call) {
  const auto &binding = *static_cast<const InventoryBinding *>(local);
  const auto id = detail::player(call);
  if (id >= binding.players.rows.size())
    return detail::denied(perm::Reason::UnknownHolder);
  const auto &player = binding.players.rows[id];
  return detail::result(false, {{"inventory", detail::handles(player.inventory)},
                                {"mailbox", detail::handles(player.mailbox)}});
}
} // namespace amber::bindings::commerce
