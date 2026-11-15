#include "structural_fixture.hpp"

using namespace amber;
using namespace amber::react;
using test_runtime::require;
using test_structural::Fixture;
namespace {
// Nested Lua bookkeeping, failure retention and self-retirement run through the
// real application in tests/game_actions.py; these are direct native boundaries.
void creation_checks_build_and_native_capacity_before_allocation() {
  Fixture x;
  x.add("return {kind='model',on={['novel.touch']=function(self)end}}");
  const auto count = x.f.objects.model.size();
  const auto denied = x.create({{"model", "m:1"}, {"position", {1, 0, 0}}});
  require(denied.reply.contains("denied") && x.f.objects.model.size() == count,
          "missing Build must not allocate any part identities");
  x.allow_build();
  const auto made = x.create({{"model", "m:1"}, {"position", {1, 0, 0}}});
  const auto &objects = made.created_objects;
  require(objects.size() == 2 && x.f.objects.owner[objects[0].slot()] == 1 &&
              space::bounds(x.world, identity(x.f.objects), objects[0]).has_value() &&
              space::bounds(x.world, identity(x.f.objects), objects[1]).has_value() &&
              x.f.objects.parent[objects[1].slot()] == objects[0],
          "create installs actual root/child native geometry and declared attachment");
  Fixture full(1);
  full.add("return {kind='model',on={['novel.touch']=function(self)end}}");
  full.allow_build();
  const auto before = full.f.objects.model.size();
  const auto blocked = full.create({{"model", "m:1"}, {"position", {1, 0, 0}}});
  require(blocked.reply.contains("denied") && full.f.objects.model.size() == before &&
              space::capacity(full.world).native_available == 1,
          "native capacity failure must not allocate an object or install half an assembly");
}
void native_reconfiguration_retains_identity_and_real_geometry() {
  Fixture x;
  x.add("return {kind='model',on={['novel.touch']=function(self)end}}");
  x.allow_build();
  auto initial = x.create({{"model", "m:1"}, {"position", {1, 0, 0}}});
  auto objects = initial.created_objects;
  require(objects.size() == 2, "initial assembly created");
  const auto root = objects[0], leaf = objects[1];
  x.f.state(root, 1, 37);
  x.variants();
  auto denied = x.replace(root, {{"model", "m:1"}, {"params", 1}});
  require(denied.reply.contains("denied") && x.f.objects.params[root.slot()] == 0,
          "owning a script installation does not grant Edit on a separate created item");
  x.grant(root, perm::bit(perm::Verb::Edit));
  x.grant(leaf, perm::bit(perm::Verb::Edit));
  auto edited = x.replace(root, {{"model", "m:1"}, {"params", 1}});
  require(edited.reply.at("effect") == "applied" && x.f.objects.params[root.slot()] == 1 &&
              identity(x.f.objects).valid(leaf) &&
              read_state(x.f.properties, x.f.processes, root, 1, 0, 20).value.numeric() == 37 &&
              lifecycle::collect(x.parts, identity(x.f.objects), root).size() == 3,
          "ordinary Edit changes geometry/topology while retaining matching handles and state");
  require(space::bounds(x.world, identity(x.f.objects), root)->max.x > 1.9f,
          "reconfiguration updates actual native shape instead of metadata alone");
  const auto holder = perm::Holder::script(x.script, x.f.a);
  require(!perm::check(holder, perm::Verb::Destroy,
                       perm::target(identity(x.f.objects), authority(x.f.objects), x.limits, root),
                       x.players, x.grants, 0),
          "Edit authority never implies Destroy");
  x.grant(root, perm::bit(perm::Verb::Destroy));
  const auto removal = step::remove_instance(x.parts, *x.f.projection, authority(x.f.objects),
                                             attribution(x.f.objects), x.ledger, x.f.models,
                                             x.f.scripts, x.call(root));
  require(removal.removed_objects == lifecycle::collect(x.parts, identity(x.f.objects), root) &&
              identity(x.f.objects).valid(root) && identity(x.f.objects).valid(leaf),
          "removal returns the exact still-live closure for the writer's ordered owner cleanup");
}
void hidden_reconfiguration_stays_hidden_and_destroy_needs_no_reach() {
  Fixture x;
  x.add("return {kind='model',on={['novel.touch']=function(self)end}}");
  x.allow_build();
  const auto initial = x.create({{"model", "m:1"}, {"position", {1, 0, 0}}});
  const auto objects = initial.created_objects;
  const auto root = objects[0];
  x.variants();
  for (auto h : objects)
    x.grant(h, perm::bit(perm::Verb::Edit));
  std::vector<space::RelationChange> changes;
  require(space::custody(x.world, identity(x.f.objects), root, x.f.a, parents(x.f.objects),
                         space_flags(x.f.objects), x.physical, changes,
                         std::span(objects).subspan(1))
                  .status == space::Status::Ok,
          "fixture stores assembly in hidden inventory");
  x.f.objects.preset[root.slot()] |= perm::bit(perm::Verb::Edit);
  auto edited = x.replace(root, {{"model", "m:1"}, {"params", 1}});
  require(edited.changed && !space::bounds(x.world, identity(x.f.objects), root),
          "rebuilding hidden inventory does not make its root or children query visible");
  for (auto h : lifecycle::collect(x.parts, identity(x.f.objects), root))
    require(!space::bounds(x.world, identity(x.f.objects), h),
            "hidden child remains absent from scene");
  x.grant(root, perm::bit(perm::Verb::Destroy));
  const auto removed = step::remove_instance(x.parts, *x.f.projection, authority(x.f.objects),
                                             attribution(x.f.objects), x.ledger, x.f.models,
                                             x.f.scripts, x.call(root));
  require(removed.reply.at("ok") && !removed.removed_objects.empty() &&
              identity(x.f.objects).valid(root),
          "administrative removal identifies hidden goods without invented reach or early free");
}

} // namespace
int main() {
  try {
    creation_checks_build_and_native_capacity_before_allocation();
    native_reconfiguration_retains_identity_and_real_geometry();
    hidden_reconfiguration_stays_hidden_and_destroy_needs_no_reach();
    std::cout << "Direct construction, capacity and real geometry replacement checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
