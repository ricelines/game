#pragma once

#include "perm/perm.hpp"
#include "react/react.hpp"

namespace amber::bindings {
struct BalanceBinding {
  const perm::Ledger &ledger;
  IdentityRead identity;
  AuthorityRead authority;
  const perm::Players &players;
};
struct PaymentBinding {
  perm::Ledger &ledger;
  perm::Grants &grants;
  const perm::Players &players;
};
struct IssuanceBinding {
  perm::Ledger &ledger;
  perm::Grants &grants;
  const perm::Players &players;
};
int balance_binding(lua_State *);
int payment_binding(lua_State *);
int issuance_binding(lua_State *);
} // namespace amber::bindings
