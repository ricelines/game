#include "space_fixture.hpp"

#include "bindings/commerce/commerce.hpp"
#include "bindings/construction/construction.hpp"
#include "bindings/installation/installation.hpp"
#include "bindings/metadata/metadata.hpp"
#include "bindings/revision/revision.hpp"
#include "bindings/structural/revision.hpp"
#include "bindings/structural/structural.hpp"
#include "runtime_fixture.hpp"
#include "scene_fixture.hpp"

using namespace amber;
using namespace amber::react;
using test_runtime::require;
namespace {
struct Fixture {
  test_runtime::Fixture f;
  lifecycle::Parts parts;
  Relation declared;
  lifecycle::Instances instances{f.objects, parts, declared};
  catalog::Assets assets;
  catalog::ParameterBlocks parameters;
  catalog::InstanceTexts instance_texts;
  bindings::GeometryAssets geometry;
  space::PhysicalRelations physical;
  std::vector<space::RelationChange> physical_changes;
  space::Space world;
  perm::OwnerLimits limits;
  perm::Players players;
  perm::Grants grants;
  perm::Plots plots;
  bindings::ConstructionBinding constructor{f.models,     parameters,  f.terms,
                                            f.properties, f.processes, 64};
  bindings::RemovalBinding remover{identity(f.objects), f.terms, f.properties, f.processes, 64};
  bindings::ResourceCommit resources{
      identity(f.objects), f.properties, f.processes, f.states, f.terms, f.vm};
  bindings::StructuralRights rights;
  bindings::GeometryInstallation installation;
  perm::Offers offers;
  perm::Trades trades;
  testscene::Projection projections;
  testscene::Lifecycle lifecycle;
  bindings::AssemblyPlacement placement;
  bindings::CreateOperation creation;
  bindings::RemoveOperation removal;
  bindings::ReplaceOperation replacement;
  bindings::ModelMeasureBinding model_measure{f.terms,    f.materials, f.models,
                                              parameters, assets,      10};
  bindings::InstanceMeasureBinding instance_measure{identity(f.objects),
                                                    transforms(f.objects),
                                                    f.objects.params,
                                                    declared,
                                                    f.terms,
                                                    f.materials,
                                                    f.models,
                                                    parameters,
                                                    assets,
                                                    10};
  bindings::Execution execution{f.vm, f.states, f.random, identity(f.objects), declared, 44};
  drive::StructuralOperations operations;
  space::GeometryId solid = 0;
  explicit Fixture(std::uint32_t capacity = 64)
      : f(250), world([&] {
          auto settings = testspace::settings();
          settings.max_bodies = capacity;
          return settings;
        }()),
        rights{identity(f.objects),
               authority(f.objects),
               transforms(f.objects),
               declared,
               limits,
               players,
               grants,
               plots,
               world,
               5},
        installation{
            world,    geometry, *f.projection, transforms(f.objects), space_flags(f.objects),
            {10, 11}, {.6f, 0}},
        projections(f.objects, f.models, parameters, assets, *f.projection, f.runtime,
                    f.subscriptions, declared, {30, 31}),
        lifecycle(f, world, instances, parts, declared, physical, physical_changes, projections,
                  players, grants, limits, offers, trades),
        placement{world,
                  identity(f.objects),
                  transforms(f.objects),
                  parents(f.objects),
                  space_flags(f.objects),
                  declared,
                  physical,
                  physical_changes},
        creation{instances,
                 {f.models, parameters, assets},
                 installation,
                 placement,
                 rights,
                 resources,
                 lifecycle.lifecycle},
        removal{instances, rights, resources, lifecycle.lifecycle},
        replacement{instances,
                    parts,
                    {f.models, parameters, assets},
                    installation,
                    transforms(f.objects),
                    parents(f.objects),
                    physical,
                    physical_changes,
                    rights,
                    resources,
                    lifecycle.lifecycle} {
    f.terms.rows.resize(32);
    for (auto [id, role] :
         {std::pair{30u, MeasurementRole::Mass}, std::pair{31u, MeasurementRole::Runtime}}) {
      auto &term = f.terms.rows[id];
      term.id = id;
      term.name = "mechanical.mass" + std::to_string(id);
      term.source = Source::Measured;
      term.measurement = role;
      term.dimensions = {0, 1, 0, 0, 0, 0, 0, 0};
      f.terms.by_name[term.name] = id;
    }
    f.projection->density = 10;
    for (TermId id : {10u, 11u}) {
      f.terms.rows[id].id = id;
      f.terms.rows[id].name = id == 10 ? "novel.density" : "novel.transmission";
      f.terms.rows[id].source = Source::Intrinsic;
      f.terms.by_name[f.terms.rows[id].name] = id;
    }
    f.materials.rows[8] = {{10, Value(1000.0)}, {11, Value(0.0)}};
    f.objects.owner[f.a.slot()] = 1;
    players.rows.resize(3);
    plots.rows.resize(2);
    plots.rows[1] = {1, 1, 0, 0, {-10, -10, 10, 10}, {}};
    testspace::Box root({1, 1, 1}), child({.2f, .2f, .4f});
    auto root_geometry = space::import_geometry(world, root.source());
    auto child_geometry = space::import_geometry(world, child.source());
    require(root_geometry.geometry && child_geometry.geometry,
            "real fixture collision assets import");
    solid = root_geometry.geometry;
    f.models.rows.resize(3);
    f.models.rows[1].material = 8;
    f.models.rows[1].asset = std::string(64, 'a');
    f.models.rows[1].parts = {{"leaf", 2}};
    f.models.rows[2].id = 2;
    f.models.rows[2].material = 8;
    f.models.rows[2].asset = std::string(64, 'b');
    catalog::Asset asset;
    asset.hash = f.models.rows[1].asset;
    asset.volume = 1;
    asset.minimum = root.bounds.min;
    asset.maximum = root.bounds.max;
    asset.manifest = {
        {"bindings", nlohmann::json::object()},
        {"assembly", {{"bounds", {{"min", {-0.5, -0.5, 0}}, {"max", {1.1, 0.5, 1}}}}}},
        {"parts", {{"leaf", {{"position", {1, 0, 0}}, {"rotation", {0, 0, 0, 1}}}}}}};
    assets.rows[asset.hash] = asset;
    geometry.rows[asset.hash] = {root_geometry.geometry, {}};
    asset.hash = f.models.rows[2].asset;
    asset.volume = .016;
    asset.minimum = child.bounds.min;
    asset.maximum = child.bounds.max;
    asset.manifest["parts"] = nlohmann::json::object();
    assets.rows[asset.hash] = asset;
    geometry.rows[asset.hash] = {child_geometry.geometry, {}};
    f.assets.rows[1].volume = 1;
    f.assets.rows[2].volume = .016;
    register_binding(f.vm, "create", bindings::create_binding, &constructor);
    register_binding(f.vm, "remove", bindings::remove_binding, &remover);
    register_binding(f.vm, "replace", bindings::replace_binding, &constructor);
    register_binding(f.vm, "measure_model", bindings::measure_model_binding, &model_measure);
    register_binding(f.vm, "measure_instance", bindings::measure_instance_binding,
                     &instance_measure);
    operations.rows[StructuralRequest::Kind::Create] = {bindings::create_instance, &creation};
    operations.rows[StructuralRequest::Kind::Destroy] = {bindings::remove_instance, &removal};
    operations.rows[StructuralRequest::Kind::ReplaceBinding] = {bindings::replace_instance,
                                                                &replacement};
    f.state(f.a, 1, 10);
  }
  ScriptId add(std::string_view source) {
    auto id = f.add(source);
    f.states[{f.a, id}][std::string("tokens")] = 10.0;
    return id;
  }
  drive::PlannedInvocation plan(ScriptId id, Handle target = {}, EventPayload payload = {}) {
    drive::PlannedInvocation plan;
    plan.handler = {id,   "novel.touch",
                    1,    1,
                    true, f.scripts.rows[id].declaration.kind == ScriptDeclaration::Kind::Rule};
    plan.input.receiver = f.a;
    plan.input.sender = target;
    plan.input.trigger = 7;
    plan.input.payload = std::move(payload);
    plan.input.action_caller = PlayerId{1};
    plan.input.action_body = f.a;
    plan.input.action_target = target;
    return plan;
  }
  double energy() { return read_state(f.properties, f.processes, f.a, 1, 0, 20).value.numeric(); }
  double tokens(ScriptId script) {
    return std::get<double>(f.states.at({f.a, script}).at(std::string("tokens")));
  }
  bindings::CatalogPublication publication(catalog::ModelTexts &texts) {
    return {f.models,
            f.scripts,
            f.terms,
            f.materials,
            texts,
            instance_texts,
            *f.projection,
            authority(f.objects),
            parents(f.objects),
            f.triggers,
            lifecycle.priorities,
            projections.metadata,
            {}};
  }
  void variants() {
    f.materials.rows[9] = {{10, Value(500.)}, {11, Value(.5)}};
    f.models.rows.resize(4);
    f.models.rows[3].id = 3;
    f.models.rows[3].material = 9;
    f.models.rows[3].asset = std::string(64, 'd');
    auto child = assets.rows.at(std::string(64, 'b'));
    child.hash = f.models.rows[3].asset;
    assets.rows[child.hash] = child;
    geometry.rows[child.hash] = geometry.rows.at(std::string(64, 'b'));
    testspace::Box expanded({2, 1, 1});
    auto asset = assets.rows.at(std::string(64, 'a'));
    asset.hash = std::string(64, 'c');
    asset.volume = 2;
    asset.maximum = expanded.bounds.max;
    asset.minimum = expanded.bounds.min;
    asset.manifest["assembly"]["bounds"] = {{"min", {-2.1, -.5, 0}}, {"max", {2.1, .5, 1}}};
    asset.manifest["parts"] = {{"leaf", {{"position", {2, 0, 0}}, {"rotation", {0, 0, 0, 1}}}},
                               {"tip", {{"position", {-2, 0, 0}}, {"rotation", {0, 0, 0, 1}}}}};
    assets.rows[asset.hash] = asset;
    geometry.rows[asset.hash] = {space::import_geometry(world, expanded.source()).geometry, {}};
    parameters.rows.push_back({1, 1, asset.hash, "{}", {{"leaf", 2}, {"tip", 3}}});
    asset.hash = std::string(64, 'e');
    asset.volume = 0;
    asset.manifest["parts"].erase("leaf");
    assets.rows[asset.hash] = asset;
    geometry.rows[asset.hash] = {0, {}};
    parameters.rows.push_back({2, 1, asset.hash, "{}", {{"tip", 3}}});
    f.assets.rows[3].volume = .016;
    f.assets.parameter_rows[1].volume = 2;
    f.assets.parameter_rows[2].volume = 0;
  }
};
constexpr auto create_source = R"(return {kind='rule',applies_to={material='novel.mat'},
  sets={'novel.heat'},on={['novel.touch']=function(self,target,args)
    local exchange={terms={['novel.heat']=self:get('novel.heat')-3},state={tokens=self.state.tokens-4}}
    self:create(1,nil,nil,vector.create(3,0,0),nil,exchange)
    if args=='twice' then self:create(1,nil,nil,vector.create(5,0,0),nil,exchange) end
    if args=='stale' then self:set('novel.heat',11) end
  end},tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=10}}}})";
constexpr auto remove_source = R"(return {kind='rule',applies_to={material='novel.mat'},
  sets={'novel.heat'},on={['novel.touch']=function(self,target)
    self:remove(target,{terms={['novel.heat']=self:get('novel.heat')+3},state={tokens=self.state.tokens+4}})
  end},tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=10}}}})";
constexpr auto replace_source = R"(return {kind='rule',applies_to={material='novel.mat'},
 on={['novel.touch']=function(self,target,args)
   local before=self:measure_instance(target).mass_kg
   local after=self:measure_model(1,args,8).mass_kg
   self:replace(target,1,args,8,{state={tokens=self.state.tokens-(after-before)}})
 end},tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=10}}}})";

void preflight_failures_do_not_allocate_or_debit() {
  Fixture f;
  auto script = f.add(create_source);
  auto plan = f.plan(script);
  auto slots = f.f.objects.model.size();
  plan.input.action_caller = PlayerId{2};
  auto denied = drive::execute(f.execution, plan, f.operations);
  require(denied.invocation.outcome.status == RunOutcome::Status::Error && denied.created.empty() &&
              f.f.objects.model.size() == slots && f.energy() == 10 && f.tokens(script) == 10,
          "approved construction rule cannot bypass immediate actor's Build permission or debit on "
          "denial");
  plan.input.action_caller = ScriptHolder{script, f.f.a};
  auto script_denied = drive::execute(f.execution, plan, f.operations);
  require(script_denied.created.empty() && f.energy() == 10,
          "installed script does not inherit its owner's land-build shortcut");
  plan = f.plan(script, {}, std::string("stale"));
  auto stale = drive::execute(f.execution, plan, f.operations);
  require(stale.invocation.outcome.status == RunOutcome::Status::Error && stale.created.empty() &&
              f.f.objects.model.size() == slots && f.energy() == 11 && f.tokens(script) == 10,
          "changed expected term rejects allocation and every resource exchange while retaining "
          "prior live script writes");
  Fixture limited(1);
  script = limited.add(create_source);
  auto capacity = drive::execute(limited.execution, limited.plan(script), limited.operations);
  require(capacity.invocation.outcome.status == RunOutcome::Status::Error &&
              capacity.created.empty() && limited.f.objects.model.size() == 4 &&
              limited.energy() == 10 && limited.tokens(script) == 10,
          "real native capacity preflight rejects a two-body assembly before allocation or debit");
}

void real_creation_and_duplicate_spend() {
  Fixture f;
  auto script = f.add(create_source);
  set_rate(f.f.properties, f.f.processes, f.f.terms.rows[1], f.f.a, 2, 0, 100, 0, 20, f.f.effects);
  auto completed =
      drive::execute(f.execution, f.plan(script, {}, std::string("twice")), f.operations);
  require(completed.invocation.outcome.status == RunOutcome::Status::Error &&
              completed.created.size() == 2 && completed.invocation.effects.changed &&
              f.energy() == 7 && f.tokens(script) == 6 &&
              read_state(f.f.properties, f.f.processes, f.f.a, 1, 20, 20).value.numeric() == 9,
          "one deferred creation succeeds, second cannot spend the same captured balance, and rate "
          "continues");
  auto root = completed.created[0], leaf = completed.created[1];
  std::vector<const record::Event *> births;
  for (const auto &event : completed.invocation.structural_records)
    if (event.kind == record::Kind::Create)
      births.push_back(&event);
  require(births.size() == 2 && births[0]->things == std::vector<Handle>{root} &&
              births[1]->things == std::vector<Handle>{leaf} &&
              births[0]->detail.at("model") == "m:1" && births[1]->detail.at("model") == "m:2" &&
              births[1]->detail.at("maker") == "p:1" && births[1]->detail.at("owner") == "p:1",
          "each ordinary constructed handle has its own typed model/player birth attribution");
  require(f.parts.get(root.slot())->at(0) == lifecycle::NamedPart{"leaf", leaf} &&
              f.f.objects.owner[root.slot()] == 1 && f.f.objects.maker[root.slot()] == 1 &&
              f.f.objects.parent[leaf.slot()] == root && !(f.f.objects.flags[root.slot()] & Body),
          "actual lifecycle creation assigns metadata and complete named parts without forged body "
          "flags");
  auto root_hit = space::raycast(f.world, identity(f.f.objects), {3, -2, .5f}, {3, 2, .5f}, {},
                                 space::Deadline::max());
  auto leaf_hit = space::raycast(f.world, identity(f.f.objects), {4, -2, .2f}, {4, 2, .2f}, {},
                                 space::Deadline::max());
  require(root_hit.hit && root_hit.hit->object == root && leaf_hit.hit &&
              leaf_hit.hit->object == leaf,
          "success installs actual independent static root and named-part Jolt geometry");
}

void measured_single_solid_creation() {
  Fixture f;
  f.f.models.rows[1].parts.clear();
  auto &manifest = f.assets.rows.at(std::string(64, 'a')).manifest;
  manifest.erase("assembly");
  manifest.erase("parts");
  const auto script = f.add(R"(return {kind='rule',applies_to={material='novel.mat'},
   on={['novel.touch']=function(self)
    local m=self:measure_model(1)
    assert(m.mass_kg==1000 and m.bounds.min.X==-0.5 and m.bounds.max.Z==1)
    self:create(1,nil,nil,vector.create(3,0,0))
  end},tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=10}}}})");
  const auto completed = drive::execute(f.execution, f.plan(script), f.operations);
  require(completed.invocation.outcome.status == RunOutcome::Status::Ok,
          completed.invocation.outcome.error.c_str());
  require(completed.created.size() == 1,
          "single-solid measured construction creates one root without assembly metadata");
  const auto hit = space::raycast(f.world, identity(f.f.objects), {3, -2, .5f}, {3, 2, .5f}, {},
                                  space::Deadline::max());
  require(hit.hit && hit.hit->object == completed.created.front(),
          "measured single-solid creation installs the actual Jolt collider");
}

void removal_credits_before_cleanup_and_keeps_ordinary_occupants() {
  Fixture f;
  auto creator = f.add(create_source);
  auto made = drive::execute(f.execution, f.plan(creator), f.operations);
  require(made.created.size() == 2, "fixture assembly created");
  auto root = made.created[0], leaf = made.created[1];
  auto occupant = f.f.b;
  require(space::add(f.world, identity(f.f.objects), transforms(f.f.objects),
                     {occupant, f.solid, space::Motion::Dynamic, {.6f, 0, 0}, 20, false, false, {}})
                  .status == space::Status::Ok,
          "actual independent occupant body installs");
  std::vector<space::RelationChange> changes;
  require(space::attach(f.world, identity(f.f.objects), transforms(f.f.objects),
                        parents(f.f.objects), space_flags(f.f.objects), f.physical.contains,
                        f.physical.supports, f.physical.attached_to,
                        {occupant, root, {{0, 0, 1.5f}, {}}, false}, changes)
                  .status == space::Status::Ok,
          "ordinary external occupant attaches without becoming a declared part");
  auto remover = f.add(remove_source);
  // A real authored relation removal must leave a watch draft for its
  // surviving endpoint; no callback-only notification sink is involved.
  set_relation(f.f.relations.at(3), 3, root, f.f.a, true, f.f.effects);
  auto removed = drive::execute(f.execution, f.plan(remover, root), f.operations);
  require(removed.invocation.outcome.status == RunOutcome::Status::Ok &&
              removed.invocation.effects.changed && f.energy() == 10 && f.tokens(remover) == 14 &&
              !identity(f.f.objects).valid(root) && !identity(f.f.objects).valid(leaf) &&
              identity(f.f.objects).valid(occupant) &&
              space::bounds(f.world, identity(f.f.objects), occupant) &&
              !f.f.objects.parent[occupant.slot()] &&
              !space::bounds(f.world, identity(f.f.objects), root),
          "credit precedes cleanup; declared assembly retires while the ordinary attached occupant "
          "survives detached");
  record::Journal journal(8, {});
  journal.restore_next_id(10);
  step::WorkQueues queues;
  step::TimerWheel timers;
  auto cause = bindings::publish_invocation(journal, queues, timers, std::move(removed.invocation),
                                            {9, 5, {1}, 0, false}, 1000);
  bool saw_notification = false;
  while (auto queued = step::pop_foreground(queues))
    if (auto *event = std::get_if<step::WatchWork>(&queued->work)) {
      require(
          event->origin == cause && event->origin.cause == 10 && event->origin.root == 5,
          "retirement effects publish with completed handler cause rather than an empty origin");
      saw_notification = true;
    }
  require(saw_notification, "ordinary cleanup notification survives structural completion");
  Fixture self;
  remover = self.add(remove_source);
  auto own = self.f.a;
  auto retired = drive::execute(self.execution, self.plan(remover, own), self.operations);
  require(retired.invocation.outcome.status == RunOutcome::Status::Ok &&
              !identity(self.f.objects).valid(own) && !self.f.states.contains({own, remover}) &&
              !self.f.properties[1].get(own.slot()),
          "retiring the invoking holder applies its resource credit before state cleanup and slot "
          "release");
}

void actual_vm_approval_and_activation_gate() {
  Fixture f;
  auto script = f.add(create_source);
  set_activation(f.f.vm, script, false, true);
  auto unapproved = drive::execute(f.execution, f.plan(script), f.operations);
  require(unapproved.invocation.outcome.status == RunOutcome::Status::Missing &&
              unapproved.created.empty() && unapproved.invocation.effects.structural.empty(),
          "unapproved cached rule cannot execute through an obsolete handler reference");
  auto remover = f.add(remove_source);
  set_activation(f.f.vm, remover, false, true);
  auto stale_destroy = drive::execute(f.execution, f.plan(remover, f.f.a), f.operations);
  require(stale_destroy.invocation.outcome.status == RunOutcome::Status::Missing &&
              identity(f.f.objects).valid(f.f.a) &&
              stale_destroy.invocation.effects.structural.empty(),
          "revoked world-rule approval cannot retain the own-subject destruction exception");
  set_activation(f.f.vm, script, true, false);
  auto inactive = drive::execute(f.execution, f.plan(script), f.operations);
  require(inactive.invocation.outcome.status == RunOutcome::Status::Missing &&
              inactive.created.empty(),
          "inactive cached rule cannot execute a stale handler reference");
  auto ordinary = f.add(R"(return {kind='model',on={['novel.touch']=function(self)
    self:create(1,nil,nil,vector.create(3,0,0))end}})");
  auto model = drive::execute(f.execution, f.plan(ordinary), f.operations);
  require(model.invocation.outcome.status == RunOutcome::Status::Error && model.created.empty() &&
              f.energy() == 10,
          "ordinary model scripts cannot invoke the approved-world-rule constructor primitive");
}

void replacement_preflights_and_preserves_actual_state() {
  Fixture f;
  const auto creator = f.add(create_source);
  const auto made = drive::execute(f.execution, f.plan(creator), f.operations);
  require(made.created.size() == 2, "replacement fixture actual construction");
  const auto root = made.created[0], leaf = made.created[1];
  require(space::body_motion(f.world, identity(f.f.objects), leaf)->motion ==
                  space::Motion::Kinematic &&
              f.physical.attached_to.out.get(leaf.slot())->front() == root,
          "native creation installs independent real kinematic part attachment");
  f.variants();
  const auto editor = f.add(replace_source);
  f.f.states[{f.f.a, editor}][std::string("tokens")] = 2000.;
  f.f.states[{root, editor}][std::string("kept")] = 42.;
  f.f.states[{leaf, editor}][std::string("kept")] = 55.;
  f.limits.set(leaf.slot(), perm::inherited_rights & ~perm::bit(perm::Verb::Edit));
  const auto denied = drive::execute(f.execution, f.plan(editor, root, Value(1.)), f.operations);
  require(denied.invocation.outcome.status == RunOutcome::Status::Error &&
              f.f.objects.params[root.slot()] == 0 && f.tokens(editor) == 2000 &&
              identity(f.f.objects).valid(leaf) && denied.created.empty(),
          "part inherited Edit ceiling rejects all resource and geometry changes");
  f.limits.erase(leaf.slot());
  permission_flags(f.f.objects).set(root.slot(), Escrow);
  const auto escrow = drive::execute(f.execution, f.plan(editor, root, Value(1.)), f.operations);
  require(escrow.invocation.outcome.status == RunOutcome::Status::Error &&
              f.tokens(editor) == 2000 && f.f.objects.params[root.slot()] == 0,
          "replacement cannot change active escrow membership");
  permission_flags(f.f.objects).set(root.slot(), 0);
  // Edit is the specified object right; it does not silently require a second Build grant.
  f.plots.rows[1].owner = 2;
  const auto applied = drive::execute(f.execution, f.plan(editor, root, Value(1.)), f.operations);
  require(applied.invocation.outcome.status == RunOutcome::Status::Ok,
          applied.invocation.outcome.error.c_str());
  require(
      applied.invocation.outcome.status == RunOutcome::Status::Ok &&
          applied.invocation.effects.changed && applied.created.size() == 1 &&
          f.f.objects.params[root.slot()] == 1 && f.tokens(editor) == 992,
      "authored replacement charges complete measured mixed-material delta after Edit preflight");
  const auto tip = applied.created[0];
  require(f.parts.get(root.slot())->at(0) == lifecycle::NamedPart{"leaf", leaf} &&
              f.f.objects.material[tip.slot()] == 9 &&
              std::get<double>(f.f.states.at({root, editor}).at(std::string("kept"))) == 42 &&
              std::get<double>(f.f.states.at({leaf, editor}).at(std::string("kept"))) == 55 &&
              f.projections.replicas.rows.contains(tip.bits) &&
              f.projections.replicas.rows.contains(leaf.bits),
          "matching parts keep handles/state and all actual replicas update");
  auto hit = space::raycast(f.world, identity(f.f.objects), {5, -2, .2f}, {5, 2, .2f}, {},
                            space::Deadline::max());
  require(hit.hit && hit.hit->object == leaf, "replacement moves real retained collision geometry");
  f.f.states[{tip, editor}][std::string("kept")] = 77.;
  const auto removed = drive::execute(f.execution, f.plan(editor, root, Value(2.)), f.operations);
  require(removed.invocation.outcome.status == RunOutcome::Status::Ok && removed.created.empty() &&
              f.tokens(editor) == 3008 && !identity(f.f.objects).valid(leaf) &&
              !f.f.states.contains({leaf, editor}) && identity(f.f.objects).valid(tip) &&
              std::get<double>(f.f.states.at({tip, editor}).at(std::string("kept"))) == 77 &&
              space::is_logical_root(f.world, identity(f.f.objects), root),
          "topology removal refunds actual disjoint mass, retires removed state and supports empty "
          "root");
  const auto starts =
      std::count_if(applied.invocation.effects.ordered.begin(),
                    applied.invocation.effects.ordered.end(), [](const auto &effect) {
                      const auto *event = std::get_if<step::EventWork>(&effect);
                      return event && event->event == 6;
                    });
  require(starts == 1 && std::none_of(removed.invocation.effects.ordered.begin(),
                                      removed.invocation.effects.ordered.end(),
                                      [](const auto &effect) {
                                        return std::holds_alternative<step::EventWork>(effect);
                                      }),
          "only the genuinely new part gets start; retained rows never restart");
}

void member_replacement_does_not_widen_authority() {
  Fixture f;
  const auto creator = f.add(create_source);
  const auto made = drive::execute(f.execution, f.plan(creator), f.operations);
  const auto outer = made.created[0], member = made.created[1];
  const std::array children{member};
  require(space::assemble(f.world, identity(f.f.objects), transforms(f.f.objects), outer, children,
                          space::Motion::Dynamic)
                  .status == space::Status::Ok,
          "member edit fixture promotes actual physical assembly");
  f.f.objects.owner[outer.slot()] = 2;
  f.limits.set(outer.slot(), perm::inherited_rights & ~perm::bit(perm::Verb::Edit));
  f.variants();
  const auto editor = f.add(replace_source);
  f.f.states[{f.f.a, editor}][std::string("tokens")] = 5000.;
  const auto applied = drive::execute(f.execution, f.plan(editor, member, Value(1.)), f.operations);
  require(applied.invocation.outcome.status == RunOutcome::Status::Ok,
          applied.invocation.outcome.error.c_str());
  require(applied.created.size() == 2 && f.f.objects.owner[outer.slot()] == 2 &&
              f.f.objects.model[outer.slot()] == 1 && f.f.objects.params[outer.slot()] == 0 &&
              f.f.objects.model[member.slot()] == 1 && f.f.objects.params[member.slot()] == 1 &&
              f.projections.replicas.rows.contains(member.bits) &&
              space::assembly_root(f.world, identity(f.f.objects), member) == outer &&
              space::body_motion(f.world, identity(f.f.objects), outer)->motion ==
                  space::Motion::Dynamic,
          "wider mechanical rebuild preserves outer binding/authority while editing selected "
          "member only");
}

void material_refresh_updates_real_mass_and_optics_without_reconstruction() {
  Fixture f;
  const auto creator = f.add(create_source);
  const auto made = drive::execute(f.execution, f.plan(creator), f.operations);
  const auto root = made.created[0], child = made.created[1];
  const std::array descendants{child};
  const auto ids = identity(f.f.objects);
  require(space::assemble(f.world, ids, transforms(f.f.objects), root, descendants,
                          space::Motion::Dynamic)
                  .status == space::Status::Ok,
          "material refresh physical aggregate fixture");
  require(space::impulse(f.world, ids, root, {1016, 0, 0}, {3, 0, .5f}).status == space::Status::Ok,
          "initial measured assembly impulse");
  std::vector<space::MotionRow> before;
  space::capture_motion(f.world, ids, before);
  require(before.size() == 1 && std::abs(before[0].linear_velocity_m_s.x - 1) < .001,
          "initial impulse uses actual 1016kg disjoint assembly mass");
  const auto available = space::capacity(f.world).native_available;
  f.f.materials.rows[8][10] = Value(500.);
  f.f.materials.rows[8][11] = Value(1.);
  const auto values = f.f.values();
  bindings::MaterialRefresh refresh{f.world,  f.geometry, f.f.models, f.parameters,
                                    f.assets, values,     {10, 11},   {.5f, 0}};
  const std::array<TermId, 1> changed{8};
  const auto plan = bindings::prepare_material_refresh(refresh, changed, 0, 20);
  require(std::ranges::find(plan.affected, root) != plan.affected.end() &&
              std::ranges::find(plan.affected, child) != plan.affected.end() &&
              plan.physical.size() == 2,
          "rare material refresh includes actual root and declared member");
  bindings::apply_material_refresh(f.world, ids, plan);
  std::vector<space::MotionRow> after;
  space::capture_motion(f.world, ids, after);
  require(after.size() == 1 && after[0].object == root &&
              after[0].linear_velocity_m_s == before[0].linear_velocity_m_s &&
              after[0].angular_velocity_rad_s == before[0].angular_velocity_rad_s &&
              space::capacity(f.world).native_available == available &&
              f.f.objects.parent[child.slot()] == root &&
              space::assembly_root(f.world, ids, child) == root,
          "material edit preserves ordinary attachment, aggregate shape slots and exact velocity");
  require(space::impulse(f.world, ids, root, {508, 0, 0}, {3, 0, .5f}).status == space::Status::Ok,
          "updated measured assembly impulse");
  space::capture_motion(f.world, ids, after);
  require(after.size() == 1 && std::abs(after[0].linear_velocity_m_s.x - 2) < .001,
          "native mass and inertia use new 508kg total rather than stale birth density");
  const auto visible =
      space::visibility(f.world, ids, {3, -2, .5f}, {3, 2, .5f}, {}, .5f, space::Deadline::max());
  require(visible.clear && visible.transmission == 1 &&
              std::ranges::find(visible.through, root) != visible.through.end(),
          "aggregate root query shape immediately uses new material optical truth");
  std::vector<space::RelationChange> changes;
  require(space::custody(f.world, ids, root, {}, parents(f.f.objects), space_flags(f.f.objects),
                         f.physical, changes, descendants)
                  .status == space::Status::Ok,
          "material refresh hidden assembly fixture");
  f.f.materials.rows[8][10] = Value(250.);
  const auto hidden = bindings::prepare_material_refresh(refresh, changed, 0, 20);
  bindings::apply_material_refresh(f.world, ids, hidden);
  require(hidden.affected.size() == plan.affected.size() && hidden.physical.empty() &&
              !space::bounds(f.world, ids, root) && (f.f.objects.flags[root.slot()] & Held),
          "custody material refresh invalidates mass answers without recreating native geometry");
}

void replacement_capacity_and_full_pool_creation() {
  Fixture f(2);
  const auto creator = f.add(create_source);
  const auto made = drive::execute(f.execution, f.plan(creator), f.operations);
  require(made.created.size() == 2 && space::capacity(f.world).native_available == 0,
          "multipart creation plus real attachments fits exact native pool without spare body");
  const auto root = made.created[0], leaf = made.created[1];
  f.variants();
  const auto editor = f.add(replace_source);
  f.f.states[{f.f.a, editor}][std::string("tokens")] = 2000.;
  const auto slots = f.f.objects.model.size();
  const auto failed = drive::execute(f.execution, f.plan(editor, root, Value(1.)), f.operations);
  require(failed.invocation.outcome.status == RunOutcome::Status::Error &&
              f.f.objects.model.size() == slots && f.f.objects.params[root.slot()] == 0 &&
              f.tokens(editor) == 2000 && identity(f.f.objects).valid(leaf) &&
              space::bounds(f.world, identity(f.f.objects), root) &&
              space::bounds(f.world, identity(f.f.objects), leaf),
          "replacement capacity failure precedes object allocation, resource debit and native "
          "extraction");
}
void structural_rebind_notifies_actual_watched_answers() {
  Fixture f;
  f.variants();
  f.f.terms.rows.resize(32);
  for (auto [id, name, role] : {std::tuple{12u, "novel.mass", MeasurementRole::Mass},
                                std::tuple{13u, "novel.volume", MeasurementRole::Volume},
                                std::tuple{14u, "novel.capacity", MeasurementRole::Baked}}) {
    auto &term = f.f.terms.rows[id];
    term.id = id;
    term.name = name;
    term.source = Source::Measured;
    term.measurement = role;
    f.f.terms.by_name[name] = id;
  }
  f.f.projection->density = 10;
  f.f.models.rows.resize(6);
  f.f.models.rows[1].author = 1;
  f.f.models.rows[1].source_hash = std::string(64, '1');
  f.f.models.rows[1].canonical_params = "{}";
  f.f.models.rows[4] = f.f.models.rows[1];
  f.f.models.rows[4].id = 4;
  f.f.models.rows[4].parts.clear();
  for (auto actor : {f.f.a, f.f.b, f.f.c})
    f.f.objects.model[actor.slot()] = 4;
  auto &old = f.assets.rows.at(std::string(64, 'a'));
  old.source_hash = std::string(64, '1');
  old.canonical_params = "{}";
  old.manifest["parameter_schema"] = nlohmann::json::object();
  auto &variant = f.assets.rows.at(std::string(64, 'c'));
  variant.source_hash = old.source_hash;
  variant.canonical_params = "{}";
  auto next = old;
  next.hash = std::string(64, 'f');
  next.source_hash = std::string(64, '2');
  next.volume = 3;
  testspace::Box next_shape({3, 1, 1});
  next.minimum = next_shape.bounds.min;
  next.maximum = next_shape.bounds.max;
  f.assets.rows[next.hash] = next;
  f.geometry.rows[next.hash] = {space::import_geometry(f.world, next_shape.source()).geometry, {}};
  f.f.models.rows[5] = f.f.models.rows[1];
  f.f.models.rows[5].id = 5;
  f.f.models.rows[5].asset = next.hash;
  f.f.models.rows[5].source_hash = next.source_hash;
  const catalog::MeasurementBindings roles{13, 14};
  for (ModelId id : {1u, 2u, 3u, 4u, 5u})
    catalog::project_measurements(f.f.assets, id, 0, f.assets.rows.at(f.f.models.rows[id].asset),
                                  roles);
  for (const auto &block : f.parameters.rows)
    if (block.id)
      catalog::project_measurements(f.f.assets, block.model, block.id,
                                    f.assets.rows.at(block.asset), roles);
  bindings::ModelRevisionQueue queue{f.f.models, f.f.properties, f.f.processes, f.f.terms, 64};
  register_binding(f.f.vm, "revise_model", bindings::revise_model_binding, &queue);
  const auto watch = f.add(R"(return {kind='model',
    watches={'novel.mass','novel.volume','novel.capacity','novel.transmission'},on={
    ['novel.mass']=function(self) self.state.mass_notices=(self.state.mass_notices or 0)+1
      self.state.observed_mass=self:get('novel.mass') end,
    ['novel.volume']=function(self) self.state.volume_notices=(self.state.volume_notices or 0)+1 end,
    ['novel.capacity']=function(self) self.state.capacity_notices=(self.state.capacity_notices or 0)+1 end,
    ['novel.transmission']=function(self) self.state.material_notices=(self.state.material_notices or 0)+1 end}})");
  f.f.models.rows[1].script = watch;
  install::project_model(f.f.triggers, f.f.models, f.f.scripts, f.f.terms, 1);
  bindings::InstallReactions reactions{f.f.models,        f.f.scripts,     f.f.installed,
                                       f.f.rules,         *f.f.projection, f.f.triggers,
                                       f.f.subscriptions, f.f.links,       f.f.allocations};
  const auto placed = [&](std::uint32_t params, Vec3 position) {
    ObjectInit init;
    init.owner = init.maker = 1;
    init.flags = Static;
    init.position = position;
    const auto created =
        f.instances.create(lifecycle::expand(f.f.models, f.parameters, f.assets, 1, params), init);
    for (auto h : created.objects) {
      const auto slot = h.slot();
      const auto asset = catalog::resolve_asset(f.f.models, f.parameters, f.f.objects.model[slot],
                                                f.f.objects.params[slot]);
      const auto body =
          bindings::describe_physics(f.geometry, f.assets.rows.at(asset.hash), *f.f.projection,
                                     {f.f.objects.model[slot], f.f.objects.params[slot], 8, 0, h},
                                     Static, {10, 11}, {.6f, 0}, 0, 20);
      bindings::install_physics(f.world, identity(f.f.objects), transforms(f.f.objects),
                                space_flags(f.f.objects), h, body);
    }
    bindings::install_declared_attachments(f.placement, created.objects);
    DeferredEffects initial;
    bindings::install_reactions(reactions, created.objects, {}, initial);
    require(initial.ordered.empty(), "installation quietly saves the actual measured baseline");
    return created.root;
  };
  const auto first = placed(0, {2, 0, 0}), second = placed(1, {-2, 0, 0});
  const auto along = f.add(R"(return {kind='instance',
    watches={{term='novel.mass',along='novel.link'}},on={['novel.mass']=function(self,source)
      self.state.notices=(self.state.notices or 0)+1
      self.state.mass=self:read(source,'novel.mass') end}})");
  const std::array<ScriptId, 1> installed{along};
  install::set_instances(f.f.installed, f.f.c, installed, f.f.scripts);
  set_relation(f.f.relations.at(3), 3, f.f.c, first, true, f.f.effects);
  DeferredEffects initial;
  const std::array<Handle, 1> observer{f.f.c};
  bindings::install_reactions(reactions, observer, {}, initial);
  const auto editor = f.add(R"(return {kind='rule',applies_to={material='novel.mat'},
    on={['novel.touch']=function(self,target,args)
      self:replace(target,1,args,8,{state={tokens=self.state.tokens-1}})
    end},tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=10}}}})");
  record::Journal journal(8, {});
  step::WorkQueues queues;
  step::TimerWheel timers;
  const auto apply = [&](drive::ExecutionResult completed) {
    require(completed.invocation.outcome.status == RunOutcome::Status::Ok,
            completed.invocation.outcome.error.c_str());
    const auto origin = bindings::publish_invocation(journal, queues, timers,
                                                     std::move(completed.invocation), {}, 1000);
    while (auto next = step::pop_foreground(queues)) {
      std::vector<drive::PlannedInvocation> invocations;
      record::Origin work_origin;
      if (const auto *work = std::get_if<step::WatchWork>(&next->work)) {
        work_origin = work->origin;
        auto plan = drive::plan_watch(f.f.subscriptions, f.f.links, *f.f.projection,
                                      f.f.allocations, f.f.triggers, f.f.scripts, *work, 0, 20);
        invocations = std::move(plan.invocations);
      } else {
        const auto &event = std::get<step::EventWork>(next->work);
        work_origin = event.origin;
        invocations = drive::plan_event(f.f.triggers, *f.f.projection, event, 0, 20);
      }
      require(work_origin.root == origin.root,
              "structural notifications keep their final published causal root");
      for (auto &invocation : invocations) {
        auto ran = drive::execute(f.execution, invocation, f.operations);
        require(ran.invocation.outcome.status == RunOutcome::Status::Ok,
                ran.invocation.outcome.error.c_str());
        bindings::publish_invocation(journal, queues, timers, std::move(ran.invocation),
                                     work_origin, 1000);
      }
    }
  };
  for (auto [object, params] :
       {std::pair{first, 1u}, std::pair{first, 0u}, std::pair{second, 0u}, std::pair{second, 1u}})
    apply(drive::execute(f.execution, f.plan(editor, object, Value(double(params))), f.operations));
  catalog::ModelTexts texts;
  auto publication = f.publication(texts);
  bindings::ModelRevisionRead read{
      identity(f.f.objects), f.f.objects.params, f.parts, f.f.models, f.parameters, f.assets,
      f.f.materials,         f.f.terms,          10};
  bindings::ModelRevisionOperation revision{read,
                                            f.f.models,
                                            f.parameters,
                                            texts,
                                            f.f.model_settings,
                                            f.f.assets,
                                            roles,
                                            bindings::replacement_edit(f.replacement),
                                            publication};
  f.operations.rows[StructuralRequest::Kind::ReviseModel] = {bindings::revise_model, &revision};
  const auto reviser = f.add(R"(return {kind='rule',applies_to={material='novel.mat'},
    on={['novel.touch']=function(self)
      self:revise_model(1,5,string.rep('1',64),{state={tokens=self.state.tokens-1}})
    end},tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=10}}}})");
  apply(drive::execute(f.execution, f.plan(reviser), f.operations));
  for (auto object : {first, second}) {
    const auto &state = f.f.states.at({object, watch});
    require(std::get<double>(state.at(std::string("mass_notices"))) == 3 &&
                std::get<double>(state.at(std::string("observed_mass"))) == 3000 &&
                std::get<double>(state.at(std::string("volume_notices"))) == 3 &&
                !state.contains(std::string("capacity_notices")) &&
                !state.contains(std::string("material_notices")),
            "actual default model watch sees both parameter edits and same-binding definition "
            "edit, while unchanged baked/intrinsic answers stay silent");
  }
  const auto &observed = f.f.states.at({f.f.c, along});
  require(std::get<double>(observed.at(std::string("notices"))) == 3 &&
              std::get<double>(observed.at(std::string("mass"))) == 3000,
          "foreign along observer is dispatched on its own receiver with acquired source");
  const auto material_editor = f.add(R"(return {kind='rule',applies_to={material='novel.mat'},
    on={['novel.touch']=function(self,target)
      self:replace(target,1,0,9,{state={tokens=self.state.tokens-1}})
    end},tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=10}}}})");
  apply(drive::execute(f.execution, f.plan(material_editor, first), f.operations));
  const auto &updated = f.f.states.at({first, watch});
  require(std::get<double>(updated.at(std::string("mass_notices"))) == 4 &&
              std::get<double>(updated.at(std::string("material_notices"))) == 1 &&
              std::get<double>(updated.at(std::string("volume_notices"))) == 3,
          "same generic publication covers intrinsic material changes without duplicate volume");
  apply(drive::execute(f.execution, f.plan(material_editor, first), f.operations));
  require(std::get<double>(updated.at(std::string("mass_notices"))) == 4 &&
              std::get<double>(updated.at(std::string("material_notices"))) == 1,
          "identical reconfiguration does not fabricate another watched change");
}

// Real quote cancellation is a prerequisite, not a callback between extracted
// assemblies: it can restore the grasp that determines the new aggregate demand.
void model_definition_quoted_topology(bool remove_part, bool capacity_failure = false,
                                      bool hidden = false) {
  namespace cm = bindings::commerce;
  Fixture f(capacity_failure ? 2 : 32);
  f.f.models.rows.resize(5);
  f.f.models.rows[1].author = 1;
  f.f.models.rows[1].source_hash = std::string(64, '1');
  f.f.models.rows[1].canonical_params = "{}";
  if (!remove_part)
    f.f.models.rows[1].parts.clear();
  f.f.models.rows[4] = f.f.models.rows[1];
  f.f.models.rows[4].id = 4;
  f.f.models.rows[4].parts.clear();
  for (auto actor : {f.f.a, f.f.b, f.f.c})
    f.f.objects.model[actor.slot()] = 4;
  f.f.objects.flags[f.f.a.slot()] |= Body;
  auto &old_asset = f.assets.rows.at(std::string(64, 'a'));
  old_asset.source_hash = std::string(64, '1');
  old_asset.canonical_params = "{}";
  old_asset.manifest["parameter_schema"] = nlohmann::json::object();
  if (!remove_part)
    old_asset.manifest["parts"] = nlohmann::json::object();
  auto next_asset = old_asset;
  next_asset.hash = std::string(64, 'f');
  next_asset.source_hash = std::string(64, '2');
  next_asset.volume = 2;
  next_asset.manifest["parts"] =
      remove_part ? nlohmann::json::object()
                  : nlohmann::json{{"leaf", {{"position", {1, 0, 0}}, {"rotation", {0, 0, 0, 1}}}}};
  f.assets.rows[next_asset.hash] = next_asset;
  f.geometry.rows[next_asset.hash] = f.geometry.rows.at(old_asset.hash);
  f.f.models.rows[3] = f.f.models.rows[1];
  f.f.models.rows[3].id = 3;
  f.f.models.rows[3].asset = next_asset.hash;
  f.f.models.rows[3].source_hash = next_asset.source_hash;
  f.f.models.rows[3].parts =
      remove_part ? std::map<std::string, ModelId>{} : std::map<std::string, ModelId>{{"leaf", 2}};
  ObjectInit init;
  init.owner = init.maker = 1;
  init.flags = Static;
  init.preset = perm::all_rights << 16;
  init.position = {3, 0, 0};
  const auto born =
      f.instances.create(lifecycle::expand(f.f.models, f.parameters, f.assets, 1, 0), init);
  for (auto h : born.objects) {
    const auto slot = h.slot();
    const auto hash = f.f.models.rows[f.f.objects.model[slot]].asset;
    const auto definition = bindings::describe_physics(
        f.geometry, f.assets.rows.at(hash), *f.f.projection, {f.f.objects.model[slot], 0, 8, 0, h},
        Static, {10, 11}, {.6f, 0}, 0, 20);
    bindings::install_physics(f.world, identity(f.f.objects), transforms(f.f.objects),
                              space_flags(f.f.objects), h, definition);
  }
  bindings::install_declared_attachments(f.placement, born.objects);
  std::vector<space::RelationChange> relations;
  if (remove_part)
    require(space::assemble(f.world, identity(f.f.objects), transforms(f.f.objects), born.root,
                            std::span(born.objects).subspan(1), space::Motion::Kinematic)
                    .status == space::Status::Ok,
            "quote fixture promotes the original multipart goods");
  require(space::attach(f.world, identity(f.f.objects), transforms(f.f.objects),
                        parents(f.f.objects), space_flags(f.f.objects), f.physical.contains,
                        f.physical.supports, f.physical.attached_to,
                        {born.root, f.f.a, {{3, 0, 0}, {}}, true}, relations)
                  .status == space::Status::Ok,
          "quote fixture starts with actual held goods");
  if (hidden) {
    require(space::custody(f.world, identity(f.f.objects), born.root, f.f.a, parents(f.f.objects),
                           space_flags(f.f.objects), f.physical, relations,
                           std::span(born.objects).subspan(1))
                    .status == space::Status::Ok,
            "hidden quote fixture removes every actual assembly body");
    f.players.rows[1].inventory.push_back(born.root);
  }
  auto &offers = f.offers;
  auto &trades = f.trades;
  cm::Custody custody{f.world,
                      identity(f.f.objects),
                      transforms(f.f.objects),
                      parents(f.f.objects),
                      space_flags(f.f.objects),
                      f.physical,
                      f.parts,
                      f.declared,
                      {},
                      f.projections.scene};
  cm::TradeChangeBinding changer{trades,    offers,   permission_flags(f.f.objects),
                                 f.players, f.grants, authority(f.f.objects),
                                 f.limits,  custody};
  const auto opened = perm::open_trade(trades, f.players, 1, 2);
  require(bool(opened), "actual bilateral quote opens");
  drive::NativeCall trade_call{{PlayerId{1}, 0, {}, {}},
                               {{"trade", "trade:" + std::to_string(opened.trade)},
                                {"goods", to_string(born.root)},
                                {"money", 0}},
                               {},
                               0,
                               20,
                               {}};
  const auto quoted = cm::trade_change(&changer, trade_call);
  require(quoted.changed && (f.f.objects.flags[born.root.slot()] & Escrow) &&
              bool(f.f.objects.flags[born.root.slot()] & Held) == hidden &&
              !f.f.objects.parent[born.root.slot()],
          "actual quote preserves visible geometry but records prior grasp");
  const auto offer = trades.rows.at(opened.trade).sides[0].offer;
  trades.rows[opened.trade].sides[0].confirmed = true;
  auto replacement = bindings::replacement_edit(f.replacement);
  bindings::ModelRevisionRead read{
      identity(f.f.objects), f.f.objects.params, f.parts, f.f.models, f.parameters, f.assets,
      f.f.materials,         f.f.terms,          10};
  catalog::ModelTexts texts;
  auto publication = f.publication(texts);
  cm::RebindOffersBinding goods{offers, trades, permission_flags(f.f.objects), custody};
  bindings::ModelRevisionOperation revision{
      read,
      f.f.models,
      f.parameters,
      texts,
      f.f.model_settings,
      f.f.assets,
      {},
      replacement,
      publication,
      {&goods, cm::preflight_rebound_offers, cm::rebind_offers}};
  bindings::ModelRevisionQueue queue{f.f.models, f.f.properties, f.f.processes, f.f.terms, 64};
  register_binding(f.f.vm, "revise_model", bindings::revise_model_binding, &queue);
  f.operations.rows[StructuralRequest::Kind::ReviseModel] = {bindings::revise_model, &revision};
  const auto editor = f.add(R"(return {kind='rule',applies_to={material='novel.mat'},
    on={['novel.touch']=function(self)
      self:revise_model(1,3,string.rep('1',64),{state={tokens=self.state.tokens-1}})
    end},tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=10}}}})");
  const auto result = drive::execute(f.execution, f.plan(editor), f.operations);
  if (capacity_failure) {
    require(result.invocation.outcome.status == RunOutcome::Status::Error &&
                result.invocation.outcome.error.find("capacity") != std::string::npos &&
                offers.rows[offer].live && trades.rows[opened.trade].live &&
                trades.rows[opened.trade].sides[0].confirmed &&
                f.f.models.rows[1].source_hash == std::string(64, '1') && f.tokens(editor) == 10 &&
                !f.f.objects.parent[born.root.slot()] &&
                (f.f.objects.flags[born.root.slot()] & Escrow),
            "future first-child grasp aggregate capacity denies without cancelling quote or debit");
    return;
  }
  require(result.invocation.outcome.status == RunOutcome::Status::Ok,
          result.invocation.outcome.error.c_str());
  require(
      !offers.rows[offer].live && !trades.rows[opened.trade].live && f.tokens(editor) == 9 &&
          !(f.f.objects.flags[born.root.slot()] & Escrow) &&
          (f.f.objects.flags[born.root.slot()] & Held) &&
          f.f.objects.parent[born.root.slot()] == f.f.a &&
          space::bounds(f.world, identity(f.f.objects), born.root).has_value() != hidden,
      "definition invalidates quote before extraction and restores exact visible/hidden custody");
  if (remove_part)
    require(!identity(f.f.objects).valid(born.objects[1]) && !f.parts.get(born.root.slot()),
            "quoted named child is truly retired with no custody failure after extraction");
  else if (hidden) {
    for (auto h : lifecycle::collect(f.parts, identity(f.f.objects), born.root))
      require(!space::bounds(f.world, identity(f.f.objects), h),
              "revision cannot reveal stored root or newly added parts");
    require(space::capacity(f.world).native_available == 32,
            "hidden definition revision consumes no native body capacity");
  } else {
    const auto leaf = f.parts.get(born.root.slot())->front().object;
    require(space::assembly_root(f.world, identity(f.f.objects), leaf) == born.root &&
                space::capacity(f.world).native_available == 29,
            "first new part creates the forecast aggregate, own query body and child body");
  }
}

double model_definition_batch(bool capacity_failure = false, std::size_t pairs = 1) {
  Fixture f(capacity_failure ? 8 : std::uint32_t(64 + pairs * 16));
  f.variants();
  bindings::ModelRevisionRead read{
      identity(f.f.objects), f.f.objects.params, f.parts, f.f.models, f.parameters, f.assets,
      f.f.materials,         f.f.terms,          10};
  bindings::ModelRevisionMeasure measure{read};
  bindings::ModelRevisionQueue queue{f.f.models, f.f.properties, f.f.processes, f.f.terms, 64};
  register_binding(f.f.vm, "measure_model_change", bindings::measure_model_change_binding,
                   &measure);
  register_binding(f.f.vm, "revise_model", bindings::revise_model_binding, &queue);
  const auto default_script = f.f.add(R"(return {kind='model',on={
    ['novel.touch']=function(self)self.state.noticed=true end}})");
  f.f.models.rows[1].script = default_script;
  f.f.models.rows[2].script = default_script;
  f.f.terms.rows.resize(32);
  for (TermId term : {12u, 13u}) {
    f.f.terms.rows[term].id = term;
    f.f.terms.rows[term].name = term == 12 ? "novel.volume" : "novel.capacity";
    f.f.terms.rows[term].source = Source::Measured;
    f.f.terms.by_name[f.f.terms.rows[term].name] = term;
  }
  f.f.terms.rows[12].measurement = MeasurementRole::Volume;

  f.f.models.rows.resize(7);
  f.f.models.rows[1].author = 1;
  f.f.models.rows[1].source_hash = std::string(64, '1');
  f.f.models.rows[1].canonical_params = "{\"wide\":1}";
  f.f.models.rows[4] = f.f.models.rows[1];
  f.f.models.rows[4].id = 4;
  f.f.models.rows[4].parts.clear();
  for (auto object : {f.f.a, f.f.b, f.f.c})
    f.f.objects.model[object.slot()] = 4;
  auto asset = [&](char hash, double volume, const char *params, float width) {
    testspace::Box shape({width, 1, 1});
    auto row = f.assets.rows.at(std::string(64, 'a'));
    row.hash = std::string(64, hash);
    row.source_hash = std::string(64, '2');
    row.canonical_params = params;
    row.volume = volume;
    row.minimum = shape.bounds.min;
    row.maximum = shape.bounds.max;
    row.manifest["parameter_schema"] = {{"wide", {1, 1, 4}}};
    row.manifest["parts"] = {{"leaf", {{"position", {3, 0, 0}}, {"rotation", {0, 0, 0, 1}}}},
                             {"tip", {{"position", {-2, 0, 0}}, {"rotation", {0, 0, 0, 1}}}},
                             {"cap", {{"position", {0, 2, 0}}, {"rotation", {0, 0, 0, 1}}}}};
    f.assets.rows[row.hash] = row;
    f.geometry.rows[row.hash] = {space::import_geometry(f.world, shape.source()).geometry, {}};
  };
  f.assets.rows.at(std::string(64, 'a')).source_hash = std::string(64, '1');
  f.assets.rows.at(std::string(64, 'a')).canonical_params = "{\"wide\":1}";
  f.assets.rows.at(std::string(64, 'c')).source_hash = std::string(64, '1');
  f.assets.rows.at(std::string(64, 'c')).canonical_params = "{\"wide\":2}";
  asset('f', 3, "{\"wide\":1}", 3);
  asset('g', 4, "{\"wide\":2}", 4);
  f.f.models.rows[5] = f.f.models.rows[1];
  f.f.models.rows[5].id = 5;
  f.f.models.rows[5].asset = std::string(64, 'f');
  f.f.models.rows[5].source_hash = std::string(64, '2');
  f.f.models.rows[5].parts = {{"leaf", 6}, {"tip", 3}, {"cap", 3}};
  f.f.models.rows[6] = f.f.models.rows[2];
  f.f.models.rows[6].id = 6;
  f.parameters.rows.push_back(
      {3, 5, std::string(64, 'g'), "{\"wide\":2}", f.f.models.rows[5].parts});
  const auto placed = [&](std::uint32_t params, PlayerId owner, Vec3 position) {
    auto model = lifecycle::expand(f.f.models, f.parameters, f.assets, 1, params);
    ObjectInit init;
    init.position = position;
    init.owner = owner;
    init.maker = owner;
    init.flags = Static;
    auto born = f.instances.create(model, init);
    for (auto h : born.objects) {
      const auto slot = h.slot();
      const auto selected = catalog::resolve_asset(
          f.f.models, f.parameters, f.f.objects.model[slot], f.f.objects.params[slot]);
      auto shape = bindings::describe_physics(
          f.geometry, f.assets.rows.at(selected.hash), *f.f.projection,
          {f.f.objects.model[slot], f.f.objects.params[slot], f.f.objects.material[slot], 0, h},
          Static, {10, 11}, {.6f, 0}, 0, 20);
      bindings::install_physics(f.world, identity(f.f.objects), transforms(f.f.objects),
                                space_flags(f.f.objects), h, shape);
    }
    bindings::install_declared_attachments(f.placement, born.objects);
    return born;
  };
  const auto first = placed(0, 1, {-3, 0, 0});
  const auto second = placed(1, 2, {3, 0, 0});
  for (std::size_t i = 1; i < pairs; ++i) {
    placed(0, 1, {float(i * 10), 0, 0});
    placed(1, 2, {float(i * 10 + 3), 0, 0});
  }
  const auto leaf = first.objects[1];
  f.f.objects.material[leaf.slot()] = 9;
  const auto source_before = f.f.models.rows[1].source_hash;
  const auto blocks_before = f.parameters.rows.size();
  const auto models_before = f.f.models.rows.size();
  f.limits.set(second.root.slot(), 0);
  std::vector<space::RelationChange> changes;
  require(space::assemble(f.world, identity(f.f.objects), transforms(f.f.objects), first.root,
                          std::span(first.objects).subspan(1), space::Motion::Kinematic)
                      .status == space::Status::Ok &&
              space::attach(f.world, identity(f.f.objects), transforms(f.f.objects),
                            parents(f.f.objects), space_flags(f.f.objects), f.physical.contains,
                            f.physical.supports, f.physical.attached_to,
                            {first.root, f.f.a, {{-3, 0, 0}, {}}, true}, changes)
                      .status == space::Status::Ok,
          "definition fixture holds actual promoted physical-root assembly");
  const auto masses = bindings::measure_model_change(read, 1, 5);
  require(
      masses.count == pairs * 2 &&
          std::abs(masses.before.at(8) - (3016 + (pairs - 1) * 3032)) < .0001 &&
          std::abs(masses.before.at(9) - (16 + (pairs - 1) * 8)) < .0001 &&
          std::abs(masses.after.at(8) - (7016 + (pairs - 1) * 7032)) < .0001 &&
          std::abs(masses.after.at(9) - (40 + (pairs - 1) * 32)) < .0001,
      "revision mass uses disjoint actual root/part material overrides across two parameter sets");
  catalog::ModelTexts texts;
  auto publication = f.publication(texts);
  const auto publications_before = f.projections.metadata.models;
  bindings::ModelRevisionOperation revision{read,
                                            f.f.models,
                                            f.parameters,
                                            texts,
                                            f.f.model_settings,
                                            f.f.assets,
                                            {12, 13},
                                            bindings::replacement_edit(f.replacement),
                                            publication};
  f.operations.rows[StructuralRequest::Kind::ReviseModel] = {bindings::revise_model, &revision};
  const auto editor = f.add(R"(return {kind='rule',applies_to={material='novel.mat'},
    on={['novel.touch']=function(self)
      local m=self:measure_model_change(1,5)
      local cost=math.max(0,m.after.mass_kg-m.before.mass_kg)
      assert(self.state.tokens>=cost,'insufficient stock')
      self:revise_model(1,5,string.rep('1',64),{state={tokens=self.state.tokens-cost}})
    end},tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=10}}}})");
  const auto poor = drive::execute(f.execution, f.plan(editor), f.operations);
  require(poor.invocation.outcome.status == RunOutcome::Status::Error && f.tokens(editor) == 10 &&
              f.f.models.rows[1].source_hash == source_before,
          "actual authored law rejects insufficient stock without definition or instance mutation");
  const auto stock = double(10000 * pairs);
  f.f.states[{f.f.a, editor}][std::string("tokens")] = stock;
  f.f.states[{leaf, editor}][std::string("kept")] = 55.;
  auto denied_plan = f.plan(editor);
  denied_plan.input.action_caller = PlayerId{2};
  const auto denied = drive::execute(f.execution, denied_plan, f.operations);
  require(denied.invocation.outcome.status == RunOutcome::Status::Error &&
              f.tokens(editor) == stock && f.f.models.rows[1].source_hash == source_before,
          "foreign owner cannot revise a model definition");
  const auto started = std::chrono::steady_clock::now();
  const auto result = drive::execute(f.execution, f.plan(editor), f.operations);
  const auto elapsed_us =
      std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
  if (capacity_failure) {
    require(result.invocation.outcome.status == RunOutcome::Status::Error &&
                result.invocation.outcome.error.find("capacity") != std::string::npos &&
                f.f.models.rows[1].source_hash == source_before &&
                f.parameters.rows.size() == blocks_before &&
                f.f.models.rows.size() == models_before && f.tokens(editor) == stock &&
                f.projections.metadata.models == publications_before &&
                f.parts.get(first.root.slot())->size() == 1 &&
                f.parts.get(second.root.slot())->size() == 2 &&
                space::bounds(f.world, identity(f.f.objects), first.root),
            "aggregate capacity rejects batch even though each assembly fits independently; no "
            "partial edits");
    return elapsed_us;
  }
  require(result.invocation.outcome.status == RunOutcome::Status::Ok,
          result.invocation.outcome.error.c_str());
  require(result.created.size() == 3 * pairs && f.tokens(editor) == 5976 * pairs &&
              f.f.models.rows[1].source_hash == std::string(64, '2') &&
              f.f.objects.params[first.root.slot()] == 0 &&
              f.f.objects.params[second.root.slot()] > 3 &&
              f.f.objects.owner[second.root.slot()] == 2 &&
              f.f.objects.material[leaf.slot()] == 9 &&
              std::get<double>(f.f.states.at({leaf, editor}).at(std::string("kept"))) == 55 &&
              f.parts.get(first.root.slot())->size() == 3 &&
              f.parts.get(second.root.slot())->size() == 3 &&
              f.f.objects.parent[first.root.slot()] == f.f.a &&
              space::assembly_root(f.world, identity(f.f.objects), leaf) == first.root,
          "one actual VM edit updates both owners/parameter sets, preserves held state and exact "
          "handles, charges once");
  const auto &published_models = f.projections.metadata.models;
  const auto alias = f.f.objects.model[leaf.slot()];
  require(std::find(published_models.begin(), published_models.end(), 1) !=
                  published_models.end() &&
              std::find(published_models.begin(), published_models.end(), alias) !=
                  published_models.end() &&
              f.f.triggers.at(7).by_model.at(1).script == default_script &&
              f.f.triggers.at(7).by_model.at(alias).script == default_script,
          "successful edit queues real model/alias metadata and projects their preserved "
          "default scripts");
  const auto stale = drive::execute(f.execution, f.plan(editor), f.operations);
  require(stale.invocation.outcome.status == RunOutcome::Status::Error &&
              f.tokens(editor) == 5976 * pairs,
          "stale source edit cannot apply the stock exchange twice");
  return elapsed_us;
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--bench-revision") {
      std::vector<double> samples;
      for (int i = 0; i < 100; ++i)
        samples.push_back(model_definition_batch(false, 50));
      std::ranges::sort(samples);
      std::cout << "{\"instances\":100,\"samples\":100,\"p50_us\":" << samples[50]
                << ",\"p95_us\":" << samples[95] << ",\"max_us\":" << samples.back() << "}\n";
      return 0;
    }
    preflight_failures_do_not_allocate_or_debit();
    real_creation_and_duplicate_spend();
    measured_single_solid_creation();
    removal_credits_before_cleanup_and_keeps_ordinary_occupants();
    actual_vm_approval_and_activation_gate();
    replacement_preflights_and_preserves_actual_state();
    replacement_capacity_and_full_pool_creation();
    member_replacement_does_not_widen_authority();
    material_refresh_updates_real_mass_and_optics_without_reconstruction();
    structural_rebind_notifies_actual_watched_answers();
    model_definition_quoted_topology(false);
    model_definition_quoted_topology(false, true);
    model_definition_quoted_topology(true);
    model_definition_quoted_topology(false, false, true);
    model_definition_quoted_topology(true, false, true);
    model_definition_batch();
    model_definition_batch(true);
    std::cout
        << "Actual VM structural authority, conservation, lifecycle and Jolt checks passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
