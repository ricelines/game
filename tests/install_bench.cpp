#include "runtime_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>

using namespace amber;
using namespace amber::react;
namespace {
using Clock = std::chrono::steady_clock;
void report(const char *name, unsigned rows, std::vector<double> &samples) {
  std::sort(samples.begin(), samples.end());
  std::cout << name << " rows=" << rows << " samples=" << samples.size()
            << " p50_us=" << samples[samples.size() / 2]
            << " p95_us=" << samples[samples.size() * 95 / 100] << " max_us=" << samples.back()
            << '\n';
}
void watches(unsigned count) {
  test_runtime::Fixture f{1000};
  const auto script = f.add(R"(return {kind='instance',watches={'novel.heat'},on={
    ['novel.heat']=function(self) self.state.count=(self.state.count or 0)+1 end}})");
  std::vector<Handle> objects;
  ObjectInit init;
  init.model = 1;
  for (unsigned i = 0; i < count; ++i) {
    const auto h = create(f.objects, init);
    objects.push_back(h);
    set_instances(f.installed, h, std::span(&script, 1), f.scripts);
    project_instance(f.triggers, f.installed, f.scripts, f.terms, h);
    f.state(h, 1, 0);
    f.baseline(h);
  }
  constexpr unsigned iterations = 2000;
  std::vector<double> samples;
  samples.reserve(iterations);
  for (unsigned i = 0; i < iterations; ++i) {
    f.effects = {};
    const auto h = objects[i % count];
    const auto begin = Clock::now();
    f.state(h, 1, i + 1);
    const auto plans = f.changed({h, 1}, i);
    test_runtime::require(plans.size() == 1, "dirty source must deliver exactly its own watch");
    f.run(plans, i);
    samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
  }
  unsigned delivered = 0;
  for (auto h : objects)
    if (const auto state = f.states.find({h, script}); state != f.states.end())
      delivered += unsigned(std::get<double>(state->second.values.at(std::string("count"))));
  test_runtime::require(delivered == iterations,
                        "actual Luau deliveries must preserve exact count");
  report("own_state_dirty_direct_watch_delivery_luau", count, samples);
}
void school_parts() {
  test_runtime::Fixture f{1000};
  const auto script = f.add(R"(return {kind='model',on={['novel.cue']=function(self,_,rate)
    self:rate('novel.heat',rate,0,1)
    for _,part in self:parts() do self:rate('novel.heat',rate,0,1,part) end
  end}})");
  std::vector<Handle> parts;
  ObjectInit init;
  init.model = 1;
  for (unsigned i = 0; i < 300; ++i)
    parts.push_back(create(f.objects, init));
  constexpr unsigned iterations = 1000;
  std::vector<double> samples;
  samples.reserve(iterations);
  for (unsigned i = 0; i < iterations; ++i) {
    f.effects = {};
    Invocation invocation;
    invocation.self = {f.a, script};
    invocation.parts = parts;
    invocation.payload = Value(i & 1 ? .01 : -.01);
    invocation.state = &f.states[invocation.self];
    invocation.random = &f.random[f.a.bits];
    invocation.effects = &f.effects;
    invocation.step = i;
    invocation.hz = 20;
    const auto begin = Clock::now();
    const auto outcome = run_handler(f.vm, {script, "novel.cue"}, invocation);
    test_runtime::require(outcome.status == RunOutcome::Status::Ok,
                          "one school controller must mutate all actual own parts");
    samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
  }
  test_runtime::require(f.processes[1].value.size() == 301,
                        "one root and three hundred ordinary part process rows exist");
  report("one_luau_controller_300_declared_part_rates", 300, samples);
}
} // namespace
int main() {
  std::cout << "Scope: actual value mutation, direct watch lookup, comparison, invocation and "
               "in-memory journal; "
               "no physics, transport, snapshots or pacing. The 300-part workload "
               "measures the school's shared-environment process update, not swimming geometry.\n";
  watches(100);
  watches(1000);
  school_parts();
}
