#pragma once
#include "perm/perm.hpp"
#include "react/react.hpp"
#include "space/space.hpp"

namespace amber::bindings {
class SceneProjection;
struct PlotObjectsBinding {
  IdentityRead identity;
  const std::vector<PlotId> &plot;
  const Relation &declared_parts;
  const perm::Plots &plots;
};
struct DeliveryTargets {
  std::unordered_map<PlayerId, Handle> present_bodies;
};
struct HoldGoodsBinding {
  space::Space &space;
  IdentityRead identity;
  AuthorityRead authority;
  ParentWrite parents;
  SpaceFlags flags;
  space::PhysicalRelations &relations;
  const Relation &declared_parts;
  const perm::OwnerLimits &limits;
  const perm::Grants &grants;
  perm::Players &players;
  const DeliveryTargets &delivery;
  std::size_t mailbox_max;
  std::vector<space::RelationChange> &relation_changes;
  SceneProjection &scene;
};
int plot_objects_binding(lua_State *);
int hold_goods_binding(lua_State *);
} // namespace amber::bindings
