#pragma once

#include "catalog/snapshot.hpp"
#include "control/snapshot.hpp"
#include "drive/drive.hpp"
#include "elaborate/snapshot.hpp"
#include "lifecycle/snapshot.hpp"
#include "perm/snapshot.hpp"
#include "react/snapshot.hpp"
#include "space/snapshot.hpp"
#include "step/snapshot.hpp"

namespace amber {
struct CheckpointFilter;
}
namespace amber::bindings {
struct BodyCauseRow;
class MechanicalCauses;
struct GapDraft;
namespace viewing {
struct SessionViews;
}
} // namespace amber::bindings

namespace amber::bindings::checkpoint {
// Both full checkpoints and local captures use the same owner encoders.
struct Encoding {
  const snap::SectionWriters &sections;
  CheckpointFilter &filter;
  std::vector<space::MotionRow> &motion;
  std::vector<BodyCauseRow> &causes;
  const space::Space &space;
  IdentityRead identity;
  const MechanicalCauses &mechanical_causes;
};
struct SnapshotProgress {
  std::optional<Step> last_step;
  double pause_max_ms = 0;
};
bool save_snapshot(snap::Writer &, Encoding &, SnapshotProgress &, const snap::Header &);

// Read access for closing a local capture over geometry, dependencies and custody.
// No live gameplay owner receives this persistence-only view.
struct CaptureRead {
  const react::ValueRead &values;
  ParentRead parents;
  TransformRead transforms;
  AuthorityRead authority;
  const lifecycle::Parts &parts;
  const catalog::Models &models;
  const catalog::ParameterBlocks &parameters;
  const install::InstanceScripts &installations;
  const react::RuleVersions &rules;
  const perm::Players &players;
  const perm::Plots &plots;
  const perm::Groups &groups;
  const perm::Grants &grants;
  const perm::Offers &offers;
  const perm::Trades &trades;
  const viewing::SessionViews &views;
  const control::Profiles &profiles;
};
struct GapCaptures {
  CaptureRead read;
  std::vector<GapDraft> &pending;
  elaborate::GapCounts &counts;
  elaborate::LastGaps &last;
  elaborate::Requests &requests;
  elaborate::CaptureRefs &captures;
  record::Journal &journal;
  elaborate::CaptureWriter &writer;
  Encoding &encoding;
  std::size_t capture_limit;
  float radius_m;
};
void flush_gaps(GapCaptures &, const snap::Header &);

std::vector<Handle> neighborhood(IdentityRead, ParentRead, const lifecycle::Parts &,
                                 std::span<const Relation *const>, std::span<const Handle> seeds);
std::vector<Handle> priority_neighborhood(IdentityRead, ParentRead, const lifecycle::Parts &,
                                          const react::Terms &, const react::SharedColumns &,
                                          std::span<const Handle> initial);
struct CatalogSelection {
  std::vector<ModelId> models;
  std::vector<std::uint32_t> parameters;
  std::vector<ScriptId> scripts;
};
CatalogSelection catalog_selection(const catalog::Models &, const catalog::ParameterBlocks &,
                                   const install::InstanceScripts &, IdentityRead,
                                   const std::vector<std::uint32_t> &parameters,
                                   const react::RuleVersions &, std::span<const Handle>);

void write_prop(snap::Output &, const react::Properties &, const react::Processes &,
                const react::InstanceSettings &, const react::ModelSettings &,
                const react::PlotSettings &, const react::WorldSettings &,
                const snap::ObjectSelection & = {}, const CatalogSelection * = nullptr,
                std::optional<std::span<const PlotId>> plots = {});
void read_prop(snap::Input &, react::Properties &, react::Processes &, react::InstanceSettings &,
               react::ModelSettings &, react::PlotSettings &, react::WorldSettings &,
               std::size_t slots);
void write_rels(snap::Output &, const react::AuthoredRelations &, const space::PhysicalRelations &,
                const lifecycle::Parts &, IdentityRead,
                std::optional<std::span<const Handle>> selected = {});
void read_rels(snap::Input &, react::AuthoredRelations &, space::PhysicalRelations &,
               lifecycle::Parts &, IdentityRead, Relation &declared_parts);
void write_text(snap::Output &, const catalog::Models &, const catalog::ParameterBlocks &,
                const catalog::Scripts &, const catalog::ModelTexts &,
                const catalog::InstanceTexts &, const catalog::AuthoredTexts &,
                const CatalogSelection * = nullptr,
                std::optional<std::span<const Handle>> selected = {});
void read_text(snap::Input &, catalog::Models &, catalog::ParameterBlocks &, catalog::Scripts &,
               catalog::ModelTexts &, catalog::InstanceTexts &, catalog::AuthoredTexts &);
void write_players(snap::Output &, const perm::Players &, const perm::Ledger &, const perm::Plots &,
                   const perm::Groups &, const control::Profiles &,
                   const perm::SnapshotSelection * = nullptr);
void read_players(snap::Input &, perm::Players &, perm::Ledger &, perm::Plots &, perm::Groups &,
                  control::Profiles &);
void write_scst(snap::Output &, const react::ScriptStates &, const react::Subscriptions &,
                const install::InstanceScripts &, IdentityRead,
                std::optional<std::span<const Handle>> selected = {});
void read_scst(snap::Input &, react::ScriptStates &, react::Subscriptions &,
               install::InstanceScripts &, std::size_t slots);
void write_queues(snap::Output &, const step::WorkQueues &, const step::Intents &,
                  std::uint64_t next_record, const drive::Inputs &,
                  const snap::ObjectSelection & = {},
                  std::span<const std::uint64_t> selected_inputs = {});
std::uint64_t read_queues(snap::Input &, step::WorkQueues &, step::Intents &, drive::Inputs &);
void write_reqs(snap::Output &, const elaborate::Requests &, const elaborate::GapCounts &,
                const elaborate::LastGaps &, const elaborate::CaptureRefs &,
                const elaborate::PendingFixtures &,
                std::optional<std::span<const Handle>> selected = {});
void read_reqs(snap::Input &, elaborate::Requests &, elaborate::GapCounts &, elaborate::LastGaps &,
               elaborate::CaptureRefs &, elaborate::PendingFixtures &);
} // namespace amber::bindings::checkpoint
