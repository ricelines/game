#pragma once
#include "react.hpp"
#include "snap/codec.hpp"

namespace amber::react {
using SnapshotSelection = snap::ObjectSelection;
void write_properties(snap::Output &, const Properties &, const Processes &,
                      const SnapshotSelection & = {});
void read_properties(snap::Input &, Properties &, Processes &, std::size_t slots);
void write_relations(snap::Output &, const AuthoredRelations &, const SnapshotSelection & = {});
void read_relations(snap::Input &, AuthoredRelations &, std::size_t slots);
void write_shared(snap::Output &, const SharedColumns &, const GivesTo &, const AllocationRows &,
                  const MeterRows &, const SnapshotSelection & = {});
void read_shared(snap::Input &, SharedColumns &, GivesTo &, AllocationRows &, MeterRows &,
                 std::size_t slots);
void write_script_states(snap::Output &, const ScriptStates &, const Subscriptions &,
                         const SnapshotSelection & = {});
void read_script_states(snap::Input &, ScriptStates &, Subscriptions &);
void write_random(snap::Output &, const RandomStates &, const SnapshotSelection & = {});
void read_random(snap::Input &, RandomStates &);
void write_settings(snap::Output &, const InstanceSettings &, const ModelSettings &,
                    const PlotSettings &, const WorldSettings &, const SnapshotSelection & = {},
                    std::optional<std::span<const ModelId>> models = {},
                    std::optional<std::span<const PlotId>> plots = {});
void read_settings(snap::Input &, InstanceSettings &, ModelSettings &, PlotSettings &,
                   WorldSettings &, std::size_t slots);

template <class S> void serialize(S &s, Process &v) {
  snap::field(s, v.v0);
  snap::field(s, v.step0);
  snap::field(s, v.rate);
  snap::field(s, v.lo);
  snap::field(s, v.hi);
  snap::field(s, v.revision);
}
template <class S> void serialize(S &s, Contribution &v) {
  snap::field(s, v.from);
  snap::field(s, v.amount);
}
template <class S> void serialize(S &s, Shared &v) {
  snap::field(s, v.total);
  snap::field(s, v.parts);
}
template <class S> void serialize(S &s, SubjectTerm &v) {
  snap::field(s, v.subject);
  snap::field(s, v.term);
}
template <class S> void serialize(S &s, AllocationKey &v) {
  snap::field(s, v.subject);
  snap::field(s, v.contributor);
  snap::field(s, v.term);
  snap::field(s, v.supplier);
}
template <class S> void serialize(S &s, Meter &v) {
  snap::field(s, v.amount0);
  snap::field(s, v.step0);
  snap::field(s, v.rate);
}
template <class S> void serialize(S &s, Answer &v) {
  snap::field(s, v.present);
  snap::field(s, v.value);
}
template <class S> void serialize(S &s, SharedWatchAnswer &v) {
  snap::field(s, v.aggregate);
  snap::field(s, v.own_allocation);
}
template <class S> void serialize(S &s, RelationAnswer &v) {
  snap::field(s, v.out);
  snap::field(s, v.in);
}
template <class S> void serialize(S &s, Threshold &v) {
  snap::field(s, v.predicate);
  snap::field(s, v.value);
}
template <class S> void serialize(S &s, WatchBinding &v) {
  snap::field(s, v.consumer);
  snap::field(s, v.index);
  snap::field(s, v.revision);
  snap::field(s, v.last);
  snap::field(s, v.thresholds);
}
} // namespace amber::react
