#pragma once

#include "bindings/allocation/allocation.hpp"
#include "bindings/measurements.hpp"
#include "control/control.hpp"
#include "drive/drive.hpp"
#include "space/space.hpp"

namespace amber::bindings {
struct GapDraft {
  Handle target;
  TermId term, action;
  PlayerId requester;
  record::Origin origin;
};
struct SelectorWatches {
  react::Subscriptions &subscriptions;
  install::WatchLinks &links;
  const catalog::Scripts &scripts;
  const react::RuleVersions &rules;
};
struct HandlerTurns {
  bindings::Execution &execution;
  const react::Triggers &triggers;
  const react::ValueRead &values;
  SelectorWatches selectors;
  record::Journal &journal;
  step::WorkQueues &queues;
  step::TimerWheel &timers;
  std::vector<FactKey> &mutated;
  const drive::Clock &clock;
  double hz;
  std::size_t event_limit;
  AllocationReconciliation &allocation;
  MassReconciliation *mass = nullptr;
};
void reconcile_effects(HandlerTurns &, react::DeferredEffects &, const record::Origin &);
void consume_event(void *, drive::EventTurn);
void consume_background(void *, step::BackgroundItem);
void consume_watch(void *, step::WatchWork);
struct ActionTurns {
  HandlerTurns &handlers;
  const command::NativeActions &declarations;
  const drive::NativeOperations &native;
  const catalog::Models &models;
  const catalog::Scripts &scripts;
  const install::InstanceScripts &instances;
  const react::RuleVersions &rules;
  const control::Profiles &profiles;
  AuthorityRead authority;
  TransformRead transforms;
  const perm::OwnerLimits &limits;
  const perm::Players &players;
  const perm::Grants &grants;
  const space::Space &space;
  const step::Intents &intents;
  std::vector<GapDraft> &gaps;
  float reach_m;
};
struct ActionOutcome {
  nlohmann::json reply;
  record::Origin origin;
  bool deferred = false;
};
ActionOutcome perform_action(ActionTurns &, ActionRequest, nlohmann::json native_arguments,
                             const record::Origin &, std::optional<drive::InputOrder> order = {},
                             std::chrono::steady_clock::time_point deadline = {});
bindings::CompletedInvocation run_invocation(ActionTurns &, const drive::PlannedInvocation &,
                                             std::vector<Handle> *created = nullptr);
} // namespace amber::bindings
