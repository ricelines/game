#include "structural_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>

using namespace amber;
using test_runtime::require;
namespace {
using Json = nlohmann::json;
struct Fixture : test_structural::Fixture {
  TermId action;
  explicit Fixture(std::uint32_t capacity = 64)
      : test_structural::Fixture(capacity),
        action(step::declare_model_edit(f.terms, declarations)) {
    add("return {kind='model',on={['novel.touch']=function(self)end}}");
    allow_build();
    f.models.rows[1].author = 1;
    f.models.rows[1].source_hash = std::string(64, '1');
    for (auto &[hash, asset] : assets.rows) {
      (void)hash;
      asset.source_hash = std::string(64, '1');
      asset.canonical_params = "{}";
      asset.manifest["parameter_schema"] = Json::object();
    }
    f.models.rows[1].canonical_params = "{}";
    f.models.rows[2].canonical_params = "{}";
  }
  std::vector<Handle> make(float x, std::uint32_t params = 0) {
    const auto out = create({{"model", "m:1"}, {"params", params}, {"position", {x, 0, 0}}});
    require(out.reply.at("ok"), "initial actual assembly creation");
    return out.created_objects;
  }
  void unrelated_observers() {
    f.models.rows[2].script = script;
    for (auto h : {f.a, f.b, f.c})
      f.objects.model[h.slot()] = 2;
    react::project_model(f.triggers, f.models, f.scripts, f.terms, 2);
  }
  void proposal(bool invalid = false) {
    f.materials.rows[9] = {{10, Value(500.)}, {11, Value(.5)}};
    f.models.rows.resize(6);
    for (ModelId id : {3u, 4u, 5u}) {
      auto &model = f.models.rows[id];
      model = f.models.rows[id == 3 ? 1 : 2];
      model.id = id;
      model.author = 2;
      model.material = 9;
      model.source_hash = std::string(64, '2');
      model.asset = std::string(64, char('c' + id - 3));
      model.script = 0;
      model.part_name = id == 3 ? "" : (id == 4 ? "leaf" : "tip");
      model.parts.clear();
      auto asset = assets.rows.at(std::string(64, id == 3 ? 'a' : 'b'));
      asset.hash = model.asset;
      asset.source_hash = model.source_hash;
      asset.volume = id == 3 ? 2 : .032;
      if (id == 3) {
        testspace::Box bigger({2, 1, 1});
        asset.minimum = bigger.bounds.min;
        asset.maximum = bigger.bounds.max;
        geometry.rows[asset.hash] = {space::import_geometry(world, bigger.source()).geometry, {}};
        asset.manifest["parts"] = {{"leaf", {{"position", {2, 0, 0}}, {"rotation", {0, 0, 0, 1}}}},
                                   {"tip", {{"position", {-2, 0, 0}}, {"rotation", {0, 0, 0, 1}}}}};
        asset.manifest["assembly"]["bounds"] = {{"min", {-2.1, -.5, 0}}, {"max", {2.1, .5, 1}}};
      } else {
        testspace::Box wider({.4f, .2f, .4f});
        asset.minimum = wider.bounds.min;
        asset.maximum = wider.bounds.max;
        geometry.rows[asset.hash] = {space::import_geometry(world, wider.source()).geometry, {}};
        asset.manifest["parts"] = Json::object();
      }
      assets.rows[asset.hash] = asset;
      catalog::project_measurements(f.assets, id, 0, asset, {});
    }
    f.models.rows[3].parts = {{"leaf", 4}, {"tip", 5}};
    if (invalid)
      assets.rows.at(f.models.rows[3].asset).manifest["bindings"]["motion"]["mode"] = "invalid";
  }
  step::NativeCompletion revise(Caller caller = PlayerId{1}) {
    return step::revise_model(f.objects_state, f.catalog_state, f.model_settings, {0, 0}, geometry,
                              *f.projection, world, space_state, perm_state, {10, 11}, {.6f, 0},
                              call({}, {{"model", "m:1"}, {"proposal", "m:3"}}, caller));
  }
  void offered(Handle root) {
    const auto out =
        step::offer(perm_state, f.objects_state, world,
                    call(root, {{"price", 5}}, PlayerId{f.objects.owner[root.slot()]}));
    require(out.reply.at("ok"), "ordinary placed goods offer");
  }
};
void two_owners_and_hidden_goods() {
  Fixture x;
  const auto first = x.make(1), second = x.make(5);
  x.unrelated_observers();
  for (auto h : second)
    x.f.objects.owner[h.slot()] = 2;
  x.f.state(first[0], 1, 37);
  x.f.terms.rows[12].id = 12;
  x.f.terms.rows[12].name = "novel.setting";
  x.f.terms.rows[12].source = react::Source::Setting;
  x.f.instance_settings.columns.resize(13);
  x.f.instance_settings.columns[12].set(first[1].slot(), Value(91.0));

  require(space::custody(x.world, x.f.objects_state, second[0], x.f.a, x.space_state,
                         std::span(second).subspan(1))
                  .status == space::Status::Ok,
          "second owner's goods enter actual hidden custody");
  x.offered(first[0]);
  x.proposal();
  const auto before = x.f.models.rows.size();
  auto denied = x.revise(PlayerId{2});
  require(denied.reply.contains("denied") && x.offers.rows[1].live &&
              x.f.models.rows.size() == before,
          "instance owner cannot edit another author's definition");
  denied = x.revise(ScriptHolder{x.script, x.f.a});
  require(denied.reply.contains("denied"), "script does not inherit model author authority");
  const auto out = x.revise();
  require(out.reply.at("ok") && out.reply.at("effect") == "applied" && !x.offers.rows[1].live,
          out.reply.dump().c_str());
  const auto ids = identity(x.f.objects);
  for (const auto &objects : {first, second}) {
    require(ids.valid(objects[1]) && x.f.objects.model[objects[0].slot()] == 1 &&
                lifecycle::collect(x.parts, ids, objects[0]).size() == 3,
            "retained part handles survive and a new ordinary child is installed");
  }
  require(
      x.f.objects.owner[first[0].slot()] == 1 && x.f.objects.owner[second[0].slot()] == 2 &&
          react::read_state(x.f.properties, x.f.processes, first[0], 1, 0, 20).value.numeric() ==
              37 &&
          x.f.instance_settings.columns[12].get(first[1].slot())->numeric() == 91,
      "definition editing preserves owners, state and instance settings");
  require(space::bounds(x.world, ids, first[0])->max.x > 1.9f &&
              !space::bounds(x.world, ids, second[0]),
          "real visible collision changes while hidden inventory stays hidden");
  const auto retained = x.f.objects.model[first[1].slot()];
  const auto children = lifecycle::collect(x.parts, ids, first[0]);
  require(retained != 4 && x.f.models.rows[retained].material == 8 &&
              x.f.models.rows[retained].author == x.f.models.rows[2].author &&
              x.f.objects.material[first[0].slot()] == 8 &&
              x.f.objects.material[first[1].slot()] == 8 &&
              x.f.objects.material[children.back().slot()] == 9,
          "retained defaults/materials survive while the new part uses its proposal material");
}
void capacity_rejection_preserves_definition_but_cancels_offer() {
  Fixture x(5);
  const auto a = x.make(1), b = x.make(5);
  x.unrelated_observers();
  x.offered(a[0]);
  x.proposal();
  const auto old = x.f.models.rows[1].asset;
  const auto count = x.f.models.rows.size();
  const auto out = x.revise();
  require(!out.reply.at("ok").get<bool>() &&
              out.reply.at("problem").get<std::string>().find("capacity") != std::string::npos &&
              x.f.models.rows[1].asset == old && x.f.models.rows.size() == count &&
              lifecycle::collect(x.parts, identity(x.f.objects), a[0]).size() == 2 &&
              lifecycle::collect(x.parts, identity(x.f.objects), b[0]).size() == 2,
          "combined capacity rejection changes neither definition nor instance identities");
  require(!x.offers.rows[1].live && !(x.f.objects.flags[a[0].slot()] & Escrow),
          "the earlier ordinary offer cancellation remains after geometry rejection");
  const auto cancellation = std::ranges::find_if(
      out.records, [](const auto &row) { return row.name == "goods_offers_invalidated"; });
  require(cancellation != out.records.end() && cancellation->changed && out.changed &&
              out.result == record::Result::Denied,
          "the native result retains real cancellation records/effects beside a denied edit");
}
void default_and_explicit_parameters_update_in_place() {
  Fixture x;
  auto old = x.assets.rows.at(x.f.models.rows[1].asset);
  old.hash = std::string(64, 'g');
  old.canonical_params = R"({"length":2})";
  x.assets.rows[old.hash] = old;
  x.geometry.rows[old.hash] = x.geometry.rows.at(x.f.models.rows[1].asset);
  x.parameters.rows.push_back({1, 1, old.hash, old.canonical_params, {{"leaf", 2}}});
  catalog::project_measurements(x.f.assets, 1, 1, old, {});
  const auto a = x.make(1), b = x.make(5, 1);
  x.unrelated_observers();
  x.proposal();
  auto &root = x.assets.rows.at(x.f.models.rows[3].asset);
  root.canonical_params = R"({"length":3})";
  root.manifest["parameter_schema"] = {{"length", {3, .1, 10}}};
  x.f.models.rows[3].canonical_params = root.canonical_params;
  auto variant = root;
  variant.hash = std::string(64, 'h');
  variant.canonical_params = R"({"length":2})";
  testspace::Box longer({4, 1, 1});
  variant.volume = 4;
  variant.minimum = longer.bounds.min;
  variant.maximum = longer.bounds.max;
  x.assets.rows[variant.hash] = variant;
  x.geometry.rows[variant.hash] = {space::import_geometry(x.world, longer.source()).geometry, {}};
  x.parameters.rows.push_back(
      {2, 3, variant.hash, variant.canonical_params, {{"leaf", 4}, {"tip", 5}}});
  catalog::project_measurements(x.f.assets, 3, 2, variant, {});
  const auto count = x.parameters.rows.size();
  const auto result = x.revise();
  require(result.reply.at("ok") && x.parameters.rows.size() == count &&
              x.f.objects.params[a[0].slot()] == 0 && x.f.objects.params[b[0].slot()] == 1 &&
              catalog::resolve_asset(x.f.models, x.parameters, 1, 1).hash == variant.hash,
          "default and explicit instances keep their parameter identities on the new definition");
  require(space::bounds(x.world, identity(x.f.objects), a[0])->max.x > 1.9f &&
              space::bounds(x.world, identity(x.f.objects), b[0])->max.x > 6.9f,
          "actual native shapes follow each independently baked new parameter set");
  require(x.f.assets.parameter_rows.at(1).volume == 4,
          "existing parameter block's measured facts follow its new geometry");
}
void two_edits_share_one_rigid_assembly() {
  Fixture x;
  const auto a = x.make(1), b = x.make(5);
  x.unrelated_observers();
  ObjectInit initial;
  initial.model = 2;
  initial.material = 8;
  initial.owner = 1;
  initial.flags = Dynamic;
  const auto parent = create(x.f.objects, initial);
  step::install_physics(
      x.world, identity(x.f.objects), transforms(x.f.objects), space_flags(x.f.objects), parent,
      step::describe_physics(x.geometry, x.assets.rows.at(x.f.models.rows[2].asset),
                             *x.f.projection, {2, 0, 8, 0, parent}, Dynamic, {10, 11}, {.6f, 0}, 0,
                             20));
  x.parts.set(parent.slot(), std::vector<lifecycle::NamedPart>{{"first", a[0]}, {"second", b[0]}});
  x.f.objects.parent[a[0].slot()] = parent;
  x.f.objects.parent[b[0].slot()] = parent;
  lifecycle::rebuild_declared_parts(x.parts, identity(x.f.objects), x.declared);
  const std::array<Handle, 2> joined{a[0], b[0]};
  step::install_declared_attachments(x.world, x.f.objects_state, x.space_state, joined);
  auto all = lifecycle::collect(x.parts, identity(x.f.objects), parent);
  require(space::assemble(x.world, identity(x.f.objects), transforms(x.f.objects), parent,
                          std::span(all).subspan(1), space::Motion::Dynamic)
                  .status == space::Status::Ok,
          "fixture promotes two separate target instances into an actual rigid assembly");
  x.proposal();
  const auto out = x.revise();
  require(
      out.reply.at("ok") &&
          lifecycle::collect(x.parts, identity(x.f.objects), parent).size() == 7 &&
          space::assembly_root(x.world, identity(x.f.objects), a[0]) == parent &&
          space::assembly_root(x.world, identity(x.f.objects), b[0]) == parent &&
          identity(x.f.objects).valid(a[1]) && identity(x.f.objects).valid(b[1]),
      "multiple affected roots rebuild their shared rigid assembly once with retained identities");
}
void removed_parts_use_ordinary_retirement() {
  Fixture x;
  const auto a = x.make(1);
  x.unrelated_observers();
  const auto grant = x.grant(a[1], perm::bit(perm::Verb::Use));
  x.f.state(a[1], 1, 19);
  x.proposal();
  x.f.models.rows[3].parts.erase("leaf");
  x.assets.rows.at(x.f.models.rows[3].asset).manifest["parts"].erase("leaf");
  const auto result = x.revise();
  require(result.reply.at("ok") && identity(x.f.objects).valid(a[1]) &&
              result.removed_objects == std::vector<Handle>{a[1]},
          "geometry replacement leaves retired identities live for owner cleanup");
  step::NativeCompletion cleanup;
  step::retire_commerce(x.perm_state, x.f.objects_state, result.removed_objects, 0, cleanup);
  react::retire_reactions(x.f.models, *x.f.projection, x.f.react_state, result.removed_objects, 0,
                          20, {}, cleanup.effects);
  lifecycle::release_rows(x.f.objects, x.parts, x.declared, result.removed_objects);
  require(!identity(x.f.objects).valid(a[1]) && !x.grants.rows[grant].live &&
              !x.f.properties[1].get(a[1].slot()) &&
              lifecycle::collect(x.parts, identity(x.f.objects), a[0]).size() == 2,
          "owner cleanup removes state/grants before the old child identity is freed");
}
void invalid_geometry_is_checked_before_catalog_mutation() {
  Fixture x;
  const auto a = x.make(1);
  x.unrelated_observers();
  x.proposal(true);
  const auto old = x.f.models.rows[1].asset;
  const auto out = x.revise();
  require(!out.reply.at("ok").get<bool>() && x.f.models.rows[1].asset == old &&
              identity(x.f.objects).valid(a[1]),
          "invalid authored physics cannot leave a changed catalog behind");
}
void benchmark() {
  std::vector<double> samples;
  for (unsigned sample = 0; sample < 20; ++sample) {
    Fixture x(512);
    std::vector<Handle> roots;
    for (unsigned i = 0; i < 100; ++i)
      roots.push_back(x.make(float(i % 10) - 5)[0]);
    x.unrelated_observers();
    x.proposal();
    const auto started = std::chrono::steady_clock::now();
    const auto result = x.revise();
    const auto elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    require(result.reply.at("ok"), "benchmark edit must actually succeed");
    for (auto root : roots)
      require(lifecycle::collect(x.parts, identity(x.f.objects), root).size() == 3,
              "benchmark must rebuild every ordinary assembly");
    samples.push_back(elapsed);
  }
  std::ranges::sort(samples);
  std::cout
      << "100-instance definition edit (200 -> 300 native object shapes, direct catalog/geometry, "
         "no physics/navigation step or VM queue drain), ms: p50="
      << samples[9] << " p95=" << samples[18] << " max=" << samples.back() << '\n';
}
} // namespace
int main(int argc, char **argv) {
  try {
    two_owners_and_hidden_goods();
    capacity_rejection_preserves_definition_but_cancels_offer();
    invalid_geometry_is_checked_before_catalog_mutation();
    default_and_explicit_parameters_update_in_place();
    two_edits_share_one_rigid_assembly();
    removed_parts_use_ordinary_retirement();
    if (argc == 2 && std::string_view(argv[1]) == "--bench")
      benchmark();
    std::cout << "native model definition integration checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
