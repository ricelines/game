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
  react::Subscriptions &subscriptions = react_state.subscriptions;
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
  const auto draw = std::get<double>(f.states.at({f.object, 2}).at(std::string("last")));
  f.run(2, 4);
  require(std::get<double>(f.states.at({f.object, 2}).at(std::string("count"))) == 2 &&
              std::get<double>(f.states.at({f.object, 2}).at(std::string("last"))) != draw,
          "scalar state and RNG continue across invocations");
  f.run(2, 4, f.other);
  require(std::get<double>(f.states.at({f.other, 2}).at(std::string("count"))) == 1,
          "another installation has its own persisted scalar state");
  react::set_activation(f.vm, 2, false, false);
  const auto disabled = f.run(2, 4);
  require(disabled.outcome.status == react::RunOutcome::Status::Missing &&
              std::get<double>(f.states.at({f.object, 2}).at(std::string("count"))) == 2,
          "inactive cached script cannot execute even with an old handler reference");
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
  f.bind(old, f.a);
  f.bind(keep, f.a);
  f.states[{f.a, old}][std::string("retired")] = true;
  f.states[{f.a, keep}][std::string("saved")] = std::string("surviving state");
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
              std::get<std::string>(f.states.at({f.a, keep}).at(std::string("saved"))) ==
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
  require(f.shared[4].get(f.b.slot())->parts.empty() && f.shared[2].get(f.b.slot())->total == 7 &&
              react::watch_sources(f.subscriptions, ScriptKey{f.a, old}).empty() &&
              !react::watch_sources(f.subscriptions, ScriptKey{f.a, keep}).empty(),
          "removed behavior withdraws contributions and watches without disturbing survivors");
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
  f.states[{f.c, old}][std::string("saved")] = true;
  const auto explicit_grant = grant(old, f.c);
  react::set_contribution(f.shared, f.gives_to, f.terms.rows[4], f.c, f.b, 3, f.effects);
  const auto explicit_result = replace(f.c);
  require(
      !explicit_result.first && grants.rows[explicit_grant].live &&
          std::get<bool>(f.states.at({f.c, old}).at(std::string("saved"))) &&
          f.shared[4].get(f.b.slot())->total == 3,
      "an old default surviving as an explicit installation keeps state, grants and contribution");
  ObjectInit init;
  init.model = 1;
  init.material = 8;
  init.owner = 1;
  const auto same_default = create(f.objects, init);
  f.states[{same_default, old}][std::string("saved")] = true;
  const auto model_grant = grant(old, same_default);
  react::set_contribution(f.shared, f.gives_to, f.terms.rows[4], same_default, f.b, 5, f.effects);
  const auto model_result = replace(same_default, 3);
  require(
      !model_result.first && grants.rows[model_grant].live &&
          std::get<bool>(f.states.at({same_default, old}).at(std::string("saved"))) &&
          f.shared[4].get(f.b.slot())->total == 8,
      "changing models retains an unchanged default script and its state, grant and contribution");
  const auto rule = f.add(R"(return {kind='rule',applies_to={material='novel.other'},
    contributes={'novel.signal'},on={['novel.cue']=function(self)end},
    tests={{when={steps=0},expect={subject='target',term='novel.signal',equals=0}}}})");
  react::add_rule(f.rules, f.scripts, f.terms, rule);
  react::project_rules(f.triggers, f.rules);
  init.material = 9;
  const auto with_rule = create(f.objects, init);
  f.states[{with_rule, old}][std::string("retired")] = true;
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
    live_effects_and_causal_finalization();
    persistence_and_activation();
    model_rebind_retires_only_removed_installations();
    std::cout << "Actual VM execution, live effects and causal finalization tests passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
