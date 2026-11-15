#pragma once
#include "causality.hpp"
#include "observe/observe.hpp"
#include "perm/perm.hpp"

namespace amber::bindings {
struct NavigationBinding {
  step::Intents &intents;
  std::vector<step::IntentEnding> &endings;
  IdentityRead identity;
  AuthorityRead authority;
  const perm::OwnerLimits &limits;
  const perm::Players &players;
  const perm::Grants &grants;
  const perm::Plots &plots;
  const Relation &declared_parts;
  struct PendingOrigin {
    Handle body;
    step::IntentId intent;
    std::optional<std::size_t> ending;
  };
  std::uint64_t next_origin = 1;
  std::unordered_map<std::uint64_t, PendingOrigin> pending_origins{};
};
struct AttachmentBinding {
  space::Space &space;
  IdentityRead identity;
  TransformWrite transforms;
  ParentWrite parents;
  SpaceFlags flags;
  space::PhysicalRelations &relations;
  PhysicalRelationTerms relation_terms;
  const Relation &declared_parts;
  AuthorityRead authority;
  const perm::OwnerLimits &limits;
  const perm::Players &players;
  const perm::Grants &grants;
  float reach_m;
  MechanicalCauses &causes;
};
struct ImpulseBinding {
  space::Space &space;
  IdentityRead identity;
  AuthorityRead authority;
  const perm::OwnerLimits &limits;
  const perm::Players &players;
  const perm::Grants &grants;
  const Relation &declared_parts;
  MechanicalCauses &causes;
};
struct SweepBinding {
  space::Space &space;
  IdentityRead identity;
  TransformWrite transforms;
  AuthorityRead authority;
  const perm::OwnerLimits &limits;
  const perm::Players &players;
  const perm::Grants &grants;
  const Relation &declared_parts;
  MechanicalCauses &causes;
};
struct PoseBatchBinding {
  space::Space &space;
  IdentityRead identity;
  TransformWrite transforms;
  AuthorityRead authority;
  const perm::OwnerLimits &limits;
  const perm::Players &players;
  const perm::Grants &grants;
  const Relation &declared_parts;
  std::size_t batch_max;
  MechanicalCauses &causes;
};
struct PartPoseBinding {
  space::Space &space;
  IdentityRead identity;
  TransformWrite transforms;
  const Relation &declared_parts;
  MechanicalCauses &causes;
};
struct QuantityBinding {
  react::Properties &properties;
  react::Processes &processes;
  const react::Terms &terms;
  IdentityRead identity;
  AuthorityRead authority;
  ParentRead parents;
  const Relation &declared_parts;
  const Relation &grasped;
  const perm::OwnerLimits &limits;
  const perm::Players &players;
  const perm::Grants &grants;
};
struct PositionBinding {
  IdentityRead identity;
  TransformRead transforms;
};
struct QueryBinding {
  const space::Space &space;
  IdentityRead identity;
  float max_radius_m;
};
struct SoundBinding {
  observe::Hearing &hearing;
  IdentityRead identity;
  TransformRead transforms;
  const std::vector<observe::Listener> &listeners;
};
struct AvatarControlBinding {
  perm::Grants &grants;
  perm::Players &players;
  IdentityRead identity;
  AuthorityRead authority;
  const SparseColumn<std::vector<ScriptId>> &installed_scripts;
  const catalog::Models &models;
};
int navigate_binding(lua_State *);
int cancel_motion_binding(lua_State *);
int attach_binding(lua_State *);
int grasp_binding(lua_State *);
int release_binding(lua_State *);
int impulse_binding(lua_State *);
int sweep_binding(lua_State *);
int transfer_binding(lua_State *);
int position_binding(lua_State *);
int transform_binding(lua_State *);
int pose_batch_binding(lua_State *);
int move_part_binding(lua_State *);
int near_binding(lua_State *);
int ray_binding(lua_State *);
int sound_binding(lua_State *);
int allow_avatar_binding(lua_State *);
int revoke_avatar_binding(lua_State *);
} // namespace amber::bindings
