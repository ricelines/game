#include "react/react.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace amber;
using namespace amber::react;
namespace {
using Clock = std::chrono::steady_clock;
template <class Fn> void bench(const char *name, std::size_t iterations, Fn run) {
  const auto start = Clock::now();
  const double checksum = run();
  const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
  std::cout << name << " iterations=" << iterations
            << " ns/op=" << elapsed / static_cast<double>(iterations) << " checksum=" << checksum
            << '\n';
}
} // namespace
int main() {
  constexpr std::size_t count = 100000;
  Process process{20, 0, -0.2, 0, 100, 1};
  bench("closed_form_threshold", count, [&]() {
    double checksum = 0;
    for (std::size_t i = 0; i < count; ++i) {
      auto p = process;
      p.v0 += static_cast<double>(i % 17);
      auto due = next_crossing(p, {Threshold::Predicate::AtMost, 10}, 0, 20);
      checksum += static_cast<double>(due.value_or(0));
    }
    return checksum;
  });
  Properties properties;
  Processes processes;
  DeferredEffects effects;
  Term term;
  term.id = 1;
  term.source = Source::State;
  std::vector<Handle> objects;
  for (std::uint32_t i = 1; i <= 10000; ++i)
    objects.push_back(Handle::make(i, 1));
  bench("sparse_state_set_read", count, [&]() {
    double checksum = 0;
    for (std::size_t i = 0; i < count; ++i) {
      auto object = objects[i % objects.size()];
      set_state(properties, processes, term, object, Value(static_cast<double>(i)), i, 20, effects);
      checksum += read_state(properties, processes, object, 1, i, 20).value.numeric();
      effects.ordered.clear();
    }
    return checksum;
  });
  Vm vm({4096, 50, 5, 64});
  ScriptDeclaration declaration;
  declaration.id = 1;
  auto compiled =
      compile_script("return {on={tick=function(self) local v=self.state.n or 0; self.state.n=v+1; "
                     "self.state.tag='live'; self.state.h=self.handle end}}");
  auto loaded = load_script(vm, compiled, declaration);
  if (loaded.status != RunOutcome::Status::Ok)
    throw std::runtime_error(loaded.error);
  std::vector<ScriptState> states(1000);
  std::vector<std::uint64_t> random(1000, 1);
  HandlerRef handler{1, "tick"};
  bench("real_luau_state_proxy", count, [&]() {
    double checksum = 0;
    for (std::size_t i = 0; i < count; ++i) {
      const auto row = i % states.size();
      Invocation invocation;
      invocation.self = {objects[row], 1};
      invocation.state = &states[row];
      invocation.random = &random[row];
      invocation.effects = &effects;
      invocation.deadline = Clock::now() + std::chrono::milliseconds(5);
      auto result = run_handler(vm, handler, invocation);
      if (result.status != RunOutcome::Status::Ok)
        throw std::runtime_error(result.error);
      checksum += std::get<double>(states[row].at(StateKey{std::string("n")}));
      effects.ordered.clear();
    }
    return checksum;
  });
}
