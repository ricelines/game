#include "drive/drive.hpp"
#include "runtime_fixture.hpp"
#include <iostream>

using namespace amber;
int main() {
  test_runtime::Fixture f;
  auto script = f.add(R"(return {kind='model',on={['novel.cue']=function(self)
    self.state.n=(self.state.n or 0)+1 end}})");
  f.models.rows[1].script = script;
  install::project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  Relation parts;
  bindings::Execution execution{f.vm, f.states, f.random, identity(f.objects), parts, 1};
  step::EventWork event{f.a, f.b, 6, {}, {}};
  constexpr int count = 100000;
  auto start = drive::Monotonic::now();
  for (int i = 0; i < count; ++i) {
    auto plans = drive::plan_event(f.triggers, f.values(), event, 1, 20);
    auto result = drive::execute(execution, plans.at(0), {});
    test_runtime::require(result.invocation.outcome.status == react::RunOutcome::Status::Ok,
                          "benchmark handler must execute successfully");
  }
  auto elapsed = std::chrono::duration<double>(drive::Monotonic::now() - start).count();
  test_runtime::require(std::get<double>(f.states.at({f.a, script}).at(std::string("n"))) == count,
                        "benchmark retains actual authored state semantics");
  std::cout << "event plan + actual Luau + persistent state: " << elapsed * 1e9 / count
            << " ns/invocation; " << count / elapsed << " invocations/s\n";
}
