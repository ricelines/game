#pragma once

namespace amber::record {
class Journal;
}
namespace amber::drive {
struct Clock;
struct Input;
struct InputReply;
} // namespace amber::drive
namespace amber::bindings::authoring {
struct FailedAuthoring {
  record::Journal &journal;
  const drive::Clock &clock;
};
// Authenticated queued submission failure; it allocates no catalog identity.
drive::InputReply failed_authoring(void *, const drive::Input &);
} // namespace amber::bindings::authoring
