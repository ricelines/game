#include "scene_fixture.hpp"
#include "space_fixture.hpp"
#include <limits>

using namespace amber;
namespace bi = amber::bindings;
namespace ca = amber::catalog;
namespace re = amber::react;
namespace ob = amber::observe;
using testspace::require;
namespace {
struct Fixture {
  test_runtime::Fixture f;
  space::Space space{testspace::settings()};
  lifecycle::Parts parts;
  Relation declared;
  lifecycle::Instances instances{f.objects, parts, declared};
  ca::ParameterBlocks parameters;
  ca::Assets assets;
  space::PhysicalRelations physical;
  std::vector<space::RelationChange> changes;
  space::StepResult result;
  perm::Players players;
  perm::Grants grants;
  perm::OwnerLimits limits;
  perm::Offers offers;
  perm::Trades trades;
  perm::Plots plots;
  perm::Ledger ledger;
  testscene::Projection projection{f.objects, f.models,        parameters, assets, *f.projection,
                                   f.runtime, f.subscriptions, declared,   {2, 3}};
  testscene::Lifecycle lifecycle{f,        space,   instances,  parts,   declared,
                                 physical, changes, projection, players, grants,
                                 limits,   offers,  trades,     plots,   ledger};
  lifecycle::Created assembly;
  std::string root_hash = ca::sha256("projection root"),
              child_hash = ca::sha256("projection child"),
              variant_hash = ca::sha256("projection variant");
  Fixture() {
    f.terms.rows.resize(7);
    f.terms.rows[6].kind = re::Kind::Event;
    f.projection->density = 1;
    f.properties.resize(6);
    f.processes.resize(6);
    for (TermId id = 1; id < 6; ++id) {
      f.terms.rows[id] = {};
      f.terms.rows[id].id = id;
      f.terms.rows[id].name = "unfamiliar.quantity" + std::to_string(id);
      f.terms.by_name[f.terms.rows[id].name] = id;
    }
    f.terms.rows[1].source = re::Source::Intrinsic;
    f.terms.rows[1].dimensions = {-3, 1, 0, 0, 0, 0, 0, 0};
    f.terms.rows[2].source = re::Source::Measured;
    f.terms.rows[2].measurement = re::MeasurementRole::Mass;
    f.terms.rows[2].dimensions = {0, 1, 0, 0, 0, 0, 0, 0};
    f.terms.rows[3] = f.terms.rows[2];
    f.terms.rows[3].id = 3;
    f.terms.rows[3].measurement = re::MeasurementRole::Runtime;
    f.materials.rows[5][1] = Value(2000.);
    f.assets.rows[1].volume = .001;
    f.assets.rows[2].volume = .002;
    f.assets.parameter_rows[1].volume = .003;
    f.models.rows.resize(3);
    for (ModelId id : {1u, 2u}) {
      f.models.rows[id].id = id;
      f.models.rows[id].asset = id == 1 ? root_hash : child_hash;
      f.models.rows[id].material = 5;
    }
    ca::Asset asset;
    asset.hash = root_hash;
    asset.maximum = {1, 1, 1};
    asset.canonical_params = "{}";
    asset.replicated = {4};
    asset.manifest = {
        {"bindings",
         {{"light", {{"intensity", f.terms.rows[4].name}, {"site", {0, 0, 1}}, {"range", 4}}},
          {"zone", {{"ambient", 7}, {"priority", 2}}}}}};
    assets.rows[root_hash] = asset;
    asset.hash = child_hash;
    asset.manifest["bindings"] = nlohmann::json::object();
    assets.rows[child_hash] = asset;
    asset = assets.rows.at(root_hash);
    asset.hash = variant_hash;
    asset.manifest["bindings"]["light"]["site"] = {1, 0, 1};
    asset.manifest["bindings"]["zone"]["ambient"] = 12;
    assets.rows[variant_hash] = asset;
    parameters.rows.push_back({1, 1, variant_hash, "{}", {}});
    ObjectInit init;
    init.model = 1;
    init.material = 5;
    init.owner = 1;
    init.flags = Static;
    assembly = lifecycle::instantiate(f.objects, parts, declared,
                                      {1, 0, 5, {{"leaf", 2, 5, {1, 0, 0}, {}}}}, init);
    testspace::Box box({.2f, .2f, .2f});
    const auto geometry = space::import_geometry(space, box.source()).geometry;
    for (auto h : assembly.objects)
      require(space::add(space, identity(f.objects), transforms(f.objects),
                         {h,
                          geometry,
                          h == assembly.root ? space::Motion::Static : space::Motion::Kinematic,
                          {.5f, 0, 0},
                          1,
                          false,
                          false,
                          {}})
                      .status == space::Status::Ok,
              "fixture physics installed by its geometry owner");
    const auto child = assembly.objects[1];
    require(space::attach(space, identity(f.objects), transforms(f.objects), parents(f.objects),
                          space_flags(f.objects), physical.contains, physical.supports,
                          physical.attached_to, {child, assembly.root, {{1, 0, 0}, {}}, false},
                          changes)
                    .status == space::Status::Ok,
            "fixture real child attachment");
    const auto sensor = space::sensor_box_geometry(space, {4, 4, 4});
    require(space::add_volume(space, identity(f.objects), transforms(f.objects),
                              {assembly.root, sensor, {{0, 0, 2}, {}}, space::VolumeKind::Zone})
                    .status == space::Status::Ok,
            "fixture zone sensor installed once");
    f.properties[4].set(assembly.root.slot(), Value(100.));
  }
  void birth(std::span<const Handle> objects) {
    re::DeferredEffects effects;
    bi::objects_created(lifecycle.lifecycle, objects, true, {}, effects);
  }
  void retire(Handle root) {
    const auto closure = instances.closure(std::span(&root, 1));
    std::string error;
    require(bi::can_retire_objects(lifecycle.lifecycle, closure, error), error.c_str());
    re::DeferredEffects effects;
    bi::retire_objects(lifecycle.lifecycle, closure, {}, effects);
  }
  void tick() {
    space::step(space, identity(f.objects), transforms(f.objects), parents(f.objects),
                space_flags(f.objects), physical, 1, .05f, {}, result);
  }
};
void birth_move_rebind_retire() {
  Fixture f;
  const auto root = f.assembly.root, child = f.assembly.objects[1];
  const auto available = space::capacity(f.space).native_available;
  f.birth(f.assembly.objects);
  require(space::capacity(f.space).native_available == available &&
              f.projection.lights.rows.size() == 1 && f.projection.zones.rows.value.size() == 1 &&
              f.projection.masses.rows.at(root.bits).total_kg == 6,
          "birth registers actual projections without adding duplicate bodies or sensors");
  bi::view_interest(f.projection.replicas, 1, {});
  require(bi::publication(f.projection.replicas, identity(f.f.objects), transforms(f.f.objects),
                          *f.f.projection, 0, 20)
                  .upserts.size() == 2,
          "ordinary born root and part appear in interested publication");
  ob::subscribe(f.projection.points, {root, {0, 0, 1}, 3});
  ob::subscribe(f.projection.points, {child, {20, 0, 1}, 3});
  (void)ob::take_dirty(f.projection.points);
  require(space::set_pose(f.space, identity(f.f.objects), transforms(f.f.objects), root,
                          {{20, 0, 0}, {}})
                  .status == space::Status::Ok,
          "actual root move");
  f.tick();
  require(std::ranges::find(f.result.moved, root) != f.result.moved.end(),
          "direct pose mutation is present in the next spatial movement notification");
  bi::scene_moved(f.projection.scene, f.result.moved);
  require(ob::take_dirty(f.projection.points).size() == 2 &&
              f.projection.lights.rows.at(root.bits).position == Vec3{20, 0, 1},
          "light movement invalidates subscribed points in both old and new ranges");
  f.f.objects.params[root.slot()] = 1;
  const lifecycle::Rebinding change{root, {1, 5, 0}, {1, 5, 1}, {}, {}};
  re::DeferredEffects rebound;
  bi::objects_rebound(f.lifecycle.lifecycle, std::span(&change, 1), true, {}, rebound);
  require(f.projection.lights.rows.at(root.bits).position == Vec3{21, 0, 1} &&
              std::get<double>(f.projection.zones.rows.get(root.slot())->ambient) == 12 &&
              f.projection.masses.rows.at(root.bits).total_kg == 10 &&
              space::capacity(f.space).native_available == available,
          "parameter rebind uses real variant asset bindings and own mass");
  f.retire(root);
  require(!identity(f.f.objects).valid(root) && !identity(f.f.objects).valid(child) &&
              f.projection.lights.rows.empty() && f.projection.zones.rows.value.empty() &&
              f.projection.masses.rows.empty() && f.f.runtime.values.empty() &&
              f.projection.replicas.rows.empty() &&
              space::capacity(f.space).native_available == available + 3,
          "lifecycle cleanup removes actual bodies, sensors and every owned projection");
  auto publication = bi::publication(f.projection.replicas, identity(f.f.objects),
                                     transforms(f.f.objects), *f.f.projection, 1, 20);
  require(publication.removed.size() == 2, "retirement publishes both visible ordinary removals");
  ObjectInit init;
  init.model = 2;
  init.material = 5;
  init.flags = Static;
  const auto replacement = create(f.f.objects, init);
  require(replacement.slot() == root.slot() && replacement != root,
          "fixture reuses retired root slot with a new generation");
  f.birth(std::span(&replacement, 1));
  require(f.projection.lights.rows.empty() && f.projection.zones.rows.value.empty() &&
              f.projection.masses.rows.at(replacement.bits).total_kg == 4 &&
              !f.projection.replicas.rows.contains(root.bits),
          "reused slot inherits no retired light, zone, mass or replica identity");
}
void ancestor_retirement_and_custody() {
  Fixture f;
  const auto root = f.assembly.root, child = f.assembly.objects[1];
  f.birth(f.assembly.objects);
  f.retire(child);
  require(f.projection.masses.rows.at(root.bits).total_kg == 2 &&
              !f.projection.masses.rows.contains(child.bits),
          "retirement invalidates surviving old ancestors before graph removal");
  require(space::custody(f.space, identity(f.f.objects), root, {}, parents(f.f.objects),
                         space_flags(f.f.objects), f.physical, f.changes)
                  .status == space::Status::Ok,
          "actual offline custody extraction");
  bi::scene_visible(f.projection.scene, std::span(&root, 1), false);
  require(f.projection.lights.rows.empty() && f.projection.zones.rows.value.empty() &&
              f.projection.replicas.rows.empty() &&
              f.projection.masses.rows.at(root.bits).total_kg == 2 &&
              !space::bounds(f.space, identity(f.f.objects), root),
          "custody removes scene projections but retains actual measured goods mass");
}
void hidden_rebind_and_initialization() {
  Fixture f;
  const auto root = f.assembly.root;
  re::DeferredEffects initial;
  bi::initialize_objects(f.lifecycle.lifecycle, f.assembly.objects, f.assembly.objects, false, {},
                         initial);
  require(std::none_of(
              initial.ordered.begin(), initial.ordered.end(),
              [](const auto &effect) { return std::holds_alternative<step::EventWork>(effect); }),
          "fresh initialization does not duplicate the authored bootstrap start events");
  const auto child = f.assembly.objects[1];
  const std::array descendants{child};
  require(space::custody(f.space, identity(f.f.objects), root, {}, parents(f.f.objects),
                         space_flags(f.f.objects), f.physical, f.changes, descendants)
                  .status == space::Status::Ok,
          "fixture physically hides the whole declared assembly");
  bi::scene_visible(f.projection.scene, f.assembly.objects, false);
  f.f.objects.params[root.slot()] = 1;
  f.f.states[{root, 77}][std::string("kept")] = 42.;
  const lifecycle::Rebinding change{root, {1, 5, 0}, {1, 5, 1}, {}, {}};
  re::DeferredEffects rebound;
  bi::objects_rebound(f.lifecycle.lifecycle, std::span(&change, 1), false, {}, rebound);
  require(f.projection.lights.rows.empty() && f.projection.zones.rows.value.empty() &&
              f.projection.replicas.rows.empty() &&
              f.projection.masses.rows.at(root.bits).total_kg == 10 &&
              !space::bounds(f.space, identity(f.f.objects), root) &&
              !space::bounds(f.space, identity(f.f.objects), child) &&
              std::get<double>(f.f.states.at({root, 77}).at(std::string("kept"))) == 42.,
          "hidden rebind updates true measured mass and preserves state without ghost scene rows");
  re::DeferredEffects restored;
  bi::initialize_objects(f.lifecycle.lifecycle, f.assembly.objects, {}, true, {}, restored);
  require(f.projection.replicas.rows.empty() && f.projection.lights.rows.empty() &&
              std::none_of(restored.ordered.begin(), restored.ordered.end(),
                           [](const auto &effect) {
                             return std::holds_alternative<step::EventWork>(effect);
                           }),
          "restore retains hidden visibility and never restarts authored state");
  testspace::Box shape({.2f, .2f, .2f});
  const auto geometry = space::import_geometry(f.space, shape.source()).geometry;
  for (auto h : f.assembly.objects)
    require(space::add(f.space, identity(f.f.objects), transforms(f.f.objects),
                       {h, geometry, space::Motion::Static, {.5f, 0, 0}, 1, false, false, {}})
                    .status == space::Status::Ok,
            "actual reveal restores native query shapes before projections");
  bi::scene_visible(f.projection.scene, f.assembly.objects, true);
  require(f.projection.replicas.rows.size() == 2 && f.projection.lights.rows.size() == 1 &&
              f.projection.zones.rows.value.size() == 1 &&
              f.projection.masses.rows.at(root.bits).total_kg == 10,
          "reveal restores every spatial projection using the revised asset without rebirth");
}
void changed_values_and_scene_sampling() {
  Fixture f;
  const auto root = f.assembly.root, child = f.assembly.objects[1];
  f.f.terms.rows[5].source = re::Source::Measured;
  f.f.terms.rows[5].measurement = re::MeasurementRole::Runtime;
  f.projection.illuminance.illuminance = 5;
  f.assets.rows.at(f.root_hash).manifest["bindings"]["zone"]["ambient"] = f.f.terms.rows[4].name;
  const auto script = f.f.add(R"(return {kind='model',watches={'unfamiliar.quantity5'},
    on={['unfamiliar.quantity5']=function(self) end}})");
  f.f.models.rows[2].script = script;
  f.birth(f.assembly.objects);
  require(f.projection.points.rows.size() == 1,
          "creation discovers the actual installed illuminance watch before sampling");
  ob::ActiveViews views(8);
  ob::track_view(views, 1, {1, 0, 1}, 1);
  ob::take_dirty_views(views);
  bi::IlluminanceSampling sampling{f.projection.points,
                                   f.space,
                                   *f.f.projection,
                                   f.projection.lights,
                                   f.projection.zones,
                                   f.f.runtime,
                                   1,
                                   100};
  re::DeferredEffects baseline;
  bi::advance_scene(f.projection.scene, sampling, views, {}, 0, 20, baseline);
  const auto before = re::read(*f.f.projection, child, 5, 0, 20);
  require(before.present && before.value.numeric() > 100,
          "scene sampling resolves actual light and bound zone ambient");
  bi::view_interest(f.projection.replicas, 1, {});
  (void)bi::publication(f.projection.replicas, identity(f.f.objects), transforms(f.f.objects),
                        *f.f.projection, 0, 20);
  const auto metadata_size = f.projection.metadata.objects.size();
  re::set_rate(f.f.properties, f.f.processes, f.f.terms.rows[4], root, -100, 0, 100, 0, 20,
               f.f.effects);
  const std::array changed{FactKey{root, 4}};
  bi::scene_values_changed(f.projection.scene, f.space, views, changed);
  require(f.projection.running_lights.sources.contains(root.bits) &&
              f.projection.running_zones.sources.contains(root.bits) &&
              f.projection.replicas.dirty.contains(root.bits) &&
              ob::take_dirty_views(views) == std::vector<std::uint64_t>{1} &&
              f.projection.metadata.objects.size() == metadata_size,
          "one fact notification updates replicas and both running source indexes without metadata "
          "spam");
  re::DeferredEffects terminal;
  bi::advance_scene(f.projection.scene, sampling, views, {}, 20, 20, terminal);
  const auto after = re::read(*f.f.projection, child, 5, 20, 20);
  require(after.present && after.value.numeric() < before.value.numeric() &&
              f.projection.running_lights.sources.empty() &&
              f.projection.running_zones.sources.empty() && terminal.changed,
          "terminal light and ambient sampling emits measured change before periodic work ends");
}
void funded_retirement_preflights_the_complete_owner_refund() {
  Fixture f;
  const auto root = f.assembly.root, child = f.assembly.objects[1];
  const auto owner = f.f.objects.owner[root.slot()];
  const auto wallet = perm::Account::player(owner);
  f.ledger.balance[wallet] = std::numeric_limits<std::int64_t>::max() - 10;
  f.ledger.balance[perm::Account::object(root)] = 7;
  f.ledger.balance[perm::Account::object(child)] = 7;
  const auto before = f.ledger.balance;
  const auto closure = f.instances.closure(std::span(&root, 1));
  std::string error;
  require(!bi::can_retire_objects(f.lifecycle.lifecycle, closure, error) &&
              f.ledger.balance == before && identity(f.f.objects).valid(root) &&
              identity(f.f.objects).valid(child) &&
              space::bounds(f.space, identity(f.f.objects), root).has_value(),
          "aggregate account overflow changed money, identity or real geometry before refusal");
  f.ledger.balance[wallet] = 1;
  require(bi::can_retire_objects(f.lifecycle.lifecycle, closure, error), error.c_str());
  re::DeferredEffects effects;
  bi::retire_objects(f.lifecycle.lifecycle, closure, {}, effects);
  require(perm::balance(f.ledger, wallet) == 15 &&
              !f.ledger.balance.contains(perm::Account::object(root)) &&
              !f.ledger.balance.contains(perm::Account::object(child)) &&
              !identity(f.f.objects).valid(root) && !identity(f.f.objects).valid(child) &&
              !space::bounds(f.space, identity(f.f.objects), root).has_value(),
          "actual funded assembly retirement lost funds or left spatial/identity rows alive");
  std::size_t receipts = 0;
  for (const auto &effect : effects.ordered)
    if (const auto *event = std::get_if<record::Event>(&effect);
        event && event->name == "account_refund") {
      require(event->detail.at("agent") == false && event->detail.at("amount") == 7,
              "refund diagnostic became a player choice or lost the actual amount");
      ++receipts;
    }
  require(receipts == 2, "funded part retirement omitted completed account refund records");
}
} // namespace
int main() {
  try {
    birth_move_rebind_retire();
    ancestor_retirement_and_custody();
    hidden_rebind_and_initialization();
    changed_values_and_scene_sampling();
    funded_retirement_preflights_the_complete_owner_refund();
    std::cout << "projection lifecycle checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
