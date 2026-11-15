#include "bindings/authority/authority.hpp"
#include "bindings/construction/construction.hpp"
#include "bindings/metadata/metadata.hpp"

#include <iostream>
#include <stdexcept>

using namespace amber;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct Fixture {
  Objects objects;
  Handle root, part, unrelated;
  catalog::Models models;
  catalog::ParameterBlocks parameters;
  catalog::Assets assets;
  react::Terms terms;
  react::Materials materials;
  react::Properties properties;
  react::Processes processes;
  react::MeterRows meters;
  Relation parts;
  SparseColumn<std::vector<ScriptId>> instances;
  perm::Plots plots;
  perm::Players players;
  perm::Grants grants;
  // This fixture verifies metadata behavior under sanitizers, not production timing.
  react::Vm vm{{4096, 50, 250, 64}};
  react::ScriptState state;
  react::DeferredEffects effects;
  ScriptId next = 100;
  bindings::ContextBinding context{authority(objects)};
  bindings::ObjectBinding object{identity(objects), transforms(objects), authority(objects),
                                 parents(objects),  objects.params,      models,
                                 instances};
  bindings::AssetReadBinding asset{identity(objects), objects.params, models, parameters, assets};
  bindings::MaterialReadBinding material{terms, materials};
  bindings::ModelMeasureBinding measure_model{terms, materials, models, parameters, assets, 1};
  bindings::InstanceMeasureBinding measure_instance{identity(objects),
                                                    transforms(objects),
                                                    objects.params,
                                                    parts,
                                                    terms,
                                                    materials,
                                                    models,
                                                    parameters,
                                                    assets,
                                                    1};
  bindings::MeterReadBinding meter{meters, terms, identity(objects)};
  bindings::PlotReadBinding plot{plots, authority(objects)};
  bindings::ConstructionBinding construction{models, parameters, terms, properties, processes, 64};
  bindings::RemovalBinding removal{identity(objects), terms, properties, processes, 64};
  bindings::PlotTransferBinding plot_transfer{plots, grants, players};
  Fixture() {
    terms.rows.resize(6);
    for (TermId i = 1; i < terms.rows.size(); ++i) {
      terms.rows[i].id = i;
      terms.rows[i].name = "custom." + std::to_string(i);
      terms.by_name[terms.rows[i].name] = i;
    }
    terms.rows[1].source = react::Source::Intrinsic;
    terms.rows[3].kind = terms.rows[4].kind = react::Kind::Material;
    terms.rows[5].kind = react::Kind::Event;
    materials.rows[3][1] = Value(10.0);
    materials.rows[4][1] = Value(20.0);
    models.rows.resize(4);
    for (ModelId id = 1; id < 4; ++id) {
      models.rows[id].id = id;
      models.rows[id].asset = "asset" + std::to_string(id);
      models.rows[id].material = id == 2 ? 4 : 3;
      auto &a = assets.rows[models.rows[id].asset];
      a.hash = models.rows[id].asset;
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
    state[std::int64_t{3}] = 5.0;
    meters[{root, part, 2, false}] = {2, 0, 3};
    players.rows.resize(4);
    plots.rows.push_back({1, 2, 0, 7, {-5, -4, 30, 40}, {}});
    react::register_binding(vm, "context", bindings::context_binding, &context);
    react::register_binding(vm, "object", bindings::object_binding, &object);
    react::register_binding(vm, "asset", bindings::asset_binding, &asset);
    react::register_binding(vm, "material", bindings::material_binding, &material);
    react::register_binding(vm, "measure_model", bindings::measure_model_binding, &measure_model);
    react::register_binding(vm, "measure_instance", bindings::measure_instance_binding,
                            &measure_instance);
    react::register_binding(vm, "meter", bindings::meter_binding, &meter);
    react::register_binding(vm, "plot", bindings::plot_binding, &plot);
    react::register_binding(vm, "create", bindings::create_binding, &construction);
    react::register_binding(vm, "replace", bindings::replace_binding, &construction);
    react::register_binding(vm, "remove", bindings::remove_binding, &removal);
    react::register_binding(vm, "transfer_plot", bindings::transfer_plot_binding, &plot_transfer);
  }
  react::RunOutcome run(std::string_view body, bool world_rule = false,
                        std::optional<Caller> caller = {}, bool direct = false) {
    const auto source = "return {on={run=function(self,from,args) " + std::string(body) + " end}}";
    react::ScriptDeclaration declaration;
    declaration.id = next++;
    declaration.author = 2;
    declaration.kind = world_rule ? react::ScriptDeclaration::Kind::Rule
                                  : react::ScriptDeclaration::Kind::Instance;
    declaration.approved = world_rule;
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
    invocation.action_caller = caller;
    invocation.action_body = root;
    invocation.action_target = part;
    if (direct)
      invocation.direct_body = react::BodyEndpoints{root, part, unrelated, 2, {}};
    return react::run_handler(vm, {declaration.id, "run", 2, 1, true, world_rule}, invocation);
  }
  void ok(std::string_view body, bool rule = false, std::optional<Caller> caller = {},
          bool direct = false) {
    const auto result = run(body, rule, caller, direct);
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
    assert(not pcall(function()a.seats[1].position[2]=99 end))
    assert(self:material('custom.4','custom.1')==20)
    local m=self:measure_model('m:1',1,4)
    assert(m.mass_kg==100 and m.bounds.min.X==-1 and m.bounds.max.X==4)
    assert(m.materials[1].material==3 and m.materials[1].mass_kg==40)
    assert(m.materials[2].material==4 and m.materials[2].mass_kg==60)
    local live=self:measure_instance()
    assert(live.mass_kg==100 and live.bounds.min.X==9 and live.bounds.max.X==22)
    assert(self:meter(self.handle,'custom.2',from)==5)
    local p=self:plot()
    assert(p.id==1 and p.owner=='p:2' and p.bounds.min[1]==-5 and p.bounds.max[2]==40)
    assert(not pcall(function()p.bounds.min[1]=0 end))
  )");
  require(f.assets.rows.at("variant").manifest["seats"][0]["position"][1] == 2,
          "nested read-only metadata does not alter asset catalog");
  require(f.run("self:measure_model(1,0.5)").status == react::RunOutcome::Status::Error,
          "fractional parameter block cannot silently select another variant");
}
void immediate_context_is_descriptive() {
  Fixture f;
  f.ok(R"(local c=self:context();assert(c.actor=='p:2' and c.body==self.handle
    and c.selected==from and c.held~=nil and c.owner=='p:2'))",
       false, PlayerId{2}, true);
  f.ok(R"(local c=self:context();assert(c.actor==nil and c.body==nil and c.held==nil
    and c.caller_script==71 and c.caller_object==from and c.selected==from))",
       false, ScriptHolder{71, f.part});
  f.ok(R"(local c=self:context();assert(c.actor==nil and c.selected==nil and c.held==nil))");
}
void single_solid_measurement_has_no_assembly_metadata() {
  Fixture f;
  f.ok(R"(local m=self:measure_model(2)
    assert(m.mass_kg==40 and m.bounds.min.X==0 and m.bounds.max.Z==2)
    assert(#m.materials==1 and m.materials[1].material==4))");
}
void retained_handles_reacquire_reads() {
  Fixture f;
  f.state[std::string("saved")] = f.unrelated;
  f.ok("assert(self:object(self.state.saved).model==3)");
  f.ok(R"(for key,value in self.state do
    if key=='saved' then assert(self:object(value).model==3) end
  end)");
  f.state[std::string("saved")] = to_string(f.unrelated);
  require(f.run("self:object(self.state.saved)").status == react::RunOutcome::Status::Error,
          "a stored string is not promoted into a typed object reference");
}
void structural_requests_and_permission_boundary() {
  Fixture f;
  const auto create = R"(local q=self:create('m:1',1,'custom.4',vector.create(2,3,4),
    {0,0,0,2},{terms={['custom.2']=7},state={[3]=4}});assert(q.queued))";
  require(f.run(create).status == react::RunOutcome::Status::Error && f.effects.structural.empty(),
          "ordinary scripts cannot gain create authority through binding presence");
  f.ok(create, true, PlayerId{2});
  require(f.effects.structural.size() == 1 && !f.effects.changed,
          "creation request does not claim commit before native preflight");
  const auto &request = f.effects.structural.front();
  require(request.binding.model == 1 && request.binding.params == 1 &&
              request.binding.material == 4 && request.rotation.w == 1 &&
              request.exchange->holder.object == f.root &&
              request.exchange->terms[0].before == Value(10.0) &&
              request.exchange->terms[0].after == Value(7.0) &&
              f.properties[2].get(f.root.slot())->number == 10 &&
              std::get<double>(f.state.at(std::int64_t{3})) == 5,
          "deferred exchange captures answers without mutating resources");
  f.ok("self:remove(from)");
  require(f.effects.structural.front().target == f.part,
          "ordinary scripts can request removal, commit still checks ordinary Destroy");
  require(f.run("self:create(1,0.5,4,vector.create(0,0,0))", true).status ==
                  react::RunOutcome::Status::Error &&
              f.effects.structural.empty(),
          "invalid geometry binding is rejected before queueing");
  f.ok("assert(not self:transfer_plot(1,'p:3').ok)", false, PlayerId{2}, true);
  require(f.plots.rows[1].owner == 2, "immediate body caller does not lend plot authority");
  const auto holder = perm::Holder::script(f.next, f.root);
  const auto grant = perm::create_grant(
      f.grants, f.players,
      {2, holder, perm::Scope::plot(1), perm::bit(perm::Verb::Transfer), 0, 0, 100},
      {perm::Scope::plot(1), 2, perm::bit(perm::Verb::Transfer), 1, false}, {holder, 2, true}, 0);
  require(bool(grant), "owner fills ordinary installed script plot slot");
  f.ok("assert(self:transfer_plot(1,'p:3').ok)");
  require(f.plots.rows[1].owner == 3 && !f.grants.rows[grant.grant].live && f.effects.changed,
          "authorized transfer commits ownership and revokes old plot grants together");
}
} // namespace
int main() {
  try {
    effective_metadata_and_mass();
    single_solid_measurement_has_no_assembly_metadata();
    immediate_context_is_descriptive();
    retained_handles_reacquire_reads();
    structural_requests_and_permission_boundary();
    std::cout
        << "Actual Luau metadata, variant mass, construction requests and plot transfer passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
