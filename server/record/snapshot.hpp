#pragma once
#include "record.hpp"
#include "snap/codec.hpp"

namespace amber::record {
template <class S> void serialize(S &s, Origin &v) {
  s.value8b(v.cause);
  s.value8b(v.root);
  snap::field(s, v.authors);
  snap::field(s, v.oldest);
  s.boolValue(v.more);
}
} // namespace amber::record
