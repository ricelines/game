#include "private.hpp"

namespace amber::space {
CauseToken native_cause(const Space::Native &n, Handle object) {
  if (auto parent = n.compound_parent.find(object.bits); parent != n.compound_parent.end())
    object = {parent->second};
  const auto found = n.causes.find(object.bits);
  return found == n.causes.end() ? 0 : found->second.token;
}
CausePair native_causes(const Space::Native &n, Handle first, Handle second) {
  return {native_cause(n, first), native_cause(n, second)};
}
void clear_cause(Space::Native &n, Handle object) {
  n.causes.erase(object.bits);
  if (auto parent = n.compound_parent.find(object.bits); parent != n.compound_parent.end())
    n.causes.erase(parent->second);
}
void stamp_cause(Space::Native &n, Handle object, CauseToken token, CauseScope scope) {
  clear_cause(n, object);
  if (auto parent = n.compound_parent.find(object.bits); parent != n.compound_parent.end())
    object = {parent->second};
  if (token)
    n.causes.emplace(object.bits, Space::Native::CauseStamp{token, scope, n.physics_epoch + 1});
}
CauseToken motion_cause(const Space &space, IdentityRead identity, Handle object) {
  return identity.valid(object) ? native_cause(*space.native, object) : 0;
}
void active_cause_tokens(const Space &space, std::vector<CauseToken> &out) {
  out.clear();
  const auto &n = *space.native;
  for (const auto &[object, stamp] : n.causes) {
    (void)object;
    out.push_back(stamp.token);
  }
  // Synchronous removals may be waiting for the next physics result batch.
  for (const auto &contact : n.contacts) {
    if (contact.causes.first)
      out.push_back(contact.causes.first);
    if (contact.causes.second)
      out.push_back(contact.causes.second);
  }
}
OperationResult restore_motion_cause(Space &space, IdentityRead identity, Handle object,
                                     CauseToken token) {
  auto &n = *space.native;
  if (!identity.valid(object))
    return {Status::StaleHandle, {}};
  const auto found = n.bodies.find(object.bits);
  if (found == n.bodies.end())
    return {Status::MissingBody, {}};
  if (found->second.spec.motion != Motion::Dynamic && !found->second.character)
    return {Status::UnsupportedMotion, {}};
  stamp_cause(n, object, token, CauseScope::Motion);
  return {Status::Ok, {}};
}
void expire_causes(Space::Native &n, const std::map<std::uint64_t, const MoveRequest *> &active) {
  std::erase_if(n.causes, [&](const auto &row) {
    if (row.second.scope == CauseScope::Step)
      return row.second.expires_after <= n.physics_epoch;
    const auto found = n.bodies.find(row.first);
    if (found == n.bodies.end())
      return true;
    const auto &body = found->second;
    if (body.character)
      return !active.contains(row.first) && body.character->IsSupported();
    return !n.physics.GetBodyInterface().IsActive(body.body);
  });
}
void Space::Native::CharacterListener::add(const JPH::CharacterVirtual *character,
                                           const JPH::CharacterContact &hit, bool started) {
  // Jolt calls this from actual contact resolution/support; some paths set
  // mHadCollision immediately after the callback rather than before it.
  if (hit.mIsSensorB)
    return;
  const Handle first{character->GetUserData()}, second{hit.mUserData};
  if (!first || !second || first == second)
    return;
  Contact contact{first,
                  second,
                  av(hit.mPosition),
                  av(-hit.mContactNormal),
                  started,
                  native_causes(state, first, second)};
  pairs[{first.bits, hit.mBodyB.GetIndexAndSequenceNumber(), hit.mCharacterIDB.GetValue(),
         hit.mSubShapeIDB.GetValue()}] = contact;
  if (started)
    state.contacts.push_back(contact);
}
void Space::Native::CharacterListener::remove(const JPH::CharacterVirtual *character,
                                              const JPH::BodyID &body,
                                              const JPH::CharacterID &other,
                                              const JPH::SubShapeID &subshape) {
  const Key key{character->GetUserData(), body.GetIndexAndSequenceNumber(), other.GetValue(),
                subshape.GetValue()};
  const auto found = pairs.find(key);
  if (found == pairs.end())
    return;
  auto contact = found->second;
  contact.started = false;
  contact.causes = native_causes(state, contact.first, contact.second);
  state.contacts.push_back(contact);
  pairs.erase(found);
}
void Space::Native::CharacterListener::OnContactAdded(const JPH::CharacterVirtual *c,
                                                      const JPH::CharacterContact &hit,
                                                      JPH::CharacterContactSettings &) {
  add(c, hit, true);
}
void Space::Native::CharacterListener::OnContactPersisted(const JPH::CharacterVirtual *c,
                                                          const JPH::CharacterContact &hit,
                                                          JPH::CharacterContactSettings &) {
  add(c, hit, false);
}
void Space::Native::CharacterListener::OnContactRemoved(const JPH::CharacterVirtual *c,
                                                        const JPH::BodyID &body,
                                                        const JPH::SubShapeID &subshape) {
  remove(c, body, JPH::CharacterID{}, subshape);
}
void Space::Native::CharacterListener::OnCharacterContactAdded(const JPH::CharacterVirtual *c,
                                                               const JPH::CharacterContact &hit,
                                                               JPH::CharacterContactSettings &) {
  add(c, hit, true);
}
void Space::Native::CharacterListener::OnCharacterContactPersisted(
    const JPH::CharacterVirtual *c, const JPH::CharacterContact &hit,
    JPH::CharacterContactSettings &) {
  add(c, hit, false);
}
void Space::Native::CharacterListener::OnCharacterContactRemoved(const JPH::CharacterVirtual *c,
                                                                 const JPH::CharacterID &other,
                                                                 const JPH::SubShapeID &subshape) {
  remove(c, JPH::BodyID{}, other, subshape);
}
} // namespace amber::space
