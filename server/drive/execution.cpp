#include "drive.hpp"

#include <stdexcept>

namespace amber::drive {
ExecutionResult execute(bindings::Execution &execution, const PlannedInvocation &plan,
                        const StructuralOperations &operations) {
  ExecutionResult result{bindings::execute(execution, plan.handler, plan.input), {}};
  auto &completed = result.invocation;
  auto requests = std::move(completed.effects.structural);
  completed.effects.structural.clear();
  for (const auto &request : requests) {
    StructuralCompletion applied;
    try {
      auto operation = operations.rows.find(request.kind);
      if (operation == operations.rows.end() || !operation->second.run)
        throw std::invalid_argument("structural operation unavailable");
      applied =
          operation->second.run(operation->second.context, {request, plan.handler, plan.input});
    } catch (const std::exception &error) {
      applied.error = error.what();
    }
    completed.effects.changed |= applied.changed;
    result.created.insert(result.created.end(), applied.created.begin(), applied.created.end());
    completed.structural_records.insert(completed.structural_records.end(),
                                        std::make_move_iterator(applied.records.begin()),
                                        std::make_move_iterator(applied.records.end()));
    completed.effects.ordered.insert(completed.effects.ordered.end(),
                                     std::make_move_iterator(applied.effects.begin()),
                                     std::make_move_iterator(applied.effects.end()));
    if (!applied.error.empty()) {
      if (!completed.outcome.error.empty())
        completed.outcome.error += "; ";
      completed.outcome.error += applied.error;
      if (completed.outcome.status == react::RunOutcome::Status::Ok)
        completed.outcome.status = react::RunOutcome::Status::Error;
      completed.effects.ordered.emplace_back(std::in_place_type<react::Diagnostic>,
                                             react::Diagnostic{"error", std::move(applied.error)});
    }
  }
  return result;
}
void register_native(NativeOperations &operations, TermId term, NativeOperation operation) {
  if (!term || !operation.run || !operation.validate ||
      !operations.rows.emplace(term, operation).second)
    throw std::invalid_argument("invalid or duplicate native action registration");
}
NativeCompletion invoke_native(const NativeOperations &operations, NativeCall call) {
  NativeCompletion completion;
  try {
    auto operation = operations.rows.find(call.request.action);
    if (operation == operations.rows.end())
      throw std::invalid_argument("native action unavailable");
    const auto error = operation->second.validate(call.arguments);
    if (!error.empty())
      throw std::invalid_argument(error);
    if (!std::holds_alternative<PlayerId>(call.request.caller)) {
      call.body = {};
      call.order.reset();
    }
    completion = operation->second.run(operation->second.context, call);
  } catch (const std::exception &error) {
    completion.result = record::Result::Denied;
    completion.reply = {{"ok", false}, {"error", "bad_input"}, {"problem", error.what()}};
  }
  return completion;
}
record::Origin publish_native(record::Journal &journal, step::WorkQueues &queues,
                              step::TimerWheel &timers, record::Event action,
                              NativeCompletion completed, const record::Origin &parent,
                              std::span<const record::Credit> credits, Step now,
                              std::size_t event_limit) {
  if (!completed.effects.structural.empty())
    throw std::logic_error("native action must finish structural effects before publication");
  action.changed = completed.changed || completed.effects.changed;
  action.result = action.changed && completed.result != record::Result::Denied
                      ? record::Result::Applied
                      : completed.result;
  action.detail["response"] = completed.reply;
  const auto origin = journal.append(std::move(action), parent, credits);
  for (auto &event : completed.records)
    journal.append(std::move(event), origin);
  react::publish_effects(std::move(completed.effects), origin, now, queues, timers, journal,
                         event_limit);
  return origin;
}
} // namespace amber::drive
