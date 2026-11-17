#include "catalog/catalog.hpp"
#include "physical_fixture.hpp"

#include <tuple>

using namespace amber;
using test_runtime::require;
namespace {
void character_collision_uses_measured_body_bounds() {
  test_runtime::Fixture f;
  space::State physical(testspace::settings(), 8);
  f.terms.rows[1].source = react::Source::Intrinsic;
  f.terms.rows[2].source = react::Source::Intrinsic;
  f.materials.rows[8] = {{1, Value(1000.)}, {2, Value(0.)}};
  testspace::Box shape({.6f, .8f, 2});
  const auto imported = space::import_geometry(physical.space, shape.source());
  require(imported.geometry != 0, "measured body geometry imports");
  catalog::Asset asset;
  asset.hash = "measured-body";
  asset.minimum = shape.bounds.min;
  asset.maximum = shape.bounds.max;
  asset.volume = .96;
  asset.manifest = {{"replicated", nlohmann::json::array()},
                    {"bindings",
                     {{"character",
                       {{"max_push_force", 200},
                        {"max_speed", 2},
                        {"acceleration", 8},
                        {"max_step", .24},
                        {"max_slope", .78},
                        {"floor_snap", .08}}}}}};
  catalog::resolve_manifest_terms(asset, f.terms);
  step::GeometryAssets geometry;
  geometry.rows[asset.hash].collision = imported.geometry;
  const auto definition = step::describe_physics(geometry, asset, f.values(), {1, 0, 8, 0, f.a},
                                                 Body, {1, 2}, {.6f, 0}, {0, 20});
  require(definition.body.mass_kg == 960 && definition.body.character->mass_kg == 960,
          "body and character controller use solid volume times material density");
  step::install_physics(physical.space, identity(f.objects), transforms(f.objects),
                        space_flags(f.objects), f.a, definition);
  const auto bounds = space::bounds(physical.space, identity(f.objects), f.a);
  require(bounds && std::abs(bounds->min.x + .4f) < 1e-5f &&
              std::abs(bounds->max.x - .4f) < 1e-5f && std::abs(bounds->min.y + .4f) < 1e-5f &&
              std::abs(bounds->max.y - .4f) < 1e-5f && std::abs(bounds->min.z) < 1e-5f &&
              std::abs(bounds->max.z - 2.f) < 1e-5f,
          "the installed character capsule encloses the wider measured axis and measured height");
}
void container_queries_use_measured_cavity() {
  test_runtime::Fixture f;
  space::State physical(testspace::settings(), 8);
  f.terms.rows[1].source = react::Source::Intrinsic;
  f.terms.rows[2].source = react::Source::Intrinsic;
  f.materials.rows[8] = {{1, Value(1000.)}, {2, Value(0.)}};
  f.objects.position[f.a.slot()] = {5, 0, 0};
  catalog::Asset asset;
  asset.hash = "measured-cavity";
  asset.manifest = {
      {"replicated", nlohmann::json::array()},
      {"collision", {{"static", nullptr}}},
      {"cavity", {{"min", {-1, -.5, .2}}, {"max", {1, .5, 1.5}}}},
      {"bindings",
       {{"container", nlohmann::json::object()},
        {"zone",
         {{"ambient", 1},
          {"priority", 1},
          {"shape", {{"kind", "box"}, {"size", {6, 4, 4}}, {"offset", {0, 0, 2}}}}}}}}};
  catalog::resolve_manifest_terms(asset, f.terms);
  step::GeometryAssets geometry;
  step::import_geometry_asset(geometry, physical.space, asset, {});
  const auto definition = step::describe_physics(geometry, asset, f.values(), {1, 0, 8, 0, f.a},
                                                 Static, {1, 2}, {.6f, 0}, {0, 20});
  step::install_physics(physical.space, identity(f.objects), transforms(f.objects),
                        space_flags(f.objects), f.a, definition);
  std::vector<Handle> found;
  space::volumes_at(physical.space, identity(f.objects), {5, 0, .8f}, space::VolumeKind::Container,
                    found);
  require(found == std::vector{f.a}, "an interior point belongs to the measured container");
  for (const Vec3 point : {Vec3{5, 0, .1f}, Vec3{6.2f, 0, .8f}, Vec3{5, 0, 1.7f}}) {
    space::volumes_at(physical.space, identity(f.objects), point, space::VolumeKind::Container,
                      found);
    require(found.empty(), "points below the floor, beyond the wall or above the rim are outside");
    space::volumes_at(physical.space, identity(f.objects), point, space::VolumeKind::Zone, found);
    require(found == std::vector{f.a}, "zone extent remains an independently authored intent");
  }
}
void physical_primitives_are_script_only() {
  test_physical::Fixture f;
  for (const auto *name :
       {"engine.grasp", "engine.release", "engine.attach", "engine.impulse", "engine.transfer"}) {
    const auto run = f.begin_as(PlayerId{1}, name, f.f.b, nlohmann::json::object());
    require(
        run.outcome && run.outcome->reply["ok"] == false &&
            run.outcome->reply["error"] == "bad_input" &&
            run.outcome->reply["problem"] ==
                "engine.grasp, release, attach, impulse and transfer are called by a body script",
        "every physical primitive rejects player callers before permission or argument checks");
  }
  const auto denials = f.journal.take();
  require(denials.size() == 5 && std::ranges::all_of(denials,
                                                     [](const auto &row) {
                                                       return row["kind"] == "denial" &&
                                                              row["result"] == "denied";
                                                     }),
          "each refused action is recorded as a denial");
  step::declare_commerce(f.f.terms, f.declarations);
  step::declare_admin(f.f.terms, f.declarations);
  const auto listed = net::player_action_ids(f.declarations, f.f.values());
  for (const auto id : listed)
    require(!std::holds_alternative<react::MechanicAction>(f.declarations.rows.at(id).operation),
            "player discovery includes no physical primitives");
  for (const auto *name : {"engine.give", "engine.offer", "engine.grant"})
    require(std::ranges::find(listed, f.f.terms.by_name.at(name)) != listed.end(),
            "ordinary native player actions remain discoverable");
}
void registry_actions_record_the_actual_player() {
  test_physical::Fixture f;
  const std::array operations{
      std::pair{react::NativeOperation{react::RegistryAction::DefineTerm}, "engine.define_term"},
      std::pair{react::NativeOperation{react::RegistryAction::DefineMaterial},
                "engine.define_material"},
      std::pair{react::NativeOperation{react::ElaborationAction::RuleActivation},
                "engine.rule_activation"}};
  for (const auto &[operation, name] : operations) {
    catalog::declare_native(f.declarations, f.f.terms, name, "Registry record classification", "",
                            false, {{"type", "object"}}, operation);
    auto run = f.begin_as(PlayerId{1}, name, {}, nlohmann::json::object());
    require(!run.outcome, "valid player registry action is admitted");
    step::NativeCompletion completion;
    completion.result = record::Result::Applied;
    completion.changed = true;
    completion.reply = {{"ok", true}};
    f.finish(run, std::move(completion));
  }
  const auto records = f.journal.take();
  require(records.size() == operations.size() &&
              std::ranges::all_of(records,
                                  [](const auto &row) {
                                    return row["kind"] == "registry" && row["actor"] == "p:1" &&
                                           row["changed"] == true;
                                  }),
          "native registry operations retain their player actor and registry kind");
}
void ordinary_transfer_and_live_processes() {
  test_physical::Fixture f;
  f.load("return {kind='model',on={['novel.touch']=function(self)end}}");
  f.f.properties[1].set(f.f.b.slot(), Value(5.));
  f.f.properties[1].set(f.f.c.slot(), Value(1.));
  const auto args = nlohmann::json{{"to", to_string(f.f.c)}, {"term", "novel.heat"}, {"amount", 2}};
  auto result = f.transfer(f.f.b, args);
  require(result.reply.contains("denied") && f.f.properties[1].get(f.f.b.slot())->numeric() == 5,
          "a body-installed script cannot borrow its owner's source rights");
  f.grant(f.f.b, perm::bit(perm::Verb::Move));
  result = f.transfer(f.f.b, args);
  require(result.reply.contains("denied") && f.f.properties[1].get(f.f.b.slot())->numeric() == 5,
          "receiver Use is checked before moving the quantity");
  f.f.objects.preset[f.f.c.slot()] = perm::bit(perm::Verb::Use);
  result = f.transfer(f.f.b, args);
  require(result.reply.at("ok") && result.reply.at("amount") == 2 &&
              f.f.properties[1].get(f.f.b.slot())->numeric() == 3 &&
              f.f.properties[1].get(f.f.c.slot())->numeric() == 3,
          "ordinary action transfers the exact conserved quantity");
  f.f.processes[1].set(f.f.b.slot(), react::Process{5, 0, -1, 0, 10});
  f.f.processes[1].set(f.f.c.slot(), react::Process{1, 0, 1, 0, 10});
  f.clock.step = 20;
  result = f.transfer(f.f.b, args);
  require(
      result.reply.at("ok") &&
          react::read_state(f.f.properties, f.f.processes, f.f.b, 1, 20, 20).value.numeric() == 2 &&
          react::read_state(f.f.properties, f.f.processes, f.f.c, 1, 20, 20).value.numeric() == 4 &&
          f.f.processes[1].get(f.f.b.slot())->rate == -1 &&
          f.f.processes[1].get(f.f.c.slot())->rate == 1,
      "transfer settles sampled values and preserves both rates");
  f.f.processes[1].set(f.f.c.slot(), react::Process{9, 20, 1, 0, 10});
  result = f.transfer(f.f.b, args);
  require(result.reply.at("ok") && result.reply.at("amount") == 1 &&
              react::read_state(f.f.properties, f.f.processes, f.f.b, 1, 20, 20).value.numeric() ==
                  1 &&
              react::read_state(f.f.properties, f.f.processes, f.f.c, 1, 20, 20).value.numeric() ==
                  10 &&
              f.f.processes[1].get(f.f.b.slot())->rate == -1 &&
              f.f.processes[1].get(f.f.c.slot())->rate == 1,
          "destination process ceiling clamps actual transfer without losing quantity or rates");
  result = f.transfer(f.f.b, args);
  require(result.reply.at("ok") && result.reply.at("amount") == 0 &&
              react::read_state(f.f.properties, f.f.processes, f.f.b, 1, 20, 20).value.numeric() ==
                  1,
          "a full destination leaves the source unchanged");
  f.f.processes[1].erase(f.f.b.slot());
  f.f.processes[1].erase(f.f.c.slot());
  f.f.properties[1].set(f.f.b.slot(), Value(5.));
  f.f.properties[1].set(f.f.c.slot(), Value(1.));
  auto bounded = args;
  bounded["amount"] = 10;
  bounded["to_max"] = 2;
  result = f.transfer(f.f.b, bounded);
  require(result.reply.at("amount") == 1 && f.f.properties[1].get(f.f.b.slot())->numeric() == 4 &&
              f.f.properties[1].get(f.f.c.slot())->numeric() == 2,
          "explicit receiver bound uses the conserved term's units and returns actual debit");
  bounded["to_max"] = 100;
  result = f.transfer(f.f.b, bounded);
  require(result.reply.at("amount") == 4 && f.f.properties[1].get(f.f.b.slot())->numeric() == 0 &&
              f.f.properties[1].get(f.f.c.slot())->numeric() == 6,
          "source availability caps a larger request and preserves the exact total");
  f.f.processes[1].set(f.f.b.slot(), react::Process{3, 20, -1, 2, 10});
  f.f.properties[1].set(f.f.c.slot(), Value(0.));
  result = f.transfer(f.f.b, bounded);
  require(result.reply.at("amount") == 1 &&
              react::read_state(f.f.properties, f.f.processes, f.f.b, 1, 20, 20).value.numeric() ==
                  2 &&
              f.f.properties[1].get(f.f.c.slot())->numeric() == 1,
          "source process floor remains in its own reservoir");
  f.f.objects.preset[f.f.c.slot()] = 0;
  result = f.transfer(f.f.b, bounded);
  require(result.reply.contains("denied") &&
              react::read_state(f.f.properties, f.f.processes, f.f.b, 1, 20, 20).value.numeric() ==
                  2 &&
              f.f.properties[1].get(f.f.c.slot())->numeric() == 1,
          "both endpoints stay unchanged when destination authority is revoked");
}
void ordinary_grasp_release_seat_and_flight() {
  test_physical::Fixture f;
  f.load("return {kind='model',on={['novel.touch']=function(self)end}}");
  auto result = f.grasp(f.f.b, {{"local_pose", {{"position", {0, 1, 1}}}}});
  require(!result.reply.at("ok").get<bool>() && !f.f.objects.parent[f.f.b.slot()],
          "ordinary admission denies Take without inheriting the owner's rights");
  const auto take = f.grant(f.f.b, perm::bit(perm::Verb::Take));
  result = f.grasp(f.f.b, {{"local_pose", {{"position", {0, 1, 1}}}}});
  require(result.reply.at("effect") == "applied" && f.f.objects.parent[f.f.b.slot()] == f.f.a,
          "explicit Take admits actual Jolt grasp");
  perm::revoke(f.grants, f.players, take);
  result = f.release(f.f.b, {{"linear", {3, 0, 2}}});
  require(result.reply.at("effect") == "applied" && !(f.f.objects.flags[f.f.b.slot()] & Held),
          "releasing an item held here clears real held state without owner fallback");
  const auto before = f.f.objects.position[f.f.b.slot()];
  for (int i = 0; i < 5; ++i)
    f.tick();
  require(f.f.objects.position[f.f.b.slot()].x > before.x + .3f,
          "released item follows actual dynamic velocity");
  f.f.objects.preset[f.f.c.slot()] = perm::bit(perm::Verb::Use);
  const auto holder = perm::Holder::script(f.script, f.f.a);
  const auto scope = perm::Scope::avatar(f.f.a);
  const auto consent = perm::create_grant(f.grants, f.players, perm::Holder::player(1),
                                          {1, holder, scope, 1, 0, 0, 10000},
                                          {scope, 1, 3, 0, false}, {holder, 1, true}, f.clock.step);
  require(bool(consent), "the avatar owner explicitly consents to animation");
  result =
      f.attach(f.f.a, {{"parent", to_string(f.f.c)}, {"local_pose", {{"position", {0, 0, .6}}}}});
  require(result.reply.at("ok") && f.f.objects.parent[f.f.a.slot()] == f.f.c,
          "ordinary Attach seats the actual body after Avatar and parent Use checks");
  result = f.release(f.f.a);
  require(result.reply.at("ok") && !f.f.objects.parent[f.f.a.slot()] &&
              space::body_motion(f.space, identity(f.f.objects), f.f.a)->motion ==
                  space::Motion::Character,
          "detaching a body restores its character controller");
}
void own_motion_queries_and_speech() {
  test_physical::Fixture f;
  f.instance_texts.rows[f.f.a.bits].name = "Public speaker";
  f.listeners = {{1, f.f.a, {0, 0, 1}}};
  const auto script = f.load(R"(return {kind='model',actions={['novel.touch']={right='use'}},on={
    ['novel.touch']=function(self,_,args)
      if args.mode=='walk' then self:navigate(vector.create(4,0,0),{speed_m_s=0.3})
      elseif args.mode=='stop' then self:cancel_motion()
      elseif args.mode=='speech' then self:sound('hello',5,nil,{kind='speech'})
      else self.state.said=true;self:sound('hello',5,nil,{kind='speech'}) end
    end}})");
  auto result = f.invoke(std::vector<EventField>{{"mode", std::string("walk")}});
  require(result.outcome.status == react::RunOutcome::Status::Ok && !result.changed &&
              !f.intents.moving.empty() && f.intents.moving.at(f.f.a.bits).speed_m_s == .3f,
          "own navigation starts an intent with authored speed");
  result = f.invoke(std::vector<EventField>{{"mode", std::string("stop")}});
  require(!result.changed && f.intents.moving.empty(),
          "own intent control is not a stored world mutation");
  f.journal.take();
  result = f.invoke(std::vector<EventField>{{"mode", std::string("speech")}});
  require(!result.changed, "speech alone is not a stored world change");
  auto logs = f.journal.take();
  require(logs.back().at("kind") == "speech" && logs.back().at("actor") == "p:1" &&
              !logs.back().at("changed").get<bool>() && !logs.back().at("agent").get<bool>(),
          "explicit speech preserves player display and nonchosen event classification");
  std::size_t omitted = 0;
  auto heard = observe::take_heard(f.hearing, 1, 10, omitted);
  require(heard.size() == 1 && heard[0].sound.speech && heard[0].sound.name == "Public speaker",
          "heard speech carries canonical public speaker metadata");
  result = f.invoke(std::vector<EventField>{{"mode", std::string("stored")}});
  require(result.changed &&
              std::get<bool>(f.f.states.at({f.f.a, script}).values.at(std::string("said"))),
          "speech preserves an independent earlier state write");
}
void immediate_part_motion_reallocates_plot_priority() {
  test_physical::Fixture f(testspace::settings(), space::Motion::Kinematic);
  for (const auto &[id, name] :
       {std::pair{10u, "novel.available"}, {11u, "novel.wanted"}, {12u, "novel.priority"}}) {
    f.f.terms.rows[id].id = id;
    f.f.terms.rows[id].name = name;
    f.f.terms.by_name[name] = id;
  }
  f.f.terms.rows[10].combine = react::Combine::Sum;
  f.f.terms.rows[11].combine = react::Combine::SharePriority;
  f.f.terms.rows[11].paired = 10;
  f.f.terms.rows[11].priority = 12;
  f.f.terms.rows[11].along = 3;
  f.f.terms.rows[12].source = react::Source::Setting;
  react::validate_shared_terms(f.f.terms);
  f.plots.rows.push_back({1, 1, 0, 0, {-5, -5, 0, 10}, {}});
  f.plots.rows.push_back({2, 1, 0, 0, {0, -5, 5, 10}, {}});
  f.f.plots.rows[1][12] = Value(0.);
  f.f.plots.rows[2][12] = Value(10.);
  f.f.instance_settings.columns.resize(13);
  f.f.instance_settings.columns[12].set(f.f.a.slot(), Value(5.));
  require(space::set_pose(f.space, identity(f.f.objects), transforms(f.f.objects), f.f.b,
                          {{-1, 5, 0}, {0, 0, 1, 0}})
                  .status == space::Status::Ok,
          "place the independent part on the lower-priority plot");
  f.parts.out.set(f.f.a.slot(), std::vector{f.f.b});
  f.parts.in.set(f.f.b.slot(), std::vector{f.f.a});
  const std::array objects{f.f.a, f.f.b, f.f.c};
  perm::update_plots(f.f.objects, f.plots, objects, true);
  react::set_relation(f.f.relations.at(3), 3, f.f.a, f.f.c, true, f.f.effects);
  for (const auto &[from, term] : {std::pair{f.f.c, 10u}, {f.f.a, 11u}, {f.f.b, 11u}})
    react::set_contribution(f.f.shared, f.f.gives_to, f.f.terms.rows[term], from, f.f.c, 6,
                            f.f.effects);
  react::reconcile_allocations(f.f.values(), f.react_state, f.f.effects, 0, 20);
  f.f.effects = {};
  const auto watcher = f.f.add(R"(return {kind='instance',watches={'novel.priority'},on={
    ['novel.priority']=function(self,_,value) self.state.observed=value end}})");
  react::set_instances(f.f.installed, f.f.b, std::span(&watcher, 1), f.f.scripts);
  react::project_instance(f.f.triggers, f.f.installed, f.f.scripts, f.f.terms, f.f.b);
  f.f.baseline(f.f.b);
  const auto script = f.load(R"(return {kind='model',actions={['novel.touch']={right='use'}},on={
    ['novel.touch']=function(self,_,args)
      local circuit=self:related('novel.link','out')[1]
      self.state.before=self:get_share(circuit,'novel.wanted')
      local part=self:parts()[1]
      if args.mode=='part' then self:move_part(part,vector.create(1,5,0))
      else assert(self:sweep(part,vector.create(-2,0,0)).effect=='done') end
      self.state.after=self:get_share(circuit,'novel.wanted')
    end}})");
  for (const auto &[mode, before, after, priority] :
       {std::tuple{"part", 6., 0., 10.}, std::tuple{"sweep", 0., 6., 0.}}) {
    const auto result = f.invoke(std::vector<EventField>{{"mode", std::string(mode)}});
    require(result.outcome.status == react::RunOutcome::Status::Ok, result.outcome.error.c_str());
    const auto &state = f.f.states.at({f.f.a, script}).values;
    require(std::get<double>(state.at(std::string("before"))) == before &&
                std::get<double>(state.at(std::string("after"))) == after,
            "immediate part movement updates allocation before the next Lua read");
    require(f.f.objects.rotation[f.f.b.slot()] == Quat{0, 0, 1, 0},
            "moving a part without a rotation preserves its orientation");
    while (auto work = step::pop_foreground(f.queues))
      if (const auto *watch = std::get_if<react::WatchWork>(&work->work))
        f.f.run(f.f.changed(watch->source));
    require(std::get<double>(f.f.states.at({f.f.b, watcher}).values.at(std::string("observed"))) ==
                priority,
            "crossing plots delivers the inherited-setting change to the part's watcher");
  }
}
void declared_part_batch_and_foreign_rejection(bool benchmark = false) {
  test_physical::Fixture f;
  std::vector<Handle> parts;
  for (int index = 0; index < 300; ++index) {
    auto h = f.box({.03f, .08f, .03f}, {0, 0, 1}, space::Motion::Kinematic, false);
    parts.push_back(h);
    f.parts.in.set(h.slot(), std::vector<Handle>{f.f.a});
  }
  f.parts.out.set(f.f.a.slot(), parts);
  const auto script = f.load(R"(return {kind='model',actions={['novel.touch']={right='use'}},on={
    ['novel.touch']=function(self,selected,args)
      if args.mode=='foreign' then self:move_part(selected,vector.create(10,0,0),{x=0,y=0,z=0,w=1});return end
      self.state.phase=(self.state.phase or 0)+.1
      local rows={}
      for index,part in self:parts() do
        rows[index]={object=part,position=vector.create(index%20*.05,index//20*.05,1+math.sin(self.state.phase)*.2)}
      end
      self.state.count=self:move_part(rows)
    end}})");
  auto result = f.invoke(std::vector<EventField>{{"mode", std::string("batch")}});
  require(result.outcome.status == react::RunOutcome::Status::Ok && result.changed &&
              std::get<double>(f.f.states.at({f.f.a, script}).values.at(std::string("count"))) ==
                  300,
          "one script batches all actual declared part poses without foreign Move grants");
  const auto before = f.f.objects.position[f.f.c.slot()];
  result = f.invoke(std::vector<EventField>{{"mode", std::string("foreign")}}, f.f.c);
  require(result.outcome.status == react::RunOutcome::Status::Error &&
              f.f.objects.position[f.f.c.slot()] == before,
          "acquisition does not confer own-part mutation authority");
  std::vector<double> costs;
  for (int index = 0; index < (benchmark ? 100 : 1); ++index) {
    auto start = std::chrono::steady_clock::now();
    result = f.invoke(std::vector<EventField>{{"mode", std::string("batch")}});
    require(result.outcome.status == react::RunOutcome::Status::Ok, "actual batch completes");
    costs.push_back(
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
            .count());
    f.journal.take();
    f.tick();
  }
  if (benchmark) {
    std::sort(costs.begin(), costs.end());
    std::cout << "ordinary_own_parts_300_us p50=" << costs[50] << " p95=" << costs[95]
              << " max=" << costs.back() << '\n';
  }
}
void exact_queries_do_not_grant_event_authority() {
  test_physical::Fixture f;
  f.load(R"(return {kind='model',actions={['novel.touch']={right='use'}},on={
    ['novel.touch']=function(self,sender,args)
      if args.mode=='related' then
        self:emit(self:related('novel.link','out')[1],'novel.cue',true)
      elseif args.mode=='event' then self:emit(sender,'novel.cue',true)
      else
        local nearby=world.near(self,5)
        assert(#nearby.objects>=2)
        local hit=world.raycast(vector.create(0,3,.2),vector.create(0,.5,.2))
        assert(hit.object~=nil)
        self.state.hit=hit.object
        self.state.world_readonly=not pcall(function() world.near=nil end)
        if args.mode=='emit_query' then self:emit(hit.object,'novel.cue',true) end
      end
    end}})");
  const auto read = f.invoke(std::vector<EventField>{{"mode", std::string("query")}});
  require(
      read.outcome.status == react::RunOutcome::Status::Ok &&
          std::get<Handle>(f.f.states.at({f.f.a, f.script}).values.at(std::string("hit"))) ==
              f.f.b &&
          std::get<bool>(f.f.states.at({f.f.a, f.script}).values.at(std::string("world_readonly"))),
      "readonly world queries return actual geometry handles and readonly query methods");
  const auto denied = f.invoke(std::vector<EventField>{{"mode", std::string("emit_query")}});
  require(denied.outcome.status == react::RunOutcome::Status::Error && f.queues.causal.empty(),
          "an exact query grants no event mutation authority on an unrelated object");
  react::set_relation(f.f.relations.at(3), 3, f.f.a, f.f.b, true, f.f.effects);
  const auto related = f.invoke(std::vector<EventField>{{"mode", std::string("related")}});
  require(related.outcome.status == react::RunOutcome::Status::Ok && !f.queues.causal.empty(),
          "an actual related endpoint remains eligible for emitted events");
  while (step::pop_foreground(f.queues)) {
  }
  const auto event = f.invoke(std::vector<EventField>{{"mode", std::string("event")}}, f.f.c);
  require(event.outcome.status == react::RunOutcome::Status::Ok && !f.queues.causal.empty(),
          "the current event sender remains eligible for emitted events");
}
void native_query_deadline_keeps_effects_and_step_continues() {
  test_physical::Fixture f;
  f.load(R"(return {kind='model',on={['novel.touch']=function(self,_,args)
    self:set('novel.result', 7)
    if args and args.loop then while true do world.near(self, 10) end end
  end}})");
  const auto result = f.invoke(std::vector<EventField>{{"loop", Value(true)}});
  require(result.outcome.status == react::RunOutcome::Status::Cutoff &&
              f.f.properties[2].get(f.f.a.slot())->numeric() == 7,
          "native query loop cuts off and keeps prior effects");
  const auto records = f.journal.take();
  require(std::ranges::count_if(records,
                                [](const auto &row) {
                                  return row["kind"] == "system" && row["name"] == "cutoff";
                                }) == 1,
          "query cutoff is recorded exactly once");
  f.tick();
  require(f.invoke().outcome.status == react::RunOutcome::Status::Ok,
          "the next step can run a handler after a query cutoff");
}
void foreign_avatar_motion_requires_its_own_scope() {
  test_physical::Fixture f;
  f.load("return {kind='model',on={['novel.touch']=function(self)end}}");
  f.f.objects.flags[f.f.c.slot()] |= Body;
  f.f.objects.preset[f.f.c.slot()] = perm::bit(perm::Verb::Move);
  f.grant(f.f.c, perm::bit(perm::Verb::Move));
  const auto args = nlohmann::json{{"momentum", {4, 0, 0}}, {"point", {1, 0, .2}}};
  auto result = f.impulse(f.f.c, args);
  require(!result.reply.at("ok").get<bool>() && result.reply.at("denied").at("need") == "animate",
          "public and object Move cannot replace explicit foreign Avatar authority");
  const auto holder = perm::Holder::script(f.script, f.f.a);
  const auto scope = perm::Scope::avatar(f.f.c);
  const auto grant = perm::create_grant(f.grants, f.players, perm::Holder::player(2),
                                        {2, holder, scope, 1, 0, 0, 10000}, {scope, 2, 3, 0, false},
                                        {holder, 1, true}, 0);
  require(bool(grant), "avatar owner explicitly grants animation to the installed script");
  result = f.impulse(f.f.c, args);
  require(result.reply.at("ok") && result.reply.at("effect") == "applied",
          "the exact Avatar grant admits actual native motion");
  perm::revoke(f.grants, f.players, grant.grant);
  require(!f.impulse(f.f.c, args).reply.at("ok").get<bool>(),
          "revoking Avatar authority affects the next ordinary action");
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--bench") {
      declared_part_batch_and_foreign_rejection(true);
      return 0;
    }
    character_collision_uses_measured_body_bounds();
    container_queries_use_measured_cavity();
    physical_primitives_are_script_only();
    registry_actions_record_the_actual_player();
    ordinary_transfer_and_live_processes();
    ordinary_grasp_release_seat_and_flight();
    own_motion_queries_and_speech();
    immediate_part_motion_reallocates_plot_priority();
    declared_part_batch_and_foreign_rejection();
    exact_queries_do_not_grant_event_authority();
    native_query_deadline_keeps_effects_and_step_continues();
    foreign_avatar_motion_requires_its_own_scope();
    std::cout << "Ordinary physical actions and own-part Lua primitives passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
