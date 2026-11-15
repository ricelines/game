#include "finance.hpp"

#include <charconv>
#include <lua.h>
#include <lualib.h>

namespace amber::bindings {
namespace {
PlayerId wallet(lua_State *L, int index, const perm::Players &players) {
  std::size_t size = 0;
  const char *text = luaL_checklstring(L, index, &size);
  PlayerId id = 0;
  if (size < 3 || text[0] != 'p' || text[1] != ':')
    luaL_error(L, "wallet must name a player as p:N");
  const auto parsed = std::from_chars(text + 2, text + size, id);
  if (parsed.ec != std::errc{} || parsed.ptr != text + size || !id || id >= players.rows.size())
    luaL_error(L, "wallet player does not exist");
  return id;
}
std::int64_t amount(lua_State *L, int index) {
  const double value = luaL_checknumber(L, index);
  if (!std::isfinite(value) || value < 0 || value >= 0x1p63 || std::trunc(value) != value)
    luaL_error(L, "amount must be nonnegative whole currency units");
  return static_cast<std::int64_t>(value);
}
int result(lua_State *L, const perm::TransferResult &transfer) {
  if (transfer && transfer.amount > 0)
    react::invocation(L).effects->changed = true;
  lua_createtable(L, 0, 3);
  lua_pushboolean(L, bool(transfer));
  lua_setfield(L, -2, "ok");
  lua_pushnumber(L, static_cast<double>(transfer.amount));
  lua_setfield(L, -2, "amount");
  const auto why = perm::reason_text(transfer.reason);
  lua_pushlstring(L, why.data(), why.size());
  lua_setfield(L, -2, "reason");
  return 1;
}
void completed(lua_State *L, const char *operation, PlayerId to, std::optional<PlayerId> from,
               const perm::TransferResult &transfer) {
  if (!transfer || transfer.amount == 0)
    return;
  const auto &inv = react::invocation(L);
  record::Event event;
  event.kind = record::Kind::System;
  event.actor = record::Actor::script(inv.self.script);
  event.name = operation;
  event.changed = true;
  event.result = record::Result::Applied;
  event.things = {inv.self.object};
  if (inv.declaration)
    event.script_author = inv.declaration->author;
  event.detail = {{"to", "p:" + std::to_string(to)}, {"amount", transfer.amount}};
  if (from)
    event.detail["from"] = "p:" + std::to_string(*from);
  inv.effects->ordered.push_back(std::move(event));
}
} // namespace
int balance_binding(lua_State *L) {
  const auto &b = *static_cast<BalanceBinding *>(react::binding_data(L));
  const auto self = react::invocation(L).self.object;
  if (!b.identity.valid(self))
    luaL_error(L, "script object is no longer alive");
  const auto key = lua_isnoneornil(L, 2) ? b.authority.owner[self.slot()] : wallet(L, 2, b.players);
  lua_pushnumber(L, static_cast<double>(perm::balance(b.ledger, key)));
  return 1;
}
int payment_binding(lua_State *L) {
  auto &b = *static_cast<PaymentBinding *>(react::binding_data(L));
  const auto from = wallet(L, 2, b.players);
  const auto to = wallet(L, 3, b.players);
  const auto whole = amount(L, 4);
  const auto &inv = react::invocation(L);
  const auto transfer =
      perm::pay(b.ledger, b.grants, perm::Holder::script(inv.self.script, inv.self.object), from,
                to, whole, inv.step);
  completed(L, "payment", to, from, transfer);
  return result(L, transfer);
}
int issuance_binding(lua_State *L) {
  auto &b = *static_cast<IssuanceBinding *>(react::binding_data(L));
  const auto to = wallet(L, 2, b.players);
  const auto whole = amount(L, 3);
  const auto &inv = react::invocation(L);
  const auto transfer =
      perm::issue(b.ledger, b.grants, perm::Holder::script(inv.self.script, inv.self.object), to,
                  whole, inv.step);
  completed(L, "issuance", to, {}, transfer);
  return result(L, transfer);
}
} // namespace amber::bindings
