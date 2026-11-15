#pragma once

#include "bindings/turns/turns.hpp"
#include "observe/observe.hpp"

namespace amber::bindings {
struct QueuedControl {
  drive::Inputs &inputs;
  step::WorkQueues &queues;
};
control::Reply queue_control(void *, const control::Invocation &);
struct InputDelivery {
  drive::Inputs &inputs;
  const drive::InputOperations &operations;
  control::Sessions &sessions;
};
void consume_input(void *, step::InputWork);
struct DoInput {
  ActionTurns &actions;
  observe::Hearing &hearing;
  std::size_t events_per_response;
};
drive::InputReply do_input(void *, const drive::Input &);
struct CancelInput {
  step::Intents &intents;
  std::vector<step::IntentEnding> &endings;
  record::Journal &journal;
  observe::Hearing &hearing;
  const drive::Clock &clock;
  std::size_t events_per_response;
};
drive::InputReply cancel_input(void *, const drive::Input &);
EventPayload decode_arguments(const react::Term &, const nlohmann::json &, Handle body);
} // namespace amber::bindings
