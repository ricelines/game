#include "runtime_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

using namespace amber;
using namespace amber::react;
namespace {
using Clock = std::chrono::steady_clock;
void report(const char *name, const char *dimension, unsigned size, std::vector<double> &samples) {
  std::sort(samples.begin(), samples.end());
  std::cout << name << ' ' << dimension << '=' << size << " samples=" << samples.size()
            << " p50_us=" << samples[samples.size() / 2]
            << " p95_us=" << samples[samples.size() * 95 / 100] << " max_us=" << samples.back()
            << '\n';
}
void run(unsigned subjects) {
  test_runtime::Fixture f;
  f.terms.rows.resize(13);
  f.terms.rows[10].id = 10;
  f.terms.rows[10].combine = Combine::Sum;
  f.terms.rows[11].id = 11;
  f.terms.rows[11].combine = Combine::SharePriority;
  f.terms.rows[11].paired = 10;
  f.terms.rows[11].priority = 12;
  f.terms.rows[12].id = 12;
  f.terms.rows[12].source = Source::Setting;
  set_contribution(f.shared, f.gives_to, f.terms.rows[10], f.a, f.c, 7, f.effects);
  set_contribution(f.shared, f.gives_to, f.terms.rows[11], f.a, f.c, 6, f.effects);
  set_contribution(f.shared, f.gives_to, f.terms.rows[11], f.b, f.c, 7, f.effects);
  ObjectInit init;
  init.model = 1;
  for (unsigned i = 1; i < subjects; ++i) {
    const auto h = create(f.objects, init);
    set_contribution(f.shared, f.gives_to, f.terms.rows[10], h, h, 7, f.effects);
    set_contribution(f.shared, f.gives_to, f.terms.rows[11], h, h, 9, f.effects);
  }
  validate_shared_terms(f.terms);
  reconcile_allocations(*f.projection, f.react_state, f.effects, 0, 20);
  constexpr unsigned count = 2000;
  std::vector<double> samples;
  samples.reserve(count);
  for (unsigned i = 0; i < count; ++i) {
    f.effects = {};
    const auto begin = Clock::now();
    set_contribution(f.shared, f.gives_to, f.terms.rows[10], f.a, f.c, (i & 1) ? 8 : 6, f.effects);
    reconcile_allocations(*f.projection, f.react_state, f.effects, i, 20);
    samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
  }
  test_runtime::require(
      get_share(f.allocations, f.c, 11, f.a) + get_share(f.allocations, f.c, 11, f.b) == 8,
      "dirty reconciliation preserves delivered supply");
  report("dirty_contribution_and_reconcile", "subjects", subjects, samples);
  f.instance_settings.columns.resize(13);
  samples.clear();
  for (unsigned i = 0; i < count; ++i) {
    f.effects = {};
    const auto begin = Clock::now();
    f.instance_settings.columns[12].set(f.b.slot(), Value((i & 1) ? -1.0 : 1.0));
    reconcile_priority(*f.projection, f.react_state, {SettingKey::Scope::Instance, f.b.bits, 12},
                       f.effects, count + i, 20);
    samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
  }
  test_runtime::require(
      get_share(f.allocations, f.c, 11, f.a) == 6 &&
          priority_inputs(*f.projection, f.gives_to, {SettingKey::Scope::Instance, f.b.bits, 12})
                  .size() == 1,
      "instance priority selects the actual affected pair");
  report("instance_priority_and_reconcile", "subjects", subjects, samples);
}
void equal_sharing(unsigned demanders) {
  const auto subject = Handle::make(demanders + 1, 1);
  Shared supply, demand;
  for (unsigned i = 0; i < demanders; ++i) {
    const double wanted = i + 1;
    demand.parts.push_back({Handle::make(i + 1, 1), wanted});
    demand.total += wanted;
  }
  std::reverse(demand.parts.begin(), demand.parts.end());
  AllocationRows allocations;
  PriorityValues priority;
  std::vector<double> samples;
  constexpr unsigned warmup = 100, count = 1000;
  samples.reserve(count);
  for (unsigned i = 0; i < warmup + count; ++i) {
    supply.total = demand.total * ((i & 1) ? .25 : .5);
    const auto begin = Clock::now();
    allocate(supply, demand, Combine::ShareEqual, priority, {subject, 11}, allocations);
    if (i >= warmup)
      samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
  }
  double delivered = 0;
  for (const auto &[contributor, share] : allocations.values.at({subject, 11})) {
    (void)contributor;
    delivered += share;
  }
  test_runtime::require(std::abs(delivered - supply.total) < supply.total * 1e-10,
                        "equal sharing preserves scarce supply");
  report("equal_water_filling", "demanders", demanders, samples);
}
} // namespace
int main() {
  std::cout
      << "Scope: direct primary rows; all subjects have live allocations, one dirty subject "
         "with two demanders. The separate equal-sharing workload has one subject, distinct "
         "demands in descending order, and 100 warmup calls before 1000 measured calls. "
         "No VM, physics, transport, snapshot or frame pacing. Reconciliation samples include "
         "mutation; equal-sharing samples include division and current-row replacement.\n";
  run(100);
  run(1000);
  equal_sharing(100);
  equal_sharing(10000);
}
