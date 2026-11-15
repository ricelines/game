#pragma once

#include "bindings/presentation.hpp"
#include "command/command.hpp"
#include "control/control.hpp"
#include "install/install.hpp"
#include "perm/perm.hpp"
#include "sources.hpp"

#include <unordered_set>

namespace amber::bindings::viewing {
using Json = nlohmann::json;
// Read-only catalog projection shared by descriptions/discovery, not a host.
struct CatalogRead {
  const catalog::Models &models;
  const catalog::ParameterBlocks &parameters;
  const catalog::Assets &assets;
  const catalog::Scripts &scripts;
  const catalog::ModelTexts &model_text;
  const catalog::InstanceTexts &instance_text;
  const install::InstanceScripts &instances;
  const command::NativeActions &native;
  const react::ValueRead &values;
  const std::vector<TermId> &inspection_terms;
  const catalog::AuthoredTexts &authored_texts;
};
struct PendingView {
  Step step = 0;
  bool ready = false, complete = false;
  std::size_t limit = 0;
  observe::SelfDescription self;
  std::vector<observe::PreparedThing> things, far;
  std::vector<std::string> body_actions, player_actions;
};
struct SessionView {
  PlayerId player = 0;
  Handle body;
  Vec3 eye;
  observe::SessionCache sent;
  PendingView pending;
  std::unordered_set<std::uint64_t> acquired;
  std::vector<Handle> last_observation;
};
struct SessionViews {
  std::unordered_map<net::SessionId, SessionView> rows;
};
struct ViewInterestSink {
  void *local = nullptr;
  void (*changed)(void *, net::SessionId, Vec3) = nullptr;
};
struct ObservationSink {
  void *local = nullptr;
  void (*record)(void *, PlayerId, Handle, std::span<const Handle>, const Json &) = nullptr;
};
struct LookBinding {
  SessionViews &sessions;
  observe::ActiveViews &active;
  observe::Hearing &hearing;
  const space::Space &space;
  TransformRead transforms;
  const Relation &declared_parts, &attached_to;
  CatalogRead catalog;
  const observe::Lights &lights;
  const observe::ZoneAmbients &zones;
  const std::vector<observe::MotionSample> &motion;
  const step::Intents &intents;
  observe::Settings settings;
  const Step &now;
  double hz, world_ambient_lux, query_ms;
  ViewInterestSink interest;
  ObservationSink observations;
};
struct InspectBinding {
  SessionViews &sessions;
  observe::Hearing &hearing;
  CatalogRead catalog;
  TransformRead transforms;
  AuthorityRead authority;
  ParentRead parents;
  AttributionRead attribution;
  const react::RelationsRead &relations;
  const perm::OwnerLimits &limits;
  const perm::Players &players;
  const perm::Grants &grants;
  const perm::Plots &plots;
  const perm::Groups &groups;
  const perm::Ledger &ledger;
  const step::Intents &intents;
  const PublicSources &sources;
  const Step &now;
  double hz;
  std::size_t events_per_response;
  const react::ScriptStates &states;
  const react::RuleVersions &rules;
};
struct ActionsBinding {
  SessionViews &sessions;
  observe::Hearing &hearing;
  CatalogRead catalog;
  std::size_t page_size, events_per_response;
  AuthorityRead authority;
  const perm::OwnerLimits &limits;
  const perm::Players &players;
  const perm::Grants &grants;
  const Step &now;
};
struct RecordFeed {
  struct Entry {
    Json record;
    std::vector<Vec3> positions;
  };
  std::deque<Entry> rows;
  std::size_t capacity;
};
void append_records(RecordFeed &, std::span<const Json>, IdentityRead, TransformRead);
struct EventsBinding {
  const RecordFeed &feed;
  const SessionViews &sessions;
  float radius_m;
  std::size_t default_limit;
};
struct DebugStats {
  void *local = nullptr;
  Json (*read)(const void *) = nullptr;
};
struct DebugBinding {
  const space::Space &space;
  IdentityRead identity;
  const Step &now;
  double query_ms;
  float default_radius_m;
  DebugStats stats;
};

std::vector<std::string> object_actions(const CatalogRead &, Handle);
std::vector<std::string> player_actions(const CatalogRead &);
std::vector<TermId> object_action_ids(const CatalogRead &, Handle);
std::vector<TermId> player_action_ids(const CatalogRead &);
void refresh_view(LookBinding &, net::SessionId, const control::Principal &, std::size_t limit);
void view_changed(SessionViews &, net::SessionId, Vec3);
void invalidate(SessionViews &, std::span<const Handle> changed);
void remove_session(SessionViews &, observe::ActiveViews &, observe::Hearing &, net::SessionId);
void append_heard(Json &, observe::Hearing &, net::SessionId, std::size_t limit);
control::Reply look_handler(void *, const control::Invocation &);
control::Reply inspect_handler(void *, const control::Invocation &);
control::Reply actions_handler(void *, const control::Invocation &);
control::Reply events_handler(void *, const control::Invocation &);
control::Reply debug_handler(void *, const control::Invocation &);
} // namespace amber::bindings::viewing
