#include "bindings/allocation/allocation.hpp"
#include "runtime_fixture.hpp"

#include <chrono>
#include <iostream>

using namespace amber;
using namespace amber::react;
int main() {
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
  GivesTo gives;
  MeterRows meters;
  PriorityLinks links;
  bindings::AllocationReconciliation tables{f.allocations, meters, links};
  set_contribution(f.shared, gives, f.terms.rows[10], f.a, f.c, 7, f.effects);
  set_contribution(f.shared, gives, f.terms.rows[11], f.a, f.c, 6, f.effects);
  set_contribution(f.shared, gives, f.terms.rows[11], f.b, f.c, 7, f.effects);
  ObjectInit init;
  init.model = 1;
  for (unsigned i = 0; i < 10000; ++i) {
    const auto h = create(f.objects, init);
    f.shared[10].set(h.slot(), Shared{7, {{h, 7}}});
    f.shared[11].set(h.slot(), Shared{9, {{h, 9}}});
  }
  rebuild_priorities(links, *f.projection);
  constexpr unsigned count = 100000;
  std::size_t changed = 0;
  auto begin = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < count; ++i) {
    f.effects = {};
    set_contribution(f.shared, gives, f.terms.rows[10], f.a, f.c, (i & 1) ? 8 : 6, f.effects);
    bindings::reconcile_allocations(*f.projection, tables, f.effects, i, 20);
    changed += !f.effects.ordered.empty();
  }
  const auto elapsed =
      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin);
  test_runtime::require(changed == count && get_share(f.allocations, f.c, 11, f.a) +
                                                    get_share(f.allocations, f.c, 11, f.b) ==
                                                8,
                        "measured reconciliation produces actual conserved allocation changes");
  std::cout << "indexed contribution reconciliation unrelated_subjects=10000 ns/op="
            << elapsed.count() / count << '\n';
  f.instance_settings.columns.resize(13);
  begin = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < count; ++i) {
    f.effects = {};
    f.instance_settings.columns[12].set(f.b.slot(), Value((i & 1) ? -1.0 : 1.0));
    bindings::reconcile_priority(*f.projection, tables, {SettingKey::Scope::Instance, f.b.bits, 12},
                                 f.effects, count + i, 20);
  }
  const auto priority_elapsed =
      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin);
  test_runtime::require(
      get_share(f.allocations, f.c, 11, f.a) == 6 &&
          priority_inputs(links, {SettingKey::Scope::Instance, f.b.bits, 12}).size() == 1,
      "priority benchmark uses the actual indexed pair and final priority");
  std::cout << "indexed inherited priority reconciliation unrelated_subjects=10000 ns/op="
            << priority_elapsed.count() / count << '\n';
}
