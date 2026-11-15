#pragma once

#include "drive/drive.hpp"
#include "perm/perm.hpp"
#include "react/react.hpp"

namespace amber::bindings {
struct IssuanceBinding {
  perm::Ledger &ledger;
  perm::Grants &grants;
  const perm::Players &players;
  IdentityRead identity;
  AuthorityRead authority;
  const perm::Groups &groups;
};
void register_issuance_action(react::Terms &, command::NativeActions &, drive::NativeOperations &,
                              IssuanceBinding &);
} // namespace amber::bindings
