#pragma once

#include "bindings/execution/execution.hpp"
#include "lifecycle/lifecycle.hpp"
#include "space/space.hpp"

#include <map>

namespace amber::bindings {
struct NavigationDiagnostics {
  const space::Space &space;
  IdentityRead identity;
  TransformRead transforms;
  struct Request {
    Handle body;
    Vec3 reference;
    std::map<Handle, space::Bounds> regions;
    std::vector<std::string> errors;
  };
  std::map<PlayerId, Request> pending{};
};
struct NavigationNotice {
  PlayerId author;
  nlohmann::json event;
};
void navigation_birth(NavigationDiagnostics &, std::span<const Handle>, const InvocationInput &,
                      react::DeferredEffects &);
void navigation_rebind(NavigationDiagnostics &, std::span<const lifecycle::Rebinding>,
                       const InvocationInput &, react::DeferredEffects &);
// Call after each install pass. Dirty tiles and expired deadlines retain the
// requests. Returned notices go through the ordinary player event channel.
std::vector<NavigationNotice> publish_navigation(NavigationDiagnostics &,
                                                 const space::NavigationInstall &, space::Deadline);
} // namespace amber::bindings
