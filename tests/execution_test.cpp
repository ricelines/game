#include "catalog/catalog.hpp"
#include "react/react.hpp"
#include "runtime_fixture.hpp"
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
  amber::react::State react_state{{4096, 50, 5, 64}};
  amber::catalog::State catalog_state;

  Objects &objects = objects_state.objects;
  Handle object, other;
  react::Terms &terms = react_state.terms;
  react::Properties &properties = react_state.properties;
  react::Processes &processes = react_state.processes;
  react::Materials &materials = react_state.materials;
  react::AssetMeasurements &assets = catalog_state.baked_measurements;
  react::InstanceSettings &settings = react_state.instance_settings;
  react::ModelSettings &models = react_state.model_settings;
  react::PlotSettings &plots = react_state.plot_settings;
  react::WorldSettings &world = react_state.world_settings;
  react::SharedColumns &shared = react_state.shared;
  react::RelationsRead &relations = react_state.relations;
  react::RuntimeMeasurements &runtime = react_state.runtime_measurements;
  react::ScriptStates &states = react_state.states;
  react::RandomStates &random = react_state.random;
  Relation &parts = objects_state.declared_parts;
  record::Journal journal{8, {}};
  react::Vm &vm = react_state.vm;
  react::ValueRead values;
  react::GivesTo &gives_to = react_state.gives_to;
  catalog::Models &definitions = catalog_state.models;
  catalog::Scripts &scripts = catalog_state.scripts;
  react::InstanceScripts &installations = react_state.installations;
  react::Triggers &triggers = react_state.triggers;
  Fixture()
      : values{terms,        identity(objects),
               objects.plot, objects.params,
               materials,    assets,
               properties,   processes,
               settings,     models,
               plots,        world,
               shared,       relations,
               runtime,      0} {
    ObjectInit init;
    init.model = 1;
    object = create(objects, init);
    other = create(objects, init);
    terms.rows.resize(5);
    for (TermId i = 1; i < terms.rows.size(); ++i) {
      terms.rows[i].id = i;
      terms.rows[i].name = "unforeseen." + std::to_string(i);
      terms.by_name[terms.rows[i].name] = i;
    }
    terms.rows[1].kind = react::Kind::Action;
    terms.rows[2].kind = react::Kind::Event;
    terms.rows[3].source = react::Source::State;
    terms.rows[4].kind = react::Kind::Event;
    definitions.rows.resize(2);
    definitions.rows[1].id = 1;
    react::bind_values(vm, values, react_state);
    react::bind_events(vm, terms);
    load(1, R"(return {kind='model', sets={'unforeseen.3'},
      on={['unforeseen.1']=function(self,from,args)
        self:set('unforeseen.3',1)
        self:emit(self.handle,'unforeseen.2',{amount=1})
      end}})");
    load(2, R"(return {kind='instance', sets={'unforeseen.3'},
      on={['unforeseen.1']=function(self,from,args)
        assert(self:get('unforeseen.3')==1)
        self:set('unforeseen.3',11)
      end,
      ['unforeseen.4']=function(self)
        self.state.last=self:random()
        self.state.count=(self.state.count or 0)+1
      end}})");
    load(3, R"(return {kind='instance', sets={'unforeseen.3'},
      on={['unforeseen.1']=function(self)
        self:set('unforeseen.3',111)
        self:emit(self.handle,'unforeseen.2',111)
        error('deliberate partial failure')
      end}})");
    definitions.rows[1].script = 1;
    react::project_model(triggers, definitions, scripts, terms, 1);
    const std::array instance_scripts{ScriptId{2}, ScriptId{3}};
    for (auto receiver : {object, other}) {
      react::set_instances(installations, receiver, instance_scripts, scripts);
      react::project_instance(triggers, installations, scripts, terms, receiver);
    }
  }
  void load(ScriptId id, std::string_view source) {
    auto analyzed = react::analyze_script(source, terms, {}, {4096, 50, 5, 64});
    require(analyzed.errors.empty(), "real source analysis");
    analyzed.declaration.id = id;
    analyzed.declaration.author = id + 10;
    if (scripts.rows.size() <= id)
      scripts.rows.resize(id + 1);
    scripts.rows[id].id = id;
    scripts.rows[id].author = id + 10;
    scripts.rows[id].declaration = analyzed.declaration;
    const auto loaded = react::load_script(vm, analyzed.compiled, analyzed.declaration);
    require(loaded.status == react::RunOutcome::Status::Ok, loaded.error.c_str());
  }
  react::CompletedInvocation run(ScriptId script, TermId term = 1, Handle receiver = {},
                                 const amber::Origin &origin = {}) {
    react::InvocationInput input;
    input.receiver = receiver ? receiver : object;
    input.sender = other;
    input.trigger = term;
    input.step = 12;
    input.origin = origin;
    input.action_caller = PlayerId{7};
    react::Invocation invocation;
    react::CompletedInvocation completed;
    react::start_invocation(react_state, objects_state, 971, catalog_state, journal,
                            {script, terms.rows[term].name, script + 10, 1, true}, std::move(input),
                            invocation, completed);
    react::finish_invocation(identity(objects), attribution(objects), definitions, scripts, journal,
                             invocation, completed);
    return completed;
  }
};
void stored_scripts_keep_data_and_restart_without_a_saved_execution() {
  Fixture f;
  const auto key = ScriptKey{f.object, 2};
  auto first = f.run(2, 4);
  require(first.outcome.status == react::RunOutcome::Status::Ok, "script started");
  const auto count = f.states.at(key).values.at(std::string("count"));
  f.objects.flags[f.object.slot()] |= Held;
  set_plot(f.objects, f.object.slot(), 0);
  require(!react::find_handler(f.triggers, f.values, key, 4, 20, 20),
          "stored script stayed active");
  require(f.states.at(key).values.at(std::string("count")) == count, "storage changed script data");
  // A visible held item still runs; hidden inventory alone stops execution.
  set_plot(f.objects, f.object.slot(), 1);
  require(bool(react::find_handler(f.triggers, f.values, key, 4, 20, 20)),
          "hand-held script was stopped");
  f.objects.flags[f.object.slot()] &= ~Held;
  auto next = f.run(2, 4);
  require(next.outcome.status == react::RunOutcome::Status::Ok &&
              std::get<double>(f.states.at(key).values.at(std::string("count"))) ==
                  std::get<double>(count) + 1,
          "placed script could not continue from ordinary stored data");
}
void live_effects_and_causal_finalization() {
  Fixture f;
  auto &journal = f.journal;
  step::WorkQueues queues;
  step::TimerWheel timers;
  record::Event action;
  action.step = 12;
  action.kind = record::Kind::Action;
  action.actor = record::Actor::player(7);
  action.name = "unforeseen.1";
  const auto origin = journal.append(std::move(action));
  auto first = f.run(1, 1, {}, origin);
  auto second = f.run(2, 1, {}, origin);
  auto failed = f.run(3, 1, {}, origin);
  require(first.outcome.status == react::RunOutcome::Status::Ok &&
              second.outcome.status == react::RunOutcome::Status::Ok &&
              failed.outcome.status == react::RunOutcome::Status::Error &&
              react::read_state(f.properties, f.processes, f.object, 3, 12, 20).value.numeric() ==
                  111,
          "later handlers see prior writes and error preserves completed state");
  journal.finish(origin, record::Result::Applied, true, {});
  step::publish_invocation(journal, queues, timers, std::move(first), origin, 1000);
  step::publish_invocation(journal, queues, timers, std::move(second), origin, 1000);
  step::publish_invocation(journal, queues, timers, std::move(failed), origin, 1000);
  const auto logs = journal.take();
  require(logs.size() == 5 && logs[0]["kind"] == "action" && logs[0]["changed"] == true &&
              logs[1]["kind"] == "handler" && logs[1]["cause"] == logs[0]["id"] &&
              logs[2]["kind"] == "handler" && logs[3]["kind"] == "handler" &&
              logs[4]["kind"] == "system" && logs[4]["cause"] == logs[3]["id"],
          "action precedes completed handlers and their diagnostics retain direct cause");
  auto watch = step::pop_foreground(queues);
  require(watch && std::holds_alternative<react::WatchWork>(watch->work),
          "changed fact enters foreground");
  auto event = step::pop_foreground(queues);
  require(event && std::get<react::EventWork>(event->work).origin.cause == logs[1]["id"],
          "emitted event uses completed first-handler origin");
  auto failed_event = step::pop_foreground(queues);
  require(failed_event && std::holds_alternative<react::EventWork>(failed_event->work) &&
              queues.causal.empty(),
          "partial-error emissions are retained without a deferred action lane");
}
void persistence_and_activation() {
  Fixture f;
  const auto first = f.run(2, 4);
  require(first.outcome.status == react::RunOutcome::Status::Ok, "first RNG handler");
  const auto draw = std::get<double>(f.states.at({f.object, 2}).values.at(std::string("last")));
  f.run(2, 4);
  require(std::get<double>(f.states.at({f.object, 2}).values.at(std::string("count"))) == 2 &&
              std::get<double>(f.states.at({f.object, 2}).values.at(std::string("last"))) != draw,
          "scalar state and RNG continue across invocations");
  f.run(2, 4, f.other);
  require(std::get<double>(f.states.at({f.other, 2}).values.at(std::string("count"))) == 1,
          "another installation has its own persisted scalar state");
  react::set_activation(f.vm, 2, false, false);
  const auto disabled = f.run(2, 4);
  require(disabled.outcome.status == react::RunOutcome::Status::Missing &&
              std::get<double>(f.states.at({f.object, 2}).values.at(std::string("count"))) == 2,
          "inactive cached script cannot execute even with an old handler reference");
}
void world_declarations_reject_object_requirements() {
  react::Terms terms;
  terms.rows.resize(3);
  terms.rows[1].id = 1;
  terms.rows[1].name = "unforeseen.action";
  terms.rows[1].kind = react::Kind::Action;
  terms.by_name[terms.rows[1].name] = 1;
  terms.rows[2].id = 2;
  terms.rows[2].name = "unforeseen.setting";
  terms.rows[2].kind = react::Kind::Quantity;
  terms.rows[2].source = react::Source::Setting;
  terms.by_name[terms.rows[2].name] = 2;
  const auto analyze = [&](std::string_view kind, std::string_view action) {
    const auto source = "return {kind='" + std::string(kind) +
                        "',actions={['unforeseen.action']={" + std::string(action) +
                        "}},on={['unforeseen.action']=function(self)end}}";
    return react::analyze_script(source, terms, {{"edit"}}, {4096, 50, 5, 64});
  };
  require(analyze("world", "").errors.empty() &&
              analyze("world", "right='',reach=false").errors.empty(),
          "world actions accept declarations without object permission or reach requirements");
  require(!analyze("world", "right='edit'").errors.empty() &&
              !analyze("world", "reach=true").errors.empty(),
          "world declarations must reject object requirements that execution cannot enforce");
  require(analyze("model", "right='edit',reach=true").errors.empty(),
          "object action declarations retain their ordinary permission and reach requirements");
  const auto setting_handler = [&](std::string_view kind, std::string_view watches) {
    const auto source = "return {kind='" + std::string(kind) + "'," + std::string(watches) +
                        "on={['unforeseen.setting']=function(self)end}}";
    return catalog::prepare_script(source, terms, {}, catalog::sha256("world setting fixture"), {},
                                   {4096, 50, 5, 64});
  };
  require(setting_handler("world", "").declaration.handlers == std::vector<TermId>{2},
          "world scripts receive setting-change events through ordinary handlers");
  for (const auto watches :
       {"watches={'unforeseen.setting'},",
        "watches={{term='unforeseen.setting',thresholds={{predicate='below',value=2}}}},"}) {
    bool rejected = false;
    try {
      setting_handler("world", watches);
    } catch (const std::runtime_error &error) {
      rejected = std::string_view(error.what()).find("cannot declare object watches") !=
                 std::string_view::npos;
    }
    require(rejected, "world declarations must reject both ordinary and threshold object watches");
    require(setting_handler("model", watches).declaration.watches.size() == 1,
            "ordinary scripts retain their setting watch and threshold declarations");
  }
}
void world_installations_share_the_runtime_without_host_objects() {
  Fixture f;
  react::Term setting;
  setting.id = 5;
  setting.name = "unforeseen.5";
  setting.source = react::Source::Setting;
  f.terms.rows.push_back(setting);
  f.terms.by_name[setting.name] = setting.id;
  f.world.values[5] = Value(42.0);
  constexpr auto source = R"(return {kind='world', sets={'unforeseen.3'},
    actions={['unforeseen.1']={}},on={
    ['unforeseen.2']=function(self)
      assert(self.handle==nil and self:setting('unforeseen.5')==42)
      assert(not pcall(function()self:get('unforeseen.3')end))
      assert(not pcall(function()self:set('unforeseen.3',99)end))
      self.state.count=(self.state.count or 0)+1
      self.state.draw=self:random()
      self:after(0.5,'unforeseen.2')
    end,
    ['unforeseen.1']=function(self)
      local result=self:act('unforeseen.1',nil,{})
      self.state.done=result.ok
    end}})";
  f.load(4, source);
  f.load(5, source);
  react::Invocation uninstalled;
  react::CompletedInvocation ignored;
  react::InvocationInput uninstalled_input;
  uninstalled_input.trigger = 2;
  require(!react::start_invocation(f.react_state, f.objects_state, 971, f.catalog_state, f.journal,
                                   {4, "unforeseen.2", 14, 1, true}, uninstalled_input, uninstalled,
                                   ignored) &&
              !f.states.contains({{}, 4}),
          "uninstalled world script created persistent state");
  const auto objects_before = f.objects.model.size();
  f.react_state.world_scripts = {{4, "civic", "Public services", {}},
                                 {5, "market", "Market services", {}}};
  react::project_world(f.react_state, f.scripts);
  const auto start = react::EventWork{{}, {}, 2, {}, {}, 4};
  auto planned = step::plan_event(f.triggers, f.values, start, 12, 20);
  require(planned.size() == 1 && planned[0].handler.script == 4,
          "targeted world event reached another installation");
  react::Invocation invocation;
  react::CompletedInvocation completed;
  require(react::start_invocation(f.react_state, f.objects_state, 971, f.catalog_state, f.journal,
                                  planned[0].handler, std::move(planned[0].input), invocation,
                                  completed) &&
              completed.outcome.status == react::RunOutcome::Status::Ok,
          "world handler could not run in the ordinary VM");
  const auto &timer = std::get<react::Timer>(completed.effects.ordered.at(0));
  require(timer.due == 22 && !timer.event.receiver && timer.event.recipient_script == 4,
          "world timer lost its installation identity");
  auto next = step::plan_event(f.triggers, f.values, timer.event, 22, 20);
  require(next.size() == 1 && next[0].handler.script == 4 && !f.states.contains({{}, 5}) &&
              f.objects.model.size() == objects_before,
          "world execution created a host object or another script's state");
  react::CompletedInvocation second;
  require(react::start_invocation(f.react_state, f.objects_state, 971, f.catalog_state, f.journal,
                                  next[0].handler, std::move(next[0].input), invocation, second) &&
              std::get<double>(f.states.at({{}, 4}).values.at(std::string("count"))) == 2,
          "world timer did not continue the existing installation state");
  require(react::dispatch(f.triggers, f.values, f.object, 2, 22, 20).empty(),
          "world handlers leaked into object dispatch");
  auto action = step::plan_event(f.triggers, f.values, {{}, {}, 1, {}, {}, 4}, 22, 20);
  react::CompletedInvocation yielded;
  require(react::start_invocation(f.react_state, f.objects_state, 971, f.catalog_state, f.journal,
                                  action[0].handler, std::move(action[0].input), invocation,
                                  yielded) &&
              yielded.outcome.status == react::RunOutcome::Status::Action &&
              std::get<ScriptHolder>(yielded.outcome.action->request.caller) == ScriptHolder{4, {}},
          "world action did not carry its actual script principal");
  require(react::resume_handler(f.vm, invocation, {{"ok", true}}).status ==
                  react::RunOutcome::Status::Ok &&
              std::get<bool>(f.states.at({{}, 4}).values.at(std::string("done"))),
          "world handler did not resume after its ordinary action");
  bool rejected = false;
  try {
    const std::array world_script{ScriptId{4}};
    react::set_instances(f.installations, f.object, world_script, f.scripts);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "world script was installed on an object");
  f.react_state.world_scripts.clear();
  react::project_world(f.react_state, f.scripts);
  require(react::dispatch(f.triggers, f.values, {}, 2, 22, 20).empty(),
          "removed world installation retained a dispatch entry");
}
void withdrawing_from_retired_subject_is_idempotent() {
  test_runtime::Fixture f(1000);
  const auto script = f.add(R"(return {kind='model',contributes={'novel.signal'},on={
    ['novel.cue']=function(self) self:contribute(self.state.old,'novel.signal',0) end}})");
  react::ScriptState state;
  state.insert(std::string("old"), f.b);
  destroy(f.objects, f.b);
  react::Invocation invocation;
  invocation.self = {f.a, script};
  invocation.state = &state;
  invocation.effects = &f.effects;
  const auto result = react::run_handler(f.vm, {script, "novel.cue", 6}, invocation);
  require(result.status == react::RunOutcome::Status::Ok && f.gives_to.get(f.a.slot()) == nullptr,
          "withdrawing an already retired contribution failed or changed a reused subject");
}
void model_replacement_updates_allocation_priority() {
  test_runtime::Fixture f;
  f.terms.rows.resize(13);
  for (TermId id : {10u, 11u, 12u}) {
    auto &term = f.terms.rows[id];
    term.id = id;
    term.name = id == 10 ? "novel.supply" : id == 11 ? "novel.demand" : "novel.priority";
    f.terms.by_name[term.name] = id;
  }
  f.terms.rows[10].combine = react::Combine::Sum;
  f.terms.rows[11].combine = react::Combine::SharePriority;
  f.terms.rows[11].paired = 10;
  f.terms.rows[11].priority = 12;
  f.terms.rows[12].source = react::Source::Setting;
  const auto consumer = f.add(R"(return {kind='instance', contributes={'novel.demand'},
    on={['novel.cue']=function(self)end}})");
  for (auto object : {f.a, f.b}) {
    react::set_instances(f.installed, object, std::span(&consumer, 1), f.scripts);
    react::project_instance(f.triggers, f.installed, f.scripts, f.terms, object);
  }
  f.models.rows.resize(3);
  f.models.rows[2].id = 2;
  f.model_settings.rows[1][12] = Value(1.);
  f.model_settings.rows[2][12] = Value(5.);
  f.instance_settings.columns.resize(13);
  f.instance_settings.columns[12].set(f.b.slot(), Value(3.));
  react::set_contribution(f.shared, f.gives_to, f.terms.rows[10], f.c, f.c, 1, f.effects);
  for (auto object : {f.a, f.b})
    react::set_contribution(f.shared, f.gives_to, f.terms.rows[11], object, f.c, 1, f.effects);
  react::reconcile_allocations(f.values(), f.react_state, f.effects, 12, 20);
  require(react::get_share(f.allocations, f.c, 11, f.b) == 1,
          "the instance override initially outranks the first model default");
  perm::State permissions;
  lifecycle::Parts parts;
  Relation declared;
  const auto replace = [&](ModelId model) {
    const auto plan =
        lifecycle::plan_replace(f.models, parts, identity(f.objects), transforms(f.objects),
                                f.objects.params, f.a, {model, 0, 8, {}});
    lifecycle::apply_replacement(f.objects, parts, declared, plan, 1, 12);
    step::NativeCompletion completed;
    completed.rebound_objects = plan.retained;
    step::editing::rebind_scripts(f.catalog_state, f.react_state, f.values(), permissions, 6,
                                  {12, 20}, {}, completed);
  };
  replace(2);
  require(react::get_share(f.allocations, f.c, 11, f.a) == 1 &&
              react::get_share(f.allocations, f.c, 11, f.b) == 0,
          "replacing the model immediately uses its higher default priority");
  replace(1);
  require(react::get_share(f.allocations, f.c, 11, f.a) == 0 &&
              react::get_share(f.allocations, f.c, 11, f.b) == 1,
          "replacing back restores the original allocation without another setting write");
}
void model_rebind_retires_only_removed_installations() {
  test_runtime::Fixture f(1000);
  f.terms.rows[2].combine = react::Combine::Sum;
  const auto old = f.add(R"(return {kind='model',watches={'novel.heat'},
    contributes={'novel.signal'},on={['novel.heat']=function(self)end}})");
  const auto keep = f.add(R"(return {kind='instance',watches={'novel.heat'},
    contributes={'novel.result'},on={['novel.heat']=function(self)end}})");
  const auto replacement = f.add(R"(return {kind='model',on={
    ['novel.cue']=function(self) self.state.started=true end}})");
  f.models.rows[1].script = old;
  f.models.rows.resize(4);
  f.models.rows[2].id = 2;
  f.models.rows[2].script = replacement;
  f.models.rows[3].id = 3;
  f.models.rows[3].script = old;
  const std::array extra{keep};
  react::set_instances(f.installed, f.a, extra, f.scripts);
  react::project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  react::project_model(f.triggers, f.models, f.scripts, f.terms, 2);
  react::project_model(f.triggers, f.models, f.scripts, f.terms, 3);
  react::project_instance(f.triggers, f.installed, f.scripts, f.terms, f.a);
  f.baseline(f.a);
  f.states[{f.a, old}].insert(std::string("retired"), true);
  f.states[{f.a, keep}].insert(std::string("saved"), std::string("surviving state"));
  react::set_contribution(f.shared, f.gives_to, f.terms.rows[4], f.a, f.b, 10, f.effects);
  react::set_contribution(f.shared, f.gives_to, f.terms.rows[2], f.a, f.b, 7, f.effects);
  amber::perm::State perm_state;
  perm::Players &players = perm_state.players;
  players.rows.resize(2);
  perm::Grants &grants = perm_state.grants;
  f.objects.owner[f.a.slot()] = f.objects.owner[f.b.slot()] = f.objects.owner[f.c.slot()] = 1;
  const auto grant = [&](ScriptId script, Handle installation) {
    const auto holder = perm::Holder::script(script, installation);
    const auto scope = perm::Scope::object(f.b);
    const auto result =
        perm::create_grant(grants, players, perm::Holder::player(1),
                           {1, holder, scope, perm::bit(perm::Verb::Transfer), 0, 0, 1000},
                           {scope, 1, perm::all_rights, 0, false}, {holder, 1, true}, 0);
    require(bool(result), "actual explicit ScriptHolder grant created");
    return result.grant;
  };
  const auto old_grant = grant(old, f.a), kept_grant = grant(keep, f.a);
  lifecycle::Parts parts;
  Relation declared;
  const auto replace = [&](Handle root, ModelId model = 2, TermId material = 8) {
    const auto plan =
        lifecycle::plan_replace(f.models, parts, identity(f.objects), transforms(f.objects),
                                f.objects.params, root, {model, 0, material, {}});
    lifecycle::apply_replacement(f.objects, parts, declared, plan, 1, 12);
    step::NativeCompletion completed;
    completed.rebound_objects = plan.retained;
    step::editing::rebind_scripts(f.catalog_state, f.react_state, f.values(), perm_state, 6,
                                  amber::step::Clock{.step = 12, .hz = 20}, {}, completed);
    return std::pair{completed.permissions_changed, std::move(completed.effects)};
  };
  const auto [changed, effects] = replace(f.a);
  require(changed && !f.states.contains({f.a, old}) &&
              std::get<std::string>(f.states.at({f.a, keep}).values.at(std::string("saved"))) ==
                  "surviving state",
          "changing the same root model retires removed state but retains explicit script state");
  require(!grants.rows[old_grant].live && grants.rows[kept_grant].live,
          "only removed ScriptHolder authority is revoked");
  const auto target = perm::target(identity(f.objects), authority(f.objects), {}, {}, f.b);
  require(!perm::check(perm::Holder::script(old, f.a), perm::Verb::Transfer, target, players,
                       grants, 12) &&
              bool(perm::check(perm::Holder::script(keep, f.a), perm::Verb::Transfer, target,
                               players, grants, 12)),
          "the retired script actually loses its former authority");
  f.state(f.a, 1, 4);
  const auto watching = f.changed({f.a, 1});
  require(f.shared[4].get(f.b.slot())->parts.empty() && f.shared[2].get(f.b.slot())->total == 7 &&
              watching.size() == 1 && watching[0].handler.script == keep,
          "removed behavior withdraws contributions and only the surviving watch responds");
  std::size_t starts = 0;
  bool withdrew = false;
  for (const auto &effect : effects.ordered) {
    if (const auto *event = std::get_if<react::EventWork>(&effect)) {
      ++starts;
      require(event->recipient_script == replacement,
              "existing scripts must not receive a replacement startup replay");
    }
    if (const auto *change = std::get_if<react::ContributionChange>(&effect))
      withdrew |= change->source == FactKey{f.b, 4};
  }
  require(starts == 1 && withdrew, "new default starts once and withdrawal uses normal effects");

  // An old default that remains explicitly installed is still live behavior.
  const std::array retained_default{old, keep};
  react::set_instances(f.installed, f.c, retained_default, f.scripts);
  react::project_instance(f.triggers, f.installed, f.scripts, f.terms, f.c);
  f.states[{f.c, old}].insert(std::string("saved"), true);
  const auto explicit_grant = grant(old, f.c);
  react::set_contribution(f.shared, f.gives_to, f.terms.rows[4], f.c, f.b, 3, f.effects);
  const auto explicit_result = replace(f.c);
  require(
      !explicit_result.first && grants.rows[explicit_grant].live &&
          std::get<bool>(f.states.at({f.c, old}).values.at(std::string("saved"))) &&
          f.shared[4].get(f.b.slot())->total == 3,
      "an old default surviving as an explicit installation keeps state, grants and contribution");
  ObjectInit init;
  init.model = 1;
  init.material = 8;
  init.owner = 1;
  const auto same_default = create(f.objects, init);
  f.states[{same_default, old}].insert(std::string("saved"), true);
  const auto model_grant = grant(old, same_default);
  react::set_contribution(f.shared, f.gives_to, f.terms.rows[4], same_default, f.b, 5, f.effects);
  const auto model_result = replace(same_default, 3);
  require(
      !model_result.first && grants.rows[model_grant].live &&
          std::get<bool>(f.states.at({same_default, old}).values.at(std::string("saved"))) &&
          f.shared[4].get(f.b.slot())->total == 8,
      "changing models retains an unchanged default script and its state, grant and contribution");
  const auto rule = f.add(R"(return {kind='rule',applies_to={material='novel.other'},
    contributes={'novel.signal'},on={['novel.cue']=function(self)end},
    tests={{when={steps=0},expect={subject='target',term='novel.signal',equals=0}}}})");
  react::add_rule(f.rules, f.scripts, f.terms, rule);
  react::project_rules(f.triggers, f.rules);
  init.material = 9;
  const auto with_rule = create(f.objects, init);
  f.states[{with_rule, old}].insert(std::string("retired"), true);
  const auto removed_grant = grant(old, with_rule);
  react::set_contribution(f.shared, f.gives_to, f.terms.rows[4], with_rule, f.b, 5, f.effects);
  const auto rule_result = replace(with_rule, 2, 9);
  require(rule_result.first && !grants.rows[removed_grant].live &&
              !f.states.contains({with_rule, old}) && f.shared[4].get(f.b.slot())->total == 13,
          "an applicable material rule preserves a supported contribution while removed model "
          "authority retires");
}
} // namespace
int main() {
  try {
    stored_scripts_keep_data_and_restart_without_a_saved_execution();
    live_effects_and_causal_finalization();
    persistence_and_activation();
    world_declarations_reject_object_requirements();
    world_installations_share_the_runtime_without_host_objects();
    withdrawing_from_retired_subject_is_idempotent();
    model_replacement_updates_allocation_priority();
    model_rebind_retires_only_removed_installations();
    std::cout << "Actual VM execution, live effects and causal finalization tests passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
