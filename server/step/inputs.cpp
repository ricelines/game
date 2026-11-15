#include "snap/snap.hpp"
#include "step.hpp"

#include <algorithm>
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
void write_inputs(snap::Output &out, const Inputs &inputs,
                  std::optional<std::span<const std::uint64_t>> selected_ids) {
  auto next = inputs.next_id;
  snap::field(out, next);
  std::vector<Input> rows;
  rows.reserve(inputs.rows.size());
  for (const auto &[id, row] : inputs.rows) {
    (void)id;
    if (!selected_ids ||
        std::find(selected_ids->begin(), selected_ids->end(), id) != selected_ids->end())
      rows.push_back(row);
  }
  out.container(rows, snap::max_collection(), [](snap::Output &s, Input &row) {
    snap::field(s, row.id);
    snap::field(s, row.kind);
    snap::field(s, row.caller);
    snap::field(s, row.body);
    snap::field(s, row.session);
    snap::field(s, row.sequence);
    snap::field(s, row.request);
    auto fields = row.fields.dump();
    snap::field(s, fields);
    snap::field(s, row.origin);
    snap::field(s, row.awaiting);
  });
}
void read_inputs(snap::Input &in, Inputs &inputs) {
  snap::field(in, inputs.next_id);
  std::vector<Input> rows;
  inputs.rows.clear();
  in.container(rows, snap::max_collection(), [](snap::Input &s, Input &row) {
    snap::field(s, row.id);
    snap::field(s, row.kind);
    snap::field(s, row.caller);
    snap::field(s, row.body);
    snap::field(s, row.session);
    snap::field(s, row.sequence);
    snap::field(s, row.request);
    std::string fields;
    snap::field(s, fields);
    row.fields = nlohmann::json::parse(fields);
    snap::field(s, row.origin);
    snap::field(s, row.awaiting);
  });
  for (auto &row : rows) {
    auto id = row.id;
    if (!id || id >= inputs.next_id || !inputs.rows.emplace(id, std::move(row)).second)
      throw std::runtime_error("invalid input snapshot identity");
  }
}
} // namespace amber::step
