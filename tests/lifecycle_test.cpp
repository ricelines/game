#include "snap/snap.hpp"
#include "store/store.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

using namespace amber;
using namespace amber::lifecycle;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class Fn> void rejects(Fn run, const char *message) {
  try {
    run();
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error(message);
}
bool near(Vec3 a, Vec3 b) {
  return (a - b).length() < 0.00001f;
}
AssemblyTemplate original() {
  return {1, 0, 10, {{"leaf", 2, 11, {1, 0, 0}, {}}, {"window", 3, 12, {0, 0, 2}, {}}}};
}
ObjectInit initial() {
  ObjectInit init;
  init.position = {10, 20, 3};
  const float s = std::sqrt(0.5f);
  init.rotation = {0, 0, s, s};
  init.owner = 7;
  init.maker = 8;
  init.created_step = 19;
  init.plot = 3;
  init.preset = 97;
  init.flags = Static | Body | NoCopy;
  return init;
}
void creation_and_release() {
  Objects objects;
  Parts parts;
  Relation declared;
  auto born = instantiate(objects, parts, declared, original(), initial());
  const auto root = born.root, leaf = born.objects[1], window = born.objects[2];
  require(born.objects.size() == 3 && objects.model[root.slot()] == 1,
          "root plus ordinary named instances");
  require(near(objects.position[leaf.slot()], {10, 21, 3}) &&
              near(objects.position[window.slot()], {10, 20, 5}),
          "part translation composed with rotated root");
  require(objects.parent[leaf.slot()] == root && objects.material[leaf.slot()] == 11 &&
              !(objects.flags[leaf.slot()] & Body) && (objects.flags[leaf.slot()] & NoCopy),
          "parts have their own default material and restricted flags");
  auto visitor_init = initial();
  visitor_init.model = 99;
  visitor_init.parent = root;
  auto visitor = create(objects, visitor_init);
  require(collect(parts, identity(objects), root) == born.objects,
          "ordinary occupants are excluded from declared assembly");
  const std::array roots{root, root, leaf};
  const auto closed = collect(parts, identity(objects), roots);
  require(closed == born.objects && identity(objects).valid(leaf) &&
              identity(objects).valid(window),
          "collection deduplicates declared rows without releasing live identities");
  const auto stale = Handle::make(root.slot(), root.generation() + 1);
  require(collect(parts, identity(objects), std::span{&stale, 1}).empty(),
          "stale roots cannot select a live generation for release");
  release_rows(objects, parts, declared, closed);
  require(parts.value.empty() && declared.in.value.empty() && declared.out.value.empty() &&
              !identity(objects).valid(root) && !identity(objects).valid(leaf) &&
              !identity(objects).valid(window),
          "row release removes the exact declared closure and incident part edges");
  require(identity(objects).valid(visitor), "occupant remains alive");
  const auto reused = create(objects, visitor_init);
  require(identity(objects).valid(reused) && reused != root && !identity(objects).valid(root),
          "new creation leaves the retired handle invalid");
}
void topology_and_attribution() {
  Objects objects;
  Parts parts;
  Relation declared;
  auto born = instantiate(objects, parts, declared, original(), initial());
  const auto root = born.root, leaf = born.objects[1], window = born.objects[2];
  catalog::Models models;
  models.rows.resize(8);
  models.rows[2].material = 11;
  // A deliberate instance override differs from its old model default.
  objects.material[leaf.slot()] = 70;
  objects.group[leaf.slot()] = 31;
  const auto next =
      AssemblyTemplate{1, 5, 10, {{"bolt", 5, 14, {0, -2, 0}, {}}, {"leaf", 4, 13, {2, 0, 0}, {}}}};
  const auto plan = plan_replace(models, parts, identity(objects), transforms(objects),
                                 objects.params, root, next);
  const auto removed = collect(parts, identity(objects), plan.removed);
  const auto added = apply_replacement(objects, parts, declared, plan, 23, 50);
  const auto bolt = added.objects.at(0);
  require(added.root == root && bolt != window && identity(objects).valid(window) &&
              std::ranges::is_permutation(collect(parts, identity(objects), root),
                                          std::array{root, bolt, leaf}),
          "replacement unlinks removed parts but keeps their identities live for owner cleanup");
  require(objects.model[leaf.slot()] == 4 && objects.material[leaf.slot()] == 70 &&
              objects.group[leaf.slot()] == 31 && objects.maker[leaf.slot()] == 8 &&
              objects.created_step[leaf.slot()] == 19,
          "matching part retains authority, material override and attribution");
  require(objects.owner[bolt.slot()] == 7 && objects.maker[bolt.slot()] == 23 &&
              objects.created_step[bolt.slot()] == 50 && objects.material[bolt.slot()] == 14 &&
              near(objects.position[bolt.slot()], {12, 20, 3}),
          "added part records this edit maker/time and authored transform");
  require(objects.params[root.slot()] == 5 && objects.maker[root.slot()] == 8 &&
              near(objects.position[leaf.slot()], {10, 22, 3}),
          "root identity/attribution survive the binding replacement");
  release_rows(objects, parts, declared, removed);
  require(!identity(objects).valid(window) && identity(objects).valid(root) &&
              identity(objects).valid(leaf) && identity(objects).valid(bolt),
          "final row release invalidates only the removed closure");
  const auto empty = AssemblyTemplate{1, 7, 10, {}};
  const auto no_parts = plan_replace(models, parts, identity(objects), transforms(objects),
                                     objects.params, root, empty, 80);
  const auto final_children = collect(parts, identity(objects), no_parts.removed);
  const auto no_births = apply_replacement(objects, parts, declared, no_parts, 23, 52);
  require(no_births.objects.empty() && !parts.get(root.slot()) &&
              objects.material[root.slot()] == 80 && identity(objects).valid(root) &&
              identity(objects).valid(leaf) && identity(objects).valid(bolt) &&
              collect(parts, identity(objects), root) == std::vector{root},
          "variant removes every named part while keeping retired identities available");
  release_rows(objects, parts, declared, final_children);
  require(identity(objects).valid(root) && !identity(objects).valid(leaf) &&
              !identity(objects).valid(bolt),
          "cleanup frees all former parts last without replacing the root handle");
}
void expansion_and_snapshot() {
  catalog::Models models;
  models.rows.resize(3);
  models.rows[1].id = 1;
  models.rows[1].asset = "metadata-only-root";
  models.rows[1].material = 10;
  models.rows[1].parts["new_part"] = 2;
  models.rows[2].material = 11;
  catalog::Assets assets;
  auto &manifest = assets.rows["metadata-only-root"].manifest;
  manifest = {{"parts", {{"new_part", {{"position", {1, 2, 3}}, {"rotation", {0, 0, 0, 2}}}}}}};
  catalog::ParameterBlocks blocks;
  const auto expanded = expand(models, blocks, assets, 1);
  require(expanded.parts.size() == 1 && expanded.parts[0].name == "new_part" &&
              expanded.parts[0].rotation.w == 1,
          "catalog expansion reads actual part metadata and normalizes quaternion");
  manifest["parts"]["new_part"]["rotation"] = {0, 0, 0, 0};
  rejects([&] { expand(models, blocks, assets, 1); }, "zero part quaternion accepted");
  Objects objects;
  Parts parts;
  Relation declared;
  const auto a = instantiate(objects, parts, declared, expanded, initial());
  const auto b = instantiate(objects, parts, declared, original(), initial());
  snap::Buffer buffer;
  snap::Output out{buffer};
  const auto selected = std::span<const Handle>(a.objects);
  write_parts(out, parts, identity(objects), selected);
  out.adapter().flush();
  buffer.resize(out.adapter().writtenBytesCount());
  Parts restored;
  Relation restored_relation;
  snap::Input in{buffer.data(), buffer.size()};
  read_parts(in, restored, identity(objects), restored_relation);
  snap::check_complete(in);
  require(restored.value.size() == 1 && restored.get(a.root.slot()) &&
              !restored.get(b.root.slot()) &&
              restored_relation.out.get(a.root.slot())->at(0) == a.objects[1],
          "capture restores selected names and derives full-handle part edges");
  snap::Buffer empty;
  snap::Output empty_out{empty};
  write_parts(empty_out, parts, identity(objects), std::span<const Handle>{});
  empty_out.adapter().flush();
  empty.resize(empty_out.adapter().writtenBytesCount());
  snap::Input empty_in{empty.data(), empty.size()};
  read_parts(empty_in, restored, identity(objects), restored_relation);
  snap::check_complete(empty_in);
  require(restored.value.empty() && restored_relation.out.value.empty(),
          "explicit empty capture differs from whole-world encoding");
}
} // namespace
int main() {
  try {
    creation_and_release();
    topology_and_attribution();
    expansion_and_snapshot();
    std::cout << "Assembly row lifecycle, topology, generations and snapshot tests passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
