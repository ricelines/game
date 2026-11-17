#include "catalog/catalog.hpp"
#include "step/step.hpp"

#include <iostream>
#include <stdexcept>

using namespace amber;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct Fixture {
  amber::ObjectState objects_state;
  amber::catalog::State catalog_state;
  amber::react::State react_state{{4096, 50, 250, 64}};
  amber::perm::State perm_state;

  Objects &objects = objects_state.objects;
  Handle root, part, unrelated;
  catalog::Models &models = catalog_state.models;
  catalog::ParameterBlocks &parameters = catalog_state.parameters;
  catalog::Assets &assets = catalog_state.assets;
  react::Terms &terms = react_state.terms;
  react::Materials &materials = react_state.materials;
  react::Properties &properties = react_state.properties;
  react::Processes &processes = react_state.processes;
  Relation &parts = objects_state.declared_parts;
  SparseColumn<std::vector<ScriptId>> &instances = react_state.installations;
  perm::Plots &plots = perm_state.plots;
  perm::Players &players = perm_state.players;
  perm::Grants &grants = perm_state.grants;
  // This fixture verifies metadata behavior under sanitizers, not production timing.
  react::AssetMeasurements &measurements = catalog_state.baked_measurements;
  react::RuntimeMeasurements &runtime = react_state.runtime_measurements;
  react::Vm &vm = react_state.vm;
  react::ScriptState state;
  react::DeferredEffects effects;
  ScriptId next = 100;
  Fixture() {
    terms.rows.resize(7);
    for (TermId i = 1; i < terms.rows.size(); ++i) {
      terms.rows[i].id = i;
      terms.rows[i].name = "custom." + std::to_string(i);
      terms.by_name[terms.rows[i].name] = i;
    }
    terms.rows[1].source = react::Source::Intrinsic;
    terms.rows[3].kind = terms.rows[4].kind = react::Kind::Material;
    terms.rows[5].kind = react::Kind::Event;
    terms.rows[6].source = react::Source::Measured;
    measurements.rows[1].values[6] = Value(0.125);
    measurements.parameter_rows[1].values[6] = Value(0.25);
    materials.rows[3][1] = Value(10.0);
    materials.rows[4][1] = Value(20.0);
    models.rows.resize(4);
    for (ModelId id = 1; id < 4; ++id) {
      models.rows[id].id = id;
      models.rows[id].asset = "asset" + std::to_string(id);
      models.rows[id].material = id == 2 ? 4 : 3;
      auto &a = assets.rows[models.rows[id].asset];
      a.hash = models.rows[id].asset;
      a.manifest = nlohmann::json::object();
      a.volume = id == 3 ? 4 : id;
      a.minimum = {};
      a.maximum = {2, 2, 2};
    }
    models.rows[1].script = 8;
    models.rows[1].parts["old"] = 2;
    auto &variant = assets.rows["variant"];
    variant.hash = "variant";
    variant.volume = 3;
    variant.minimum = {-1, -1, -1};
    variant.maximum = {1, 1, 1};
    variant.manifest = {{"variant_marker", "actual"},
                        {"capacity", 99.0},
                        {"capacity_method", "standard_exact"},
                        {"bounds", {{"min", {-1, -1, -1}}, {"max", {1, 1, 1}}}},
                        {"size", {2, 2, 2}},
                        {"volume", 3.0},
                        {"supports", nlohmann::json::array({{{"height", 1}, {"area", 4.0}}})},
                        {"assembly", {{"bounds", {{"min", {-1, -1, -1}}, {"max", {4, 2, 2}}}}}},
                        {"seats", nlohmann::json::array({{{"position", {1, 2, 3}}}})}};
    parameters.rows.push_back({1, 1, "variant", "{}", {{"new", 3}}});
    ObjectInit init;
    init.model = 1;
    init.params = 1;
    init.material = 4;
    init.owner = 2;
    init.plot = 1;
    init.position = {10, 0, 0};
    root = create(objects, init);
    init.model = 3;
    init.material = 3;
    init.params = 0;
    init.parent = root;
    init.position = {20, 0, 0};
    part = create(objects, init);
    unrelated = create(objects, init);
    parts.out.set(root.slot(), std::vector<Handle>{part});
    parts.in.set(part.slot(), std::vector<Handle>{root});
    instances.set(root.slot(), std::vector<ScriptId>{9});
    properties.resize(6);
    processes.resize(6);
    properties[2].set(root.slot(), Value(10.0));
    state.insert(std::int64_t{3}, 5.0);
    players.rows.resize(4);
    plots.rows.push_back({1, 2, 0, 7, {-5, -4, 30, 40}, {}});
    perm::rebuild_plot_owners(plots);
    catalog_state.mechanical.values["measurements"] = {{"density", 1}, {"capacity", 6}};
    step::bind_metadata(objects_state, catalog_state, react_state, perm_state, 1000);
  }
  react::RunOutcome run(std::string_view body) {
    const auto source = "return {on={run=function(self,from,args) " + std::string(body) + " end}}";
    react::ScriptDeclaration declaration;
    declaration.id = next++;
    declaration.author = 2;
    declaration.kind = react::ScriptDeclaration::Kind::Instance;
    declaration.sets = {2};
    const auto loaded = react::load_script(vm, react::compile_script(source), declaration);
    require(loaded.status == react::RunOutcome::Status::Ok, loaded.error.c_str());
    effects = {};
    react::Invocation invocation;
    invocation.self = {root, declaration.id};
    invocation.sender = part;
    invocation.parts = {part};
    invocation.step = 20;
    invocation.state = &state;
    invocation.effects = &effects;
    return react::run_handler(vm, {declaration.id, "run", 2}, invocation);
  }
  void ok(std::string_view body) {
    const auto result = run(body);
    require(result.status == react::RunOutcome::Status::Ok, result.error.c_str());
  }
};
void effective_metadata_and_mass() {
  Fixture f;
  f.ok(R"(
    local o=self:object()
    assert(o.owner=='p:2' and o.model==1 and o.params==1 and o.material==4)
    assert(o.script==8 and o.scripts[1]==8 and o.scripts[2]==9)
    assert(o.position.X==10 and o.rotation[4]==1)
    assert(not pcall(function()o.scripts[1]=99 end))
    local a=self:asset()
    assert(a.variant_marker=='actual' and a.seats[1].position[2]==2)
    assert(a.capacity==nil and a.capacity_method==nil)
    assert(a.bounds==nil and a.size==nil and a.volume==nil and a.supports==nil)
    assert(a.assembly==nil)
    assert(not pcall(function()a.seats[1].position[2]=99 end))
    assert(self:material('custom.4','custom.1')==20)
    local m=self:measure_model('m:1',1,4)
    assert(m.mass_kg==100 and m.bounds.min.X==-1 and m.bounds.max.X==4)
    assert(m.volume_m3==7)
    assert(not pcall(function()m.volume_m3=99 end))
    assert(m.supports[1].height==1 and m.supports[1].area==4)
    assert(not pcall(function()m.supports[1].area=99 end))
    assert(m.capacity==nil and m.capacity_m3==nil)
    assert(m.materials[1].material==3 and m.materials[1].mass_kg==40)
    assert(m.materials[2].material==4 and m.materials[2].mass_kg==60)
    local live=self:measure_instance()
    assert(live.mass_kg==100 and live.bounds.min.X==9 and live.bounds.max.X==22)
    assert(live.volume_m3==7 and live.capacity_m3==0.25)
    assert(not pcall(function()live.volume_m3=99 end))
    assert(#self:plots()==1 and self:plots()[1]==1)
    assert(#self:plots('p:2')==1 and self:plots('p:2')[1]==1)
    assert(#self:plots('p:0')==0 and #self:plots('p:3')==0)
    local p=self:plot()
    assert(p.plot==1 and p.owner=='p:2' and p.bounds[1]==-5 and p.bounds[4]==40)
    assert(not pcall(function()p.bounds[1]=0 end))
  )");
  require(f.assets.rows.at("variant").manifest["capacity"] == 99.0 &&
              f.assets.rows.at("variant").manifest["seats"][0]["position"][1] == 2,
          "nested read-only metadata does not alter asset catalog");
  require(f.run("self:measure_model(1,0.5)").status == react::RunOutcome::Status::Error,
          "fractional parameter block cannot silently select another variant");
}
void installed_script_identity_is_readonly() {
  Fixture f;
  f.ok(R"(assert(self.script==100 and self:object().owner=='p:2')
    assert(self.context==nil and from~=self.handle)
    assert(not pcall(function()self.script=1 end))
    assert(self.script==100 and type(self.plot)=='function')
    assert(getmetatable(self)=='locked')
    assert(not pcall(function() self.plot=nil end))
    assert(not pcall(function() setmetatable(self,{}) end)))");
}
void plot_records_preserve_public_fields_and_readonly_rights() {
  Fixture f;
  f.plots.rows[1].bounds = {8, 50, 24, 82};
  f.plots.rows[1].preset = 3;
  f.plots.rows.push_back({2, 0, 0, 2, {0, -50, 72, 50}, {}, perm::PlotKind::Street});
  perm::rebuild_plot_owners(f.plots);
  const auto world = perm::Holder::script(90, {});
  require(
      bool(perm::create_root_grant(
          f.grants, f.players, {0, world, perm::Scope::all_land(), perm::Rights{24} << 8, 0, 0, 21},
          {world, 2, true}, 1)),
      "plot record world authority fixture failed");
  require(bool(perm::create_grant(f.grants, f.players, perm::Holder::player(2),
                                  {2, perm::Holder::player(3), perm::Scope::plot(1), 24, 0, 0, 21},
                                  {perm::Scope::plot(1), 2, perm::all_rights},
                                  {perm::Holder::player(3), 3, true}, 1)),
          "plot record ordinary authority fixture failed");
  const auto offered = perm::offer_plot(f.perm_state, perm::Holder::player(2), 1, 42, 0, 22, 1);
  require(bool(offered), "plot record public offer fixture failed");
  f.ok(R"(local p=self:plot(1)
    assert(p.kind=='lot' and p.owner=='p:2' and p.area_m2==512)
    assert(p.open and p.build_open and p.fronts=='street' and p.port==0 and p.port_x==0)
    assert(p.bounds[1]==8 and p.bounds[2]==50 and p.bounds[3]==24 and p.bounds[4]==82)
    assert(#p.rights==2 and p.rights[1].holder.script=='s:90' and p.rights[1].holder.on=='world')
    assert(p.rights[2].holder=='p:3')
    for _,right in p.rights do
      assert(#right.verbs==2 and right.verbs[1]=='transfer' and right.verbs[2]=='return')
    end
    assert(p.for_sale.offer=='offer:1' and p.for_sale.price==42 and p.for_sale.buyer==nil)
    assert(not pcall(function() p.owner='p:3' end))
    assert(not pcall(function() p.rights[1].holder.on='elsewhere' end))
    assert(not pcall(function() p.rights[1].verbs[1]='build' end))
    assert(not pcall(function() p.for_sale.price=1 end))
    assert(self:plot(2).kind=='street' and #self:plot(2).rights==0))");
  auto &offer = f.perm_state.offers.rows[offered.offer];
  offer.buyer = 3;
  f.ok("assert(self:plot(1).for_sale==nil)");
  offer.buyer = 0;
  offer.expires_step = 20;
  f.ok("assert(self:plot(1).for_sale==nil)");
}
void single_solid_measurement_has_no_assembly_metadata() {
  Fixture f;
  f.ok(R"(local m=self:measure_model(2)
    assert(m.mass_kg==40 and m.bounds.min.X==0 and m.bounds.max.Z==2)
    assert(m.volume_m3==2)
    assert(#m.materials==1 and m.materials[1].material==4))");
}
void retained_handles_grant_no_reads() {
  Fixture f;
  f.state.insert(std::string("saved"), f.unrelated);
  require(f.run("self:object(self.state.saved)").status == react::RunOutcome::Status::Error,
          "a retained handle does not authorize object metadata reads");
  require(f.run("self:measure_instance(self.state.saved)").status ==
              react::RunOutcome::Status::Error,
          "a retained handle does not authorize measured instance reads");
  require(f.run(R"(for key,value in self.state do
    if key=='saved' then self:measure_instance(value) end
  end)")
                  .status == react::RunOutcome::Status::Error,
          "iterating state does not acquire retained handles");
  f.state.replace(f.state.values.find(std::string("saved")), to_string(f.unrelated));
  require(f.run("self:object(self.state.saved)").status == react::RunOutcome::Status::Error,
          "a stored string is not promoted into a typed object reference");
}

void plot_objects_returns_only_independent_placed_things() {
  Fixture f;
  ObjectInit init;
  init.model = 1;
  init.plot = 1;
  init.flags = Body;
  create(f.objects, init);
  init.flags = Held;
  create(f.objects, init);
  init.flags = Static;
  init.plot = 2;
  create(f.objects, init);
  f.ok(R"(local things=self:plot_objects(1)
    assert(#things==2)
    local found_self=false
    for _,h in things do
      found_self=found_self or h==self.handle
      assert(self:object(h).owner=='p:2')
    end
    assert(found_self))");
  destroy(f.objects, f.unrelated);
  f.ok(R"(local things=self:plot_objects(1)
    assert(#things==1 and things[1]==self.handle))");
}
void plots_filter_by_current_owner() {
  Fixture f;
  perm::change_plot_owner(f.perm_state, 1, 0);
  f.ok(R"(assert(#self:plots('p:2')==0)
    assert(#self:plots('p:0')==1 and self:plots('p:0')[1]==1)
    assert(#self:plots()==1)
    assert(not pcall(function() self:plots('p:bad') end)))");
}
} // namespace
int main() {
  try {
    effective_metadata_and_mass();
    single_solid_measurement_has_no_assembly_metadata();
    installed_script_identity_is_readonly();
    plot_records_preserve_public_fields_and_readonly_rights();
    retained_handles_grant_no_reads();
    plot_objects_returns_only_independent_placed_things();
    plots_filter_by_current_owner();
    std::cout << "Actual readonly Luau metadata and variant mass checks passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
