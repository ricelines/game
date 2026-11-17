#include "step.hpp"

#include <stdexcept>

namespace amber::step {
step::InputWork enqueue_input(Inputs &inputs, Input input) {
  input.id = inputs.next_id++;
  input.awaiting = false;
  const auto id = input.id;
  inputs.rows.emplace(id, std::move(input));
  return {id};
}
InputCompletion complete_input(Inputs &inputs, std::uint64_t id, nlohmann::json reply) {
  auto it = inputs.rows.find(id);
  if (it == inputs.rows.end())
    throw std::invalid_argument("input is no longer pending");
  const auto &input = it->second;
  InputCompletion completed{id, input.session, input.sequence, input.request, std::move(reply)};
  inputs.rows.erase(it);
  return completed;
}
} // namespace amber::step
