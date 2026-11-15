#include "snap/objects.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace amber;
int main() {
  using Clock = std::chrono::steady_clock;
  Objects objects;
  ObjectInit init;
  init.model = 1;
  init.owner = 2;
  init.material = 3;
  for (std::uint32_t i = 0; i != 100000; ++i) {
    init.position = {float(i % 1000), float(i / 1000), 0};
    create(objects, init);
  }
  snap::Buffer bytes;
  std::vector<double> encode_ms, decode_ms;
  const std::array<std::string_view, 1> versions{"benchmark-objects-only"};
  for (int sample = 0; sample != 100; ++sample) {
    bytes.clear();
    const auto start = Clock::now();
    snap::Output out{bytes};
    snap::begin(out, {std::string(versions[0]), 123, {}});
    for (auto tag : snap::tags)
      snap::section(out, tag, [&](snap::Output &s) {
        if (tag == snap::tags[0])
          snap::write_objects(s, objects);
      });
    snap::finish(out, bytes);
    const auto encoded = Clock::now();
    const auto view = snap::parse(bytes, versions);
    Objects restored;
    snap::Input in{view.sections[0].bytes.data(), view.sections[0].bytes.size()};
    snap::read_objects(in, restored);
    const auto decoded = Clock::now();
    if (restored.position != objects.position || restored.model != objects.model)
      return 1;
    encode_ms.push_back(std::chrono::duration<double, std::milli>(encoded - start).count());
    decode_ms.push_back(std::chrono::duration<double, std::milli>(decoded - encoded).count());
  }
  auto distribution = [](std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return nlohmann::json{{"p50_ms", values[49]}, {"p95_ms", values[94]}, {"p99_ms", values[98]}};
  };
  std::cout
      << nlohmann::json{{"scope", "100000 OBJS rows, empty other sections; not full-world restore"},
                        {"build", "Release C++20"},
                        {"samples", 100},
                        {"bytes", bytes.size()},
                        {"encode_including_checksum", distribution(encode_ms)},
                        {"decode_including_checksum_and_allocations", distribution(decode_ms)}}
             .dump(2)
      << '\n';
}
