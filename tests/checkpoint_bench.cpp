#include "checkpoint_fixture.hpp"
#include <iostream>

using namespace amber;
using namespace amber::checkpoint_test;
int main() {
  Fixture fixture;
  fixture.populate(10000);
  snap::Buffer bytes;
  const auto writers = fixture.writers();
  constexpr int count = 20;
  auto start = drive::Monotonic::now();
  for (int i = 0; i < count; ++i)
    snap::encode(bytes, fixture.header, writers);
  auto milliseconds =
      std::chrono::duration<double, std::milli>(drive::Monotonic::now() - start).count() / count;
  const std::array<std::string_view, 1> versions{"checkpoint-fixture"};
  Fixture restored;
  auto view = snap::parse(bytes, versions);
  start = drive::Monotonic::now();
  snap::decode(view, restored.readers());
  auto restore = std::chrono::duration<double, std::milli>(drive::Monotonic::now() - start).count();
  test_runtime::require(restored.f.objects.model.size() == fixture.f.objects.model.size(),
                        "benchmark restores actual authoritative rows");
  std::cout << "13-section encode with 10004 objects: " << milliseconds << " ms; " << bytes.size()
            << " bytes; table decode: " << restore << " ms\n";
}
