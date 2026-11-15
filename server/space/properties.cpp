#include "private.hpp"

#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <cmath>

namespace amber::space {
OperationResult set_properties(Space &space, IdentityRead identity, Handle object,
                               BodyProperties properties) {
  auto &n = *space.native;
  if (!identity.valid(object))
    return {Status::StaleHandle, {}};
  const auto found = n.bodies.find(object.bits);
  if (found == n.bodies.end())
    return {Status::MissingBody, {}};
  const auto surface = properties.surface;
  if (!(properties.mass_kg > 0) || !std::isfinite(properties.mass_kg) ||
      !std::isfinite(surface.friction) || surface.friction < 0 ||
      !std::isfinite(surface.restitution) || surface.restitution < 0 ||
      !std::isfinite(surface.transmission) || surface.transmission < 0 || surface.transmission > 1)
    return {Status::UnsupportedMotion, {}};
  auto &body = found->second;
  auto *root = &body;
  if (const auto parent = n.compound_parent.find(object.bits); parent != n.compound_parent.end())
    root = &n.bodies.at(parent->second);
  auto &own = body.own_geometry ? body.own_geometry->spec : body.spec;
  const auto total = double(root->spec.mass_kg) - own.mass_kg + properties.mass_kg;
  if (!(total > 0) || !std::isfinite(total) || total > std::numeric_limits<float>::max())
    return {Status::UnsupportedMotion, {}};
  clear_cause(n, object);
  own.mass_kg = properties.mass_kg;
  root->spec.mass_kg = static_cast<float>(total);
  {
    JPH::BodyLockWrite lock(n.physics.GetBodyLockInterface(), root->body);
    auto &native = lock.GetBody();
    if (!native.IsStatic())
      native.GetMotionProperties()->ScaleToMass(root->spec.mass_kg);
  }
  if (body.character) {
    body.character->SetMass(properties.mass_kg);
    body.spec.character->mass_kg = properties.mass_kg;
  }
  const auto result = set_surface(space, identity, object, surface);
  const auto bounds = n.world_bounds(body);
  n.changed(object, bounds, bounds, false);
  return result;
}
} // namespace amber::space
