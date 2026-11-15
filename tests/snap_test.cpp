#include "snap/codec.hpp"
#include "snap/objects.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace amber;
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class F> void rejects(F action, const char *message) {
  bool failed = false;
  try {
    action();
  } catch (const std::runtime_error &) {
    failed = true;
  }
  require(failed, message);
}
snap::Buffer snapshot(const Objects &objects, Step step,
                      std::optional<std::span<const Handle>> selection = std::nullopt) {
  snap::Buffer bytes;
  snap::Output out{bytes};
  snap::begin(out, {"test-runtime", step, {}});
  for (auto tag : snap::tags)
    snap::section(out, tag, [&](snap::Output &s) {
      if (tag == snap::tags[0])
        snap::write_objects(s, objects, selection);
    });
  snap::finish(out, bytes);
  return bytes;
}
snap::View parse(const snap::Buffer &bytes) {
  const std::array<std::string_view, 1> versions{"test-runtime"};
  return snap::parse(bytes, versions);
}
Objects restore(const snap::Buffer &bytes) {
  const auto view = parse(bytes);
  Objects objects;
  const auto section = view.sections[0].bytes;
  snap::Input in{section.data(), section.size()};
  snap::read_objects(in, objects);
  return objects;
}
void object_continuity_and_capture_identity() {
  Objects objects;
  ObjectInit init;
  init.model = 3;
  init.material = 5;
  init.owner = 2;
  init.maker = 1;
  init.created_step = 42;
  init.position = {3, -2, 8};
  init.rotation = {0, 0, 1, 0};
  init.flags = Dynamic | NoCopy;
  init.preset = 0x12345;
  init.plot = 9;
  init.params = 8;
  const auto first = create(objects, init);
  const auto dead = create(objects, init);
  init.parent = first;
  const auto child = create(objects, init);
  destroy(objects, dead);
  const auto bytes = snapshot(objects, 123);
  auto restored = restore(bytes);
  require(restored.generation == objects.generation && restored.position == objects.position &&
              restored.rotation == objects.rotation && restored.model == objects.model &&
              restored.material == objects.material && restored.owner == objects.owner &&
              restored.maker == objects.maker && restored.created_step == objects.created_step &&
              restored.group == objects.group && restored.preset == objects.preset &&
              restored.plot == objects.plot && restored.parent == objects.parent &&
              restored.flags == objects.flags && restored.params == objects.params &&
              restored.free_slots == objects.free_slots,
          "OBJS round trip lost columns");
  require(create(restored, init) == create(objects, init), "restore changed slot reuse order");
  const std::array<Handle, 2> selected{first, child};
  const auto capture = snapshot(objects, 123, selected);
  const auto subset = restore(capture);
  require(identity(subset).valid(first) && identity(subset).valid(child) &&
              !identity(subset).valid(dead) && subset.parent[child.slot()] == first,
          "capture lost identity or included unselected objects");
  const auto empty = restore(snapshot(objects, 123, std::span<const Handle>{}));
  require(empty.model.size() == 1 && empty.free_slots.empty(), "empty capture copied full world");
}
void framing_and_corruption_are_explicit() {
  const auto bytes = snapshot(Objects{}, 0x0102030405060708ULL);
  const std::string magic(bytes.begin(), bytes.begin() + 8);
  require(magic == "AMBSNAP1" && bytes[8] == 1 && bytes[9] == 0 && bytes[10] == 0 && bytes[11] == 0,
          "header fixed encoding changed");
  const auto view = parse(bytes);
  require(view.header.step == 0x0102030405060708ULL && view.sections.size() == 13 &&
              view.sections.front().tag == snap::tags.front() &&
              view.sections.back().tag == snap::tags.back(),
          "header/order mismatch");
  for (std::size_t size : {std::size_t{0}, std::size_t{40}, bytes.size() - 1})
    rejects([&] { parse(snap::Buffer(bytes.begin(), bytes.begin() + size)); },
            "truncated snapshot accepted");
  auto damaged = bytes;
  damaged[30] ^= 1;
  rejects([&] { parse(damaged); }, "checksum corruption accepted");
  const std::array<std::string_view, 1> wrong{"other-runtime"};
  rejects([&] { snap::parse(bytes, wrong); }, "unloadable runtime accepted");
  snap::Buffer sparse;
  snap::Output out{sparse};
  SparseColumn<Value> column;
  column.set(19, Value(1.5));
  column.set(2, Value(Handle::make(8, 3)));
  snap::write_sparse(out, column, snap::object_field<snap::Output, Value>);
  sparse.resize(out.adapter().writtenBytesCount());
  snap::Input in{sparse.data(), sparse.size()};
  SparseColumn<Value> copied;
  snap::read_sparse(in, copied, snap::object_field<snap::Input, Value>);
  snap::check_complete(in);
  copied.rebuild_positions(20);
  require(*copied.get(19) == Value(1.5) && *copied.get(2) == Value(Handle::make(8, 3)),
          "sparse pairs lost order/value");
}
void async_compression_reuses_buffers_and_retains_only_own_files() {
  const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory = std::filesystem::current_path() / ("snap-test-" + std::to_string(unique));
  {
    snap::Writer writer(directory, 1, 3);
    std::ofstream(directory / "keep.txt") << "unrelated";
    auto first = writer.acquire();
    auto second = writer.acquire();
    require(first && second && !writer.acquire(), "snapshot buffer pool is unbounded");
    *first = snapshot(Objects{}, 10);
    *second = snapshot(Objects{}, 20);
    writer.submit(std::move(*first), 10);
    writer.submit(std::move(*second), 20);
    writer.finish();
    require(writer.stats().completed == 2 && writer.stats().source_bytes > 0 &&
                writer.acquire().has_value(),
            "writer did not finish/recycle");
  }
  require(!std::filesystem::exists(directory / "snap-10.amb.zst") &&
              std::filesystem::exists(directory / "snap-20.amb.zst") &&
              std::filesystem::exists(directory / "keep.txt"),
          "retention pruned wrong files");
  const auto decoded = snap::read_file(directory / "snap-20.amb.zst");
  require(parse(decoded).header.step == 20, "async file did not restore");
  rejects([&] { snap::read_file(directory / "absent"); }, "missing file accepted");
  std::filesystem::remove_all(directory);
}
} // namespace
int main() {
  try {
    object_continuity_and_capture_identity();
    framing_and_corruption_are_explicit();
    async_compression_reuses_buffers_and_retains_only_own_files();
    std::cout << "snapshot foundation tests passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
