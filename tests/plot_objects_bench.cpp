#include "catalog/catalog.hpp"
#include "step/step.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace amber;
using Clock = std::chrono::steady_clock;
constexpr std::size_t warmup = 1000, samples = 2000;

nlohmann::json measure(std::size_t unrelated) {
  ObjectState objects_state;
  catalog::State catalog_state;
  react::State react_state{{4096, 50, 5, 64}};
  perm::State permissions;
  permissions.plots.cell_m = 72;
  permissions.plots.rows.push_back({1, 1, 0, 0, {0, 0, 16, 32}, {}});
  permissions.plots.rows.push_back({2, 1, 0, 0, {16, 0, 32, 32}, {}});
  perm::rebuild_plot_index(permissions.plots);
  ObjectInit init;
  init.model = 1;
  init.owner = 1;
  init.plot = 2;
  init.position = {20, 10, 0};
  for (std::size_t i = 0; i < unrelated; ++i)
    create(objects_state.objects, init);
  init.plot = 1;
  init.position = {8, 10, 0};
  std::array<Handle, 10> expected;
  for (auto &object : expected)
    object = create(objects_state.objects, init);
  step::bind_metadata(objects_state, catalog_state, react_state, permissions,
                      perm::LandSettings{100, 1000, 72, 8, 16, 4, 4});
  react::ScriptDeclaration declaration;
  declaration.id = 1;
  declaration.author = 1;
  const auto loaded = react::load_script(
      react_state.vm,
      react::compile_script(
          "return {on={query=function(self) assert(#self:plot_objects(1)==10) end}}"),
      declaration);
  if (loaded.status != react::RunOutcome::Status::Ok)
    throw std::runtime_error(loaded.error);

  react::ScriptState state;
  react::DeferredEffects effects;
  const react::HandlerRef handler{1, "query"};
  std::vector<double> elapsed;
  elapsed.reserve(samples);
  std::size_t checksum = 0;
  for (std::size_t i = 0; i < warmup + samples; ++i) {
    react::Invocation invocation;
    invocation.self = {expected.front(), 1};
    invocation.state = &state;
    invocation.effects = &effects;
    invocation.deadline = RunDeadline::after(5);
    const auto start = Clock::now();
    const auto result = react::run_handler(react_state.vm, handler, invocation);
    const auto ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    if (result.status != react::RunOutcome::Status::Ok)
      throw std::runtime_error(result.error);
    if (!std::ranges::is_permutation(invocation.acquired, expected))
      throw std::runtime_error("plot query did not return exactly the ten placed things");
    if (i >= warmup) {
      elapsed.push_back(ns);
      checksum += invocation.acquired.size();
    }
  }
  std::ranges::sort(elapsed);
  return {{"unrelated_live_objects", unrelated},
          {"queried_plot_objects", expected.size()},
          {"warmup_calls", warmup},
          {"samples", samples},
          {"handler_ns_min", elapsed.front()},
          {"handler_ns_p50", elapsed[samples / 2]},
          {"handler_ns_p95", elapsed[samples * 95 / 100]},
          {"handler_ns_max", elapsed.back()},
          {"returned_handles_checksum", checksum}};
}
} // namespace

int main() {
  try {
    nlohmann::json results = nlohmann::json::array();
    for (const auto unrelated : {1000U, 100000U})
      results.push_back(measure(unrelated));
    std::cout << nlohmann::json(
                     {{"workload", "one real Luau self:plot_objects(1) call returning ten things"},
                      {"includes",
                       "Luau handler entry and exit, plot lookup, deadline checks, returned table "
                       "and handle allocation, result-size assertion"},
                      {"exclusions",
                       "world population, script compilation and loading, warmup, invocation "
                       "setup, returned-handle verification, scheduler, network and physics"},
                      {"results", std::move(results)}})
                     .dump(2)
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
