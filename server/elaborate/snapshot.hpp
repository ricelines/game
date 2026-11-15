#pragma once
#include "elaborate.hpp"
#include "snap/codec.hpp"

namespace amber::elaborate {
using ObjectSelection = std::optional<std::span<const Handle>>;
using GapSelection = std::optional<std::span<const GapKey>>;
using HashSelection = std::optional<std::span<const Hash>>;
void write_requests(snap::Output &, const Requests &, ObjectSelection = {});
void read_requests(snap::Input &, Requests &);
void write_gaps(snap::Output &, const GapCounts &, GapSelection = {});
void read_gaps(snap::Input &, GapCounts &);
void write_last_gaps(snap::Output &, const LastGaps &, ObjectSelection = {});
void read_last_gaps(snap::Input &, LastGaps &);
void write_capture_refs(snap::Output &, const CaptureRefs &, HashSelection = {});
void read_capture_refs(snap::Input &, CaptureRefs &);
} // namespace amber::elaborate
