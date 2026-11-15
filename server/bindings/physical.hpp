#pragma once
#include "causality.hpp"
#include "drive/drive.hpp"
#include "observe/observe.hpp"
#include "perm/perm.hpp"

namespace amber::bindings {
struct NavigationBinding {
  step::Intents &intents;
  std::vector<step::IntentEnding> &endings;
  IdentityRead identity;
  const perm::Players &players;
  const perm::Grants &grants;
  const perm::Plots &plots;
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
  PlotWrite plot_write;
  const perm::Plots &plots;
};
struct ImpulseBinding {
  space::Space &space;
  IdentityRead identity;
  MechanicalCauses &causes;
};
struct SweepBinding {
  space::Space &space;
  IdentityRead identity;
  TransformWrite transforms;
  const Relation &declared_parts;
  MechanicalCauses &causes;
  PlotWrite plot_write;
  const perm::Plots &plots;
};
struct PartPoseBinding {
  space::Space &space;
  IdentityRead identity;
  TransformWrite transforms;
  const Relation &declared_parts;
  MechanicalCauses &causes;
  PlotWrite plot_write;
  const perm::Plots &plots;
  std::size_t batch_max;
};
struct QuantityBinding {
  react::Properties &properties;
  react::Processes &processes;
  const react::Terms &terms;
  IdentityRead identity;
  AuthorityRead authority;
  const perm::OwnerLimits &limits;
  const perm::Players &players;
  const perm::Grants &grants;
};
struct QueryBinding {
  const space::Space &space;
  IdentityRead identity;
  TransformRead transforms;
};
struct SoundBinding {
  observe::Hearing &hearing;
  IdentityRead identity;
  TransformRead transforms;
  const std::vector<observe::Listener> &listeners;
  const catalog::InstanceTexts &instance_texts;
  const catalog::ModelTexts &model_texts;
};
int navigate_binding(lua_State *);
int cancel_motion_binding(lua_State *);
int sweep_binding(lua_State *);
int move_part_binding(lua_State *);
int near_binding(lua_State *);
int ray_binding(lua_State *);
int sound_binding(lua_State *);
void register_physical_actions(react::Terms &, command::NativeActions &, drive::NativeOperations &,
                               AttachmentBinding &, ImpulseBinding &, QuantityBinding &);
} // namespace amber::bindings
