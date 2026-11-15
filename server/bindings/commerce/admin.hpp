#pragma once

#include "bindings/authority/descriptors.hpp"
#include "drive/drive.hpp"

namespace amber::bindings::commerce {
using Json = nlohmann::json;
struct AdminDeclarations {
  TermId grant = 0, revoke = 0, list_grants = 0, preset = 0, group_create = 0, group_member = 0,
         plot_permissions = 0;
};
AdminDeclarations declare_admin(react::Terms &, command::NativeActions &);
// Permission edits are rare. The composition refreshes its permission-dependent views here.
struct AdminInvalidation {
  void *local = nullptr;
  void (*changed)(void *) = nullptr;
};
struct GrantMaintenance {
  perm::Grants &grants;
  perm::Players &players;
  AdminInvalidation invalidation;
};
struct GrantNotice {
  PlayerId recipient;
  Json event;
};
struct GrantUpdates {
  std::vector<GrantNotice> notices;
  std::vector<record::Event> records;
};
GrantUpdates advance_grants(GrantMaintenance &, Step now, Step reminder_steps, double hz,
                            std::size_t max_entries);
struct GrantBinding {
  perm::Grants &grants;
  perm::Players &players;
  authority::ScopeRead scopes;
  const catalog::Models &models;
  const catalog::Scripts &scripts;
  const install::InstanceScripts &instances;
  const react::RuleVersions &rules;
  const react::ValueRead &values;
  AdminInvalidation invalidation;
};
struct RevokeBinding {
  perm::Grants &grants;
  perm::Players &players;
  const control::Profiles &profiles;
  AdminInvalidation invalidation;
};
struct ListGrantsBinding {
  const perm::Grants &grants;
  const perm::Players &players;
};
struct PresetColumns {
  std::vector<GroupId> &group;
  std::vector<std::uint32_t> &preset;
};
struct PresetBinding {
  PresetColumns columns;
  PermissionFlags flags;
  IdentityRead identity;
  AuthorityRead authority;
  const perm::OwnerLimits &limits;
  const perm::Players &players;
  const perm::Groups &groups;
  AdminInvalidation invalidation;
};
struct GroupCreateBinding {
  perm::Groups &groups;
  perm::Players &players;
  AdminInvalidation invalidation;
};
struct GroupMemberBinding {
  const perm::Groups &groups;
  perm::Players &players;
  AdminInvalidation invalidation;
};
struct PlotPermissionsBinding {
  perm::Plots &plots;
  const perm::Players &players;
  const perm::Groups &groups;
  AdminInvalidation invalidation;
};
std::string validate_grant(const Json &);
std::string validate_revoke(const Json &);
std::string validate_list_grants(const Json &);
std::string validate_preset(const Json &);
std::string validate_group_create(const Json &);
std::string validate_group_member(const Json &);
std::string validate_plot_permissions(const Json &);
drive::NativeCompletion grant(void *, const drive::NativeCall &);
drive::NativeCompletion revoke(void *, const drive::NativeCall &);
drive::NativeCompletion list_grants(void *, const drive::NativeCall &);
drive::NativeCompletion preset(void *, const drive::NativeCall &);
drive::NativeCompletion group_create(void *, const drive::NativeCall &);
drive::NativeCompletion group_member(void *, const drive::NativeCall &);
drive::NativeCompletion plot_permissions(void *, const drive::NativeCall &);
} // namespace amber::bindings::commerce
