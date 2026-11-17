#include "catalog/catalog.hpp"
#include "runtime_fixture.hpp"
#include "space_fixture.hpp"
#include "step/step.hpp"
#include <toml++/toml.hpp>

#include <algorithm>
#include <limits>

using namespace amber;
using test_runtime::require;
namespace {
void light_replication_lifetimes() {
  test_runtime::Fixture f;
  space::State space_state{testspace::settings(), 8};
  auto &space = space_state.space;
  auto &parts = f.objects_state.parts;
  auto &declared = f.objects_state.declared_parts;
  auto &assets = f.catalog_state.assets;
  auto &parameters = f.catalog_state.parameters;
  f.terms.rows[4].combine = react::Combine::None;
  const auto root_hash = catalog::sha256("projection root");
  const auto child_hash = catalog::sha256("projection child");
  const auto variant_hash = catalog::sha256("projection variant");
  f.models.rows.resize(3);
  for (ModelId id : {1u, 2u}) {
    f.models.rows[id].id = id;
    f.models.rows[id].asset = id == 1 ? root_hash : child_hash;
    f.models.rows[id].material = 8;
  }
  catalog::Asset asset;
  asset.hash = root_hash;
  asset.maximum = {1, 1, 1};
  asset.canonical_params = "{}";
  asset.replicated = {4};
  asset.manifest = {{"bindings",
                     {{"light", {{"intensity", "novel.signal"}, {"site", {0, 0, 1}}, {"range", 4}}},
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
  init.material = 8;
  init.owner = 1;
  init.flags = Static;
  const auto assembly = lifecycle::instantiate(f.objects, parts, declared,
                                               {1, 0, 8, {{"leaf", 2, 8, {1, 0, 0}, {}}}}, init);
  const auto root = assembly.root, child = assembly.objects[1];
  testspace::Box box({.2f, .2f, .2f});
  const auto geometry = space::import_geometry(space, box.source()).geometry;
  for (const auto object : assembly.objects)
    require(space::add(space, identity(f.objects), transforms(f.objects),
                       {object,
                        geometry,
                        object == root ? space::Motion::Static : space::Motion::Kinematic,
                        {.5f, 0, 0},
                        1,
                        false,
                        false,
                        {}})
                    .status == space::Status::Ok,
            "real projection fixture collision import failed");
  space::PhysicalRelations &physical = space_state.physical_relations;

  require(
      space::attach(f.objects_state, space_state, {child, root, {{1, 0, 0}, {}}, false}).status ==
          space::Status::Ok,
      "real declared child attachment failed");
  const auto sensor = space::sensor_box_geometry(space, {4, 4, 4});
  require(space::add_volume(space, identity(f.objects), transforms(f.objects),
                            {root, sensor, {{0, 0, 2}, {}}, space::VolumeKind::Zone})
                  .status == space::Status::Ok,
          "real authored zone sensor failed");
  f.state(root, 4, 100.);
  amber::step::State step_state;
  step::IlluminationSources &illumination = step_state.illumination_sources;
  observe::Lights &lights = (space_state.lights = observe::Lights{8});
  observe::LightSubscriptions &points =
      (space_state.light_subscriptions = observe::LightSubscriptions{8});
  observe::ZoneAmbients &zones = space_state.zones;
  observe::ActiveViews &views = (space_state.active_views = observe::ActiveViews{8});
  net::ReplicationIndex replicas{{32, 2, 3, 65536, 1048576}};
  const auto capacity = space::capacity(space).native_available;
  step::NativeCompletion born;
  born.visible_objects = assembly.objects;
  step::project_visibility(illumination, space_state, replicas, transforms(f.objects),
                           f.catalog_state, *f.projection, born, .05f,
                           amber::step::Clock{.step = 0, .hz = 20});
  require(lights.rows.size() == 1 && zones.rows.value.size() == 1 &&
              space::capacity(space).native_available == capacity,
          "projections lost illumination or duplicated native geometry");
  net::view_interest(replicas, 1, {});
  require(
      net::publication(replicas, identity(f.objects), transforms(f.objects), *f.projection, 0, 20)
              .upserts.size() == 2,
      "ordinary root and child were absent from interested publication");
  observe::subscribe(points, {root, {0, 0, 1}, 3});
  observe::subscribe(points, {child, {20, 0, 1}, 3});
  (void)observe::take_dirty(points);
  require(space::set_pose(space, identity(f.objects), transforms(f.objects), root, {{20, 0, 0}, {}})
                  .status == space::Status::Ok,
          "real root movement failed");
  space::StepResult physics;
  space::step(space, f.objects_state, physical, 1, .05f, {}, physics);
  step::move_illumination(lights, points, views, transforms(f.objects), physics.moved);
  require(observe::take_dirty(points).size() == 2 &&
              lights.rows.at(root.bits).position == Vec3{20, 0, 1},
          "moving light failed to invalidate old and new point ranges");
  f.objects.params[root.slot()] = 1;
  step::register_illumination(illumination, space_state, transforms(f.objects), f.catalog_state,
                              *f.projection, std::span(&root, 1), .05f,
                              amber::step::Clock{.step = 1, .hz = 20});
  net::register_replication(replicas, root, identity(f.objects), transforms(f.objects), f.models,
                            parameters, assets, *f.projection);
  require(lights.rows.at(root.bits).position == Vec3{21, 0, 1} &&
              zones.rows.get(root.slot())->ambient_lux == 12 &&
              replicas.rows.at(root.bits).asset == variant_hash &&
              space::capacity(space).native_available == capacity,
          "variant projection lost parameter-dependent light/zone or duplicated a sensor");

  f.states[{root, 77}].insert(std::string("kept"), 42.);
  const auto bounds = space::volume_bounds(space, root, space::VolumeKind::Zone);
  require(bounds.has_value(), "visible authored zone must have its actual sensor bounds");
  const Vec3 zone_only{bounds->min.x + .1f, bounds->min.y + .1f, bounds->max.z - .1f};
  require((zone_only - lights.rows.at(root.bits).position).length() > 4,
          "zone observer fixture must lie beyond the point light's range");
  observe::subscribe(points, {f.c, zone_only, 3});
  (void)observe::take_dirty(points);
  step::NativeCompletion hidden;
  hidden.custody = {{root, {}, false}};
  step::change_custody(f.objects_state, space_state, {}, {}, illumination, hidden);
  const auto dirty = observe::take_dirty(points);
  require(std::ranges::any_of(dirty, [&](const auto &point) { return point.subject == f.c; }) &&
              !space::volume_bounds(space, root, space::VolumeKind::Zone),
          "custody must dirty old zone observers before removing the real Jolt sensor");
  step::refresh_object_plots(space, f.objects_state, f.react_state, f.catalog_state,
                             {.step = 1, .hz = 20}, {}, hidden);
  step::project_visibility(illumination, space_state, replicas, transforms(f.objects),
                           f.catalog_state, *f.projection, hidden, .05f,
                           amber::step::Clock{.step = 1, .hz = 20});
  require(hidden.visible_objects.empty() && hidden.hidden_objects.size() == 2 &&
              lights.rows.empty() && zones.rows.value.empty() && replicas.rows.empty() &&
              !space::bounds(space, identity(f.objects), root) &&
              std::get<double>(f.states.at({root, 77}).values.at(std::string("kept"))) == 42,
          "hidden assembly kept ghost projections or lost private state");

  step::retire_physics(space, f.objects_state, physical, step_state, assembly.objects, {}, {},
                       hidden);
  net::SessionViews sessions;
  catalog::InstanceTexts text;
  step::retire_publication(replicas, sessions, text, assembly.objects);
  lifecycle::release_rows(f.objects, parts, declared, assembly.objects);
  require(!identity(f.objects).valid(root) && !identity(f.objects).valid(child) &&
              space::capacity(space).native_available == capacity + 3,
          "owner cleanup left geometry or old-generation projection data");
  auto removed =
      net::publication(replicas, identity(f.objects), transforms(f.objects), *f.projection, 2, 20);
  require(removed.removed.size() == 2, "retirement lost visible root/child removals");
  init.model = 2;
  const auto reused = create(f.objects, init);
  require(identity(f.objects).valid(reused) && reused != root,
          "new object reused a retired identity");
  step::register_illumination(illumination, space_state, transforms(f.objects), f.catalog_state,
                              *f.projection, std::span(&reused, 1), .05f,
                              amber::step::Clock{.step = 2, .hz = 20});
  require(lights.rows.empty() && zones.rows.value.empty() && !replicas.rows.contains(root.bits),
          "reused slot inherited retired light/zone or replication identity");
}
void complete_assembly_refund() {
  test_runtime::Fixture f;
  const std::array objects{f.a, f.b};
  f.objects.owner[f.a.slot()] = f.objects.owner[f.b.slot()] = 1;
  amber::perm::State perm_state;
  perm::Players &players = perm_state.players;
  players.rows.resize(2);
  perm::Ledger &ledger = perm_state.ledger;
  const auto wallet = perm::Account::player(1);
  ledger.balance[wallet] = std::numeric_limits<std::int64_t>::max() - 10;
  ledger.balance[perm::Account::object(f.a)] = ledger.balance[perm::Account::object(f.b)] = 7;
  const auto before = ledger.balance;
  require(perm::can_refund_object_accounts(ledger, authority(f.objects), objects) !=
                  perm::Reason::Allowed &&
              ledger.balance == before,
          "aggregate refund overflow was accepted or changed money during validation");
  ledger.balance[wallet] = 1;

  step::NativeCompletion out;
  step::retire_commerce(perm_state, f.objects_state, objects, 1, out);
  require(perm::balance(ledger, wallet) == 15 &&
              !ledger.balance.contains(perm::Account::object(f.a)) &&
              !ledger.balance.contains(perm::Account::object(f.b)) && out.records.size() == 2,
          "complete assembly refund lost funds or actual refund records");
  for (const auto &record : out.records)
    require(record.name == "account_refund" && record.detail.at("agent") == false &&
                record.detail.at("amount") == 7,
            "refund diagnostic became a player choice or lost its actual amount");
}
} // namespace
int main() {
  try {
    light_replication_lifetimes();
    complete_assembly_refund();
    std::cout << "Direct light/replication lifetime and funded retirement checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
