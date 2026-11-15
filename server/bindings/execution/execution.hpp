#pragma once

#include "react/react.hpp"

namespace amber::bindings {
struct CreditResolver {
  void *context = nullptr;
  std::vector<record::Credit> (*resolve)(const void *, std::span<const Handle>, ScriptId,
                                         double hz) = nullptr;
};
struct Execution {
  react::Vm &vm;
  react::ScriptStates &states;
  react::RandomStates &random;
  IdentityRead identity;
  const Relation &declared_parts;
  std::uint64_t initial_seed;
  CreditResolver credits{};
};
struct InvocationInput {
  Handle receiver, sender;
  TermId trigger = 0;
  EventPayload payload;
  Step step = 0;
  double hz = 20;
  record::Origin origin;
  std::vector<Handle> acquired;
  std::optional<Caller> action_caller;
  Handle action_body, action_target;
  std::chrono::steady_clock::time_point deadline{};
};
struct CompletedInvocation {
  react::HandlerRef handler;
  Handle receiver;
  TermId trigger = 0;
  Step step = 0;
  react::RunOutcome outcome;
  react::DeferredEffects effects;
  std::vector<Handle> things{};
  std::vector<record::Credit> object_credits{}, script_credits{};
  record::Origin origin{};
};
void start_invocation(Execution &, record::Journal &, const react::HandlerRef &, InvocationInput,
                      react::Invocation &, CompletedInvocation &);
void finish_invocation(Execution &, record::Journal &, react::Invocation &, CompletedInvocation &);
record::Origin publish_invocation(record::Journal &, step::WorkQueues &, step::TimerWheel &,
                                  CompletedInvocation &&, const record::Origin &,
                                  std::size_t event_limit);
} // namespace amber::bindings
