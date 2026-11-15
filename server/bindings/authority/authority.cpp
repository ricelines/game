#include "authority.hpp"
#include "bindings/arguments/arguments.hpp"

#include <lua.h>
#include <lualib.h>

namespace amber::bindings {
int transfer_plot_binding(lua_State *L) {
  auto &b = *static_cast<PlotTransferBinding *>(react::binding_data(L));
  const auto id = unsigned_arg(L, 2);
  const auto recipient = player_arg(L, 3);
  const auto &inv = react::invocation(L);
  const auto transfer = perm::transfer_plot(b.plots, b.grants, b.players,
                                            perm::Holder::script(inv.self.script, inv.self.object),
                                            id, recipient, inv.step);
  if (transfer && transfer.previous_owner != recipient) {
    if (b.changed)
      b.changed(b.invalidation);
    inv.effects->changed = true;
    record::Event event;
    event.kind = record::Kind::System;
    event.actor = record::Actor::script(inv.self.script);
    event.script_author = inv.declaration->author;
    event.name = "plot_transfer";
    event.result = record::Result::Applied;
    event.changed = true;
    event.things = {inv.self.object};
    event.detail = {{"plot", id},
                    {"from", transfer.previous_owner},
                    {"to", recipient},
                    {"revoked", transfer.revoked}};
    inv.effects->ordered.push_back(std::move(event));
  }
  lua_createtable(L, 0, 2);
  lua_pushboolean(L, bool(transfer));
  lua_setfield(L, -2, "ok");
  const auto reason = perm::reason_text(transfer.reason);
  lua_pushlstring(L, reason.data(), reason.size());
  lua_setfield(L, -2, "reason");
  return 1;
}
} // namespace amber::bindings
