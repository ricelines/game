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
  std::optional<react::BodyEndpoints> direct_body;
};
struct CompletedInvocation {
  react::HandlerRef handler;
  Handle receiver;
  TermId trigger = 0;
  Step step = 0;
  react::RunOutcome outcome;
  react::DeferredEffects effects;
  std::vector<record::Event> structural_records;
  std::vector<Handle> things{};
  std::vector<record::Credit> object_credits{}, script_credits{};
};
CompletedInvocation execute(Execution &, const react::HandlerRef &, InvocationInput);
record::Origin publish_invocation(record::Journal &, step::WorkQueues &, step::TimerWheel &,
                                  CompletedInvocation &&, const record::Origin &,
                                  std::size_t event_limit);
struct ActionCompletion {
  record::Origin origin;
  std::size_t handlers = 0;
  bool changed = false;
  std::vector<std::string> errors;
};
ActionCompletion publish_action(record::Journal &, step::WorkQueues &, step::TimerWheel &,
                                record::Event action, std::vector<CompletedInvocation>,
                                const record::Origin &, std::span<const record::Credit>,
                                std::size_t event_limit);
} // namespace amber::bindings
