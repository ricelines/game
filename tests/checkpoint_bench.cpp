#include "checkpoint_fixture.hpp"
#include <iostream>

using namespace amber;
using namespace amber::checkpoint_test;
int main() {
  Fixture fixture;
  fixture.populate(10000);
  snap::Buffer bytes;
  constexpr int count = 20;
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < count; ++i)
    fixture.write(bytes);
  auto milliseconds =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() /
      count;
  const std::array<std::string_view, 1> versions{"checkpoint-fixture"};
  Fixture restored;
  auto view = snap::parse(bytes, versions);
  start = std::chrono::steady_clock::now();
  restored.read(view);
  auto restore =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  test_runtime::require(restored.f.objects.model.size() == fixture.f.objects.model.size(),
                        "benchmark restores actual authoritative rows");
  std::cout << "13-section encode with 10004 objects: " << milliseconds << " ms; " << bytes.size()
            << " bytes; table decode: " << restore << " ms\n";
}
