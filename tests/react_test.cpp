#include "catalog/catalog.hpp"
#include "react/react.hpp"
#include "runtime_fixture.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace amber;
using namespace amber::react;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
void near(double a, double b, const char *message) {
  require(std::abs(a - b) < 1e-8, message);
}
Term term(TermId id, const char *name, Source source = Source::State,
          Combine combine = Combine::None) {
  Term t;
  t.id = id;
  t.name = name;
  t.source = source;
  t.combine = combine;
  return t;
}
struct Fixture {
  amber::ObjectState objects_state;
  amber::react::State react_state{{4096, 50, 5, 64}};
  amber::catalog::State catalog_state;

  Objects &objects = objects_state.objects;
  Handle a, b, c;
  Terms &terms = react_state.terms;
  Materials &materials = react_state.materials;
  AssetMeasurements &assets = catalog_state.baked_measurements;
  Properties &properties = react_state.properties;
  Processes &processes = react_state.processes;
  InstanceSettings &instance = react_state.instance_settings;
  ModelSettings &models = react_state.model_settings;
  PlotSettings &plots = react_state.plot_settings;
  WorldSettings &world = react_state.world_settings;
  SharedColumns &shared = react_state.shared;
  RelationsRead &relations = react_state.relations;
  RuntimeMeasurements &runtime = react_state.runtime_measurements;
  Fixture() {
    ObjectInit init;
    init.model = 1;
    init.material = 10;
    init.plot = 1;
    a = create(objects, init);
    b = create(objects, init);
    c = create(objects, init);
    catalog_state.mechanical.values["measurements"] = {{"density", 1}};
    terms.rows.resize(10);
    for (TermId i = 1; i < 10; ++i) {
      terms.rows[i] = term(i, ("x." + std::to_string(i)).c_str());
      terms.by_name[terms.rows[i].name] = i;
    }
  }
  ValueRead values() { return value_read(objects_state, catalog_state, react_state); }
};
void source_resolution() {
  Fixture f;
  f.terms.rows[1].source = Source::Intrinsic;
  f.materials.rows[10][1] = Value(2000.0);
  f.terms.rows[2].source = Source::Measured;
  f.terms.rows[2].measurement = MeasurementRole::Volume;
  f.terms.rows[3].source = Source::Measured;
  f.terms.rows[3].measurement = MeasurementRole::Mass;
  f.terms.rows[4].source = Source::Setting;
  f.world.values[4] = Value(1.0);
  f.plots.rows[1][4] = Value(2.0);
  f.models.rows[1][4] = Value(3.0);
  f.assets.rows[1].volume = 0.5;
  f.assets.parameter_rows[77].volume = 0.25;
  f.objects.params[f.b.slot()] = 77;
  near(read(f.values(), f.a, 3, 0, 20).value.numeric(), 1000,
       "mass from measured volume/current material density");
  near(read(f.values(), f.b, 3, 0, 20).value.numeric(), 500,
       "parameter measurement overrides model");
  near(read(f.values(), f.a, 4, 0, 20).value.numeric(), 3, "model setting overrides plot/world");
  f.instance.columns.resize(5);
  f.instance.columns[4].set(f.a.slot(), Value(0.0));
  near(read(f.values(), f.a, 4, 0, 20).value.numeric(), 0, "explicit zero instance setting");
  f.terms.rows[5].source = Source::Measured;
  f.terms.rows[5].measurement = MeasurementRole::Runtime;
  f.properties.resize(6);
  f.properties[5].set(f.a.slot(), Value(999.0));
  require(!read(f.values(), f.a, 5, 0, 20).present, "state cannot fake runtime measurement");
  DeferredEffects effects;
  apply_derived_fact(f.runtime, {{f.a, 5}, Value(25.0), {}}, effects);
  near(read(f.values(), f.a, 5, 0, 20).value.numeric(), 25, "derived runtime fact");
}
void owner_value_view_reads_bound_relations() {
  Fixture f;
  Relation physical;
  f.relations.columns[9] = &physical;
  f.terms.rows[9].kind = Kind::Relation;
  f.terms.rows[4].combine = Combine::Sum;
  f.terms.rows[4].along = 9;
  f.shared.resize(5);
  f.shared[4].set(f.b.slot(), Shared{7, {{f.b, 7}}});
  f.shared[4].set(f.c.slot(), Shared{11, {{f.c, 11}}});
  const auto values = value_read(f.objects_state, f.catalog_state, f.react_state);
  require(!read(values, f.a, 4, 0, 20).present,
          "an owner value view starts without an unbound physical edge");
  DeferredEffects effects;
  set_relation(physical, 9, f.a, f.b, true, effects);
  near(read(values, f.a, 4, 0, 20).value.numeric(), 7,
       "the view follows the prebound physical table without a physics world");
  set_relation(physical, 9, f.a, f.b, false, effects);
  set_relation(physical, 9, f.a, f.c, true, effects);
  near(read(values, f.a, 4, 0, 20).value.numeric(), 11,
       "the same view observes later relation changes without rebuilding or copying tables");
}
void process_thresholds_and_transfer() {
  Fixture f;
  DeferredEffects effects;
  auto &t = f.terms.rows[1];
  t.conserved = true;
  set_state(f.properties, f.processes, t, f.a, Value(10.0), 0, 20, effects);
  set_state(f.properties, f.processes, t, f.b, Value(1.0), 0, 20, effects);
  set_rate(f.react_state, t, f.a, -2, 0, 10, 0, 20, effects);
  require(!set_rate(f.react_state, t, f.a, -2, 0, 10, 0, 20, effects),
          "same effective process rate/bounds is unchanged");

  auto p = *f.processes[1].get(f.a.slot());
  const auto effect_count = effects.ordered.size();
  require(!set_rate(f.react_state, t, f.a, -2, 0, 10, 20, 20, effects) &&
              f.processes[1].get(f.a.slot())->step0 == p.step0 &&
              effects.ordered.size() == effect_count,
          "repeating an unchanged process later preserves its closed form and armed timers");
  near(process_value(p, 50, 20), 5, "closed form process");
  require(next_crossing(p, {Threshold::Predicate::Below, 5}, 0, 20) == 51,
          "falling strict threshold");
  require(next_crossing(p, {Threshold::Predicate::AtMost, 5}, 0, 20) == 50,
          "falling inclusive threshold");
  Process rising{0, 0, 2, 0, 10};
  require(next_crossing(rising, {Threshold::Predicate::Above, 5}, 0, 20) == 51,
          "rising strict threshold");
  require(next_crossing(rising, {Threshold::Predicate::AtLeast, 5}, 0, 20) == 50,
          "rising inclusive threshold");
  require(!next_crossing(rising, {Threshold::Predicate::Above, 10}, 0, 20),
          "clamped unreachable threshold");
  require(transfer_state(f.react_state, t, f.a, f.b, 2, 50, 20, effects), "transfer succeeds");
  near(read_state(f.properties, f.processes, f.a, 1, 50, 20).value.numeric(), 3,
       "settled source debit");
  near(read_state(f.properties, f.processes, f.b, 1, 50, 20).value.numeric(), 3,
       "equal destination credit");
  set_rate(f.react_state, t, f.c, -1, 0, 10, 50, 20, effects);
  auto current = read_state(f.properties, f.processes, f.c, 1, 50, 20).value;
  require(set_state(f.properties, f.processes, t, f.c, current, 50, 20, effects),
          "stopping current nonzero process rate counts as change");
  require(f.processes[1].get(f.a.slot())->rate == -2,
          "transfer preserves ongoing source rate/bounds");
  near(read_state(f.properties, f.processes, f.a, 1, 60, 20).value.numeric(), 2,
       "source process continues after conserved transfer");
  set_rate(f.react_state, t, f.b, 0.5, 0, 4, 50, 20, effects);
  const auto source_before = *f.processes[1].get(f.a.slot());
  const auto destination_before = *f.processes[1].get(f.b.slot());
  bool bounded = false;
  try {
    transfer_state(f.react_state, t, f.a, f.b, 2, 50, 20, effects);
  } catch (const std::invalid_argument &) {
    bounded = true;
  }
  require(bounded, "destination process bound failure is explicit");
  const auto unchanged = [](const Process &a, const Process &b) {
    return a.v0 == b.v0 && a.step0 == b.step0 && a.rate == b.rate && a.lo == b.lo && a.hi == b.hi;
  };
  require(unchanged(*f.processes[1].get(f.a.slot()), source_before) &&
              unchanged(*f.processes[1].get(f.b.slot()), destination_before),
          "failed process-bound transfer changes neither endpoint");
  require(transfer_state(f.react_state, t, f.a, f.b, 0.5, 50, 20, effects),
          "bounded transfer into running destination");
  near(read_state(f.properties, f.processes, f.b, 1, 60, 20).value.numeric(), 3.75,
       "destination process continues after transfer");

  require(!transfer_state(f.react_state, t, f.a, f.b, 4, 50, 20, effects),
          "insufficient transfer no mutation");
  near(read_state(f.properties, f.processes, f.a, 1, 50, 20).value.numeric(), 2.5,
       "failed transfer retains value");
  Subscriptions subs;
  bind_watch(subs, {f.a, 1},
             {{f.b, 1}, 0, Answer{true, Value(3.0)}, {1, {{Threshold::Predicate::Below, 2}}}, {}});
  const react::ThresholdToken token{{f.a, 1}, f.b, 1, 0};
  require(valid_threshold(token, identity(f.objects), f.processes, subs, 60, 20),
          "current process actually crosses the subscribed predicate");
  require(!valid_threshold(token, identity(f.objects), f.processes, subs, 50, 20),
          "a timer cannot wake a predicate that has not crossed");
  CurrentAnswers answers;
  answers.values[{f.a, 1}] = Answer{true, Value(3.0)};
  require(check_watch(subs, {f.a, 1}, answers).empty(), "change back is silent");
  answers.values[{f.a, 1}] = Answer{true, Value(1.0)};
  require(check_watch(subs, {f.a, 1}, answers).size() == 1, "threshold delivery");
}
void subscription_rebind_and_relations() {
  Fixture f;
  Subscriptions subscriptions;
  const ScriptKey consumer{f.b, 7};
  const FactKey source{f.a, 1};
  bind_watch(subscriptions, source, {consumer, 0, Answer{}, {1, {}}, {}});
  unbind_watch(subscriptions, consumer, 0);
  bind_watch(subscriptions, source, {consumer, 0, Answer{}, {1, {}}, {}});
  bind_watch(subscriptions, {f.c, 1}, {consumer, 1, Answer{}, {1, {}}, {}});
  require(watch_sources(subscriptions, consumer).size() == 2,
          "consumer lookup reads the primary source rows");
  unbind_watch(subscriptions, consumer, 0);
  require(!subscriptions.rows.contains(source) && subscriptions.rows.contains({f.c, 1}),
          "unbind touches selected watch only");
  retire_subscriptions(subscriptions, f.b);
  require(subscriptions.rows.empty(), "consumer retirement removes its primary watch rows");
  Relation relation;
  DeferredEffects effects;
  bind_watch(subscriptions, {f.a, 2}, {{f.a, 8}, 0, RelationAnswer{}, {2, {}}, {}});
  set_relation(relation, 2, f.b, f.a, true, effects);
  CurrentAnswers answers;
  answers.values[{f.a, 2}] = read_relation(relation, f.a);
  auto deliveries = check_watch(subscriptions, {f.a, 2}, answers);
  require(deliveries.size() == 1 && deliveries[0].relations.size() == 1 &&
              deliveries[0].relations[0].other == f.b && deliveries[0].relations[0].added &&
              deliveries[0].relations[0].incoming,
          "relation watch carries other endpoint and direction");
  acknowledge_watch(subscriptions, {f.a, 8}, {{f.a, 2}, 0, deliveries[0].value});
  set_relation(relation, 2, f.b, f.a, false, effects);
  answers.values[{f.a, 2}] = read_relation(relation, f.a);
  deliveries = check_watch(subscriptions, {f.a, 2}, answers);
  require(deliveries.size() == 1 && !deliveries[0].relations[0].added,
          "empty relation is delivered after removal");
  retire_subscriptions(subscriptions, f.a);
  require(subscriptions.rows.empty(), "source retirement clears self subscription");
}
void allocation_rows_track_current_nonzero_shares_only() {
  Fixture f;
  amber::react::State react_state{{4096, 50, 5, 64}};
  AllocationRows &allocations = react_state.allocations;
  MeterRows &meters = react_state.meters;
  const Shared supply{2, {{f.a, 2}}};
  const Shared empty;
  PriorityValues priority;
  for (unsigned i = 0; i < 256; ++i) {
    ObjectInit init;
    init.model = 1;
    const auto consumer = create(f.objects, init);
    const Shared demand{1, {{consumer, 1}}};
    allocate(supply, demand, Combine::ShareEqual, priority, f.b, 1, 2, react_state, i * 2, 20);
    require(allocations.values.size() == 2,
            "current allocation rows grew with withdrawn contributors");
  }
  allocate(empty, empty, Combine::ShareEqual, priority, f.b, 1, 2, react_state, 512, 20);
  require(allocations.values.empty() && !meters.empty(),
          "zero shares retained current-state rows or discarded earned integrals");
  retire_allocations(allocations, meters, f.b);
  require(meters.empty(), "retiring a fully withdrawn subject retained historical meters");
  const Shared demand{1, {{f.c, 1}}};
  allocate(empty, demand, Combine::ShareEqual, priority, f.b, 1, 2, react_state, 513, 20);
  require(allocations.values.empty() && meters.empty(),
          "never-served zero demand allocated redundant rows and meters");
}

void contribution_and_metering() {
  Fixture f;
  DeferredEffects effects;
  amber::react::State react_state{{4096, 50, 5, 64}};
  GivesTo &reverse = react_state.gives_to;
  auto &t = f.terms.rows[1];
  t.combine = Combine::Sum;
  set_contribution(f.shared, reverse, t, f.b, f.a, 5, effects);
  set_contribution(f.shared, reverse, t, f.c, f.a, 7, effects);
  require(!set_contribution(f.shared, reverse, t, f.a, f.a, 0, effects),
          "withdrawing absent contributor is a no-op while others contribute");

  near(f.shared[1].get(f.a.slot())->total, 12, "sum contribution");
  withdraw_contributor(f.shared, reverse, f.terms, f.b, effects);
  near(f.shared[1].get(f.a.slot())->total, 7, "reverse-index withdrawal");
  Relation relation;
  set_relation(relation, 2, f.b, f.a, true, effects);
  f.relations.columns[2] = &relation;
  t.along = 2;
  near(read_shared(f.shared, f.relations, f.b, t).value.numeric(), 7, "along relation fallback");
  AllocationRows &allocations = react_state.allocations;
  MeterRows &meters = react_state.meters;
  PriorityValues priorities;
  Shared supply{100, {{f.a, 100}}}, demand{30, {{f.b, 10}, {f.c, 20}}};
  allocate(supply, demand, Combine::ShareEqual, priorities, f.a, 1, 2, react_state, 0, 20);
  near(get_share(allocations, f.a, 2, f.b), 10, "satisfied small demand");
  near(get_share(allocations, f.a, 2, f.c), 20, "satisfied larger demand");
  near(metered(meters.at({f.a, f.a, 1, true}), 20, 20), 30,
       "supplier meters actual served rather than offered");
  demand = {0, {}};
  allocate(supply, demand, Combine::ShareEqual, priorities, f.a, 1, 2, react_state, 20, 20);
  near(metered(meters.at({f.a, f.a, 1, true}), 40, 20), 30, "unused supply not metered");
  supply = {12, {{f.a, 12}}};
  demand = {25, {{f.b, 5}, {f.c, 20}}};
  allocate(supply, demand, Combine::ShareEqual, priorities, f.a, 1, 2, react_state, 40, 20);
  near(get_share(allocations, f.a, 2, f.b), 5, "waterfill caps small demand");
  near(get_share(allocations, f.a, 2, f.c), 7, "waterfill redistributes surplus");
}
void dispatch_order() {
  Fixture f;
  Triggers triggers;
  auto &row = triggers[1];
  row.by_material[10] = {1, "material"};
  row.by_flag.push_back({2, {2, "flag"}});
  row.by_model[1] = {3, "model"};
  row.by_instance[f.a.bits] = {{4, "instance"}};
  const std::array<TermId, 1> flags{2};
  auto handlers = dispatch(triggers, identity(f.objects), flags, f.a, 1);
  require(handlers.size() == 4 && handlers[0].script == 1 && handlers[3].script == 4,
          "dispatch material flag model instance order");
}
RunOutcome execute(Vm &vm, ScriptState &state, DeferredEffects &effects, Handle object, ScriptId id,
                   std::string function = "run", int ms = 50) {
  std::uint64_t rng = 7;
  Invocation inv;
  inv.self = {object, id};
  inv.state = &state;
  inv.random = &rng;
  inv.effects = &effects;
  inv.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  return run_handler(vm, {id, std::move(function)}, inv);
}
void action_handlers_receive_the_actual_caller() {
  test_runtime::Fixture f(1000);
  const auto script = f.add(R"(return {kind='model',on={
    ['novel.cue']=function(self) self:act('novel.touch',self.handle,{}) end,
    ['novel.touch']=function(self,sender,args,caller)
      if type(caller)=='string' then assert(caller=='p:2');self.state.kind='player'
      elseif caller then
        assert(caller.script=='s:'..self.script and caller.on==sender)
        assert(not pcall(function()caller.script='s:999'end))
        self.state.kind='script'
      else self.state.kind='event' end
    end}})");
  ScriptState state, parent_state;
  DeferredEffects effects;
  std::uint64_t random = 7;
  Invocation child;
  child.self = {f.a, script};
  child.sender = f.a;
  child.state = &state;
  child.effects = &effects;
  child.random = &random;
  child.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  child.action_caller = PlayerId{2};
  require(run_handler(f.vm, {script, "novel.touch"}, child).status == RunOutcome::Status::Ok &&
              std::get<std::string>(state.at(std::string("kind"))) == "player",
          "player action exposes its actual player caller");
  child.action_caller.reset();
  require(run_handler(f.vm, {script, "novel.touch"}, child).status == RunOutcome::Status::Ok &&
              std::get<std::string>(state.at(std::string("kind"))) == "event",
          "events have no action caller");
  Invocation parent = child;
  parent.self = {f.a, script};
  parent.state = &parent_state;
  const auto action = run_handler(f.vm, {script, "novel.cue"}, parent);
  require(action.status == RunOutcome::Status::Action && action.action,
          "real self.act yields its script caller");
  child.action_caller = action.action->request.caller;
  require(run_handler(f.vm, {script, "novel.touch"}, child).status == RunOutcome::Status::Ok &&
              std::get<std::string>(state.at(std::string("kind"))) == "script",
          "self.act exposes the script and its installed object as readonly caller metadata");
  require(resume_handler(f.vm, parent, {{"ok", true}}).status == RunOutcome::Status::Ok,
          "caller metadata does not alter action suspension and return");
}

void luau_state_and_limits() {
  const auto object = Handle::make(7, 9000001);
  Vm vm({4096, 50, 5, 64});
  ScriptDeclaration d;
  d.id = 1;
  auto compiled = compile_script(R"(
    return {on={run=function(self)
      self.state[1] = 12; self.state['1'] = 'separate'; self.state.owner=self.handle
      self.state.keep=true; self.state.remove=8; self.state.remove=nil
      local count=0; for k,v in self.state do count+=1 end; self.state.count=count
      error('after writes')
    end}}
  )");
  require(compiled.errors.empty(), "valid Luau compilation");
  require(load_script(vm, compiled, d).status == RunOutcome::Status::Ok, "load sandboxed module");
  ScriptState state;
  DeferredEffects effects;
  auto result = execute(vm, state, effects, object, 1);
  require(result.status == RunOutcome::Status::Error, "handler error");
  near(std::get<double>(state.at(StateKey{std::int64_t{1}})), 12, "integer state key retained");
  require(std::get<std::string>(state.at(StateKey{std::string("1")})) == "separate",
          "string key distinct");
  require(std::get<Handle>(state.at(StateKey{std::string("owner")})) == object,
          "handle exact bits");
  require(!state.contains(StateKey{std::string("remove")}), "nil deletes state");
  near(std::get<double>(state.at(StateKey{std::string("count")})), 4,
       "real userdata generic iteration");
  require(effects.changed, "state writes counted before error");
  require(!compile_script("local n=0; return {on={run=function(self) n+=1 end}}").errors.empty(),
          "captured scalar mutation rejected");
  require(!compile_script("local n=0; return {on={run=function(self) n=1 end}}").errors.empty(),
          "ordinary captured assignment rejected");
  Vm frozen({4096, 50, 5, 64});
  d.id = 2;
  auto capture = compile_script(
      "local hidden={n=0}; return {on={run=function(self) self.state.before=1; hidden.n+=1 end}}");
  require(load_script(frozen, capture, d).status == RunOutcome::Status::Ok,
          "captured table frozen at load");
  ScriptState fs;
  DeferredEffects fe;
  require(execute(frozen, fs, fe, object, 2).status == RunOutcome::Status::Error,
          "captured table mutation fails");
  require(fs.contains(StateKey{std::string("before")}), "write before frozen-table failure stays");
  Vm cutoff({4096, 50, 5, 64});
  d.id = 3;
  auto spin =
      compile_script("return {on={run=function(self) self.state.before=1; while true do end end}}");
  require(load_script(cutoff, spin, d).status == RunOutcome::Status::Ok, "load infinite handler");
  ScriptState cs;
  DeferredEffects ce;
  require(execute(cutoff, cs, ce, object, 3, "run", 2).status == RunOutcome::Status::Cutoff,
          "VM interrupt deadline");
  require(cs.contains(StateKey{std::string("before")}), "write before cutoff stays");
  Vm nested({4096, 50, 5, 64});
  d.id = 4;
  auto invalid =
      compile_script("return {on={run=function(self) self.state.before=1; self.state.bad={} end}}");
  require(load_script(nested, invalid, d).status == RunOutcome::Status::Ok,
          "load bad state writer");
  ScriptState ns;
  DeferredEffects ne;
  require(execute(nested, ns, ne, object, 4).status == RunOutcome::Status::Error,
          "nested state rejected");
  require(ns.size() == 1, "no rollback or hidden nested state");
  Vm memory({256, 50, 5, 64});
  d.id = 5;
  auto large = compile_script("return {on={run=function(self) self.state.before=1; for i=1,10000 "
                              "do self.state[i]=string.rep('x',100) end end}}");
  require(load_script(memory, large, d).status == RunOutcome::Status::Ok, "load memory test");
  ScriptState ms;
  DeferredEffects me;
  auto limited = execute(memory, ms, me, object, 5);
  require(limited.status == RunOutcome::Status::Error && limited.memory_bytes <= 256 * 1024,
          "combined Lua/state memory limit enforced");
  require(ms.contains(StateKey{std::string("before")}) && ms.size() > 1,
          "partial writes survive memory limit");
  DeferredEffects repeat_effects;
  require(execute(memory, ms, repeat_effects, object, 5).status == RunOutcome::Status::Error,
          "repeated memory failure remains protected during VM call setup");

  Vm iterators({256, 50, 5, 64});
  d.id = 6;
  auto nested_iteration = compile_script(R"(return {on={run=function(self)
    for i=1,400 do self.state[i]=i end
    local function nested(n)
      for k,v in self.state do if n>0 then nested(n-1) end; break end
    end
    nested(100)
  end}})");
  require(load_script(iterators, nested_iteration, d).status == RunOutcome::Status::Ok,
          "load retained iterator test");
  ScriptState is;
  DeferredEffects ie;
  auto iter_result = execute(iterators, is, ie, object, 6);
  require(iter_result.status == RunOutcome::Status::Error && iter_result.memory_bytes <= 256 * 1024,
          "retained iterator snapshots count against script budget");
}
void live_activation_and_dispatch_authority() {
  Vm vm({4096, 50, 5, 64});
  auto compiled = compile_script("return {on={run=function(self) self.state.ran=true end}}");
  ScriptDeclaration declaration;
  declaration.id = 11;
  declaration.kind = ScriptDeclaration::Kind::Rule;
  declaration.approved = false;
  declaration.active = false;
  require(load_script(vm, compiled, declaration).status == RunOutcome::Status::Ok,
          "load inactive rule closure");
  ScriptState state;
  DeferredEffects effects;
  std::uint64_t rng = 1;
  Invocation inv;
  inv.self = {Handle::make(1, 1), 11};
  inv.state = &state;
  inv.random = &rng;
  inv.effects = &effects;
  HandlerRef handler{11, "run", 1, 1, true};
  require(run_handler(vm, handler, inv).status == RunOutcome::Status::Missing && state.empty(),
          "inactive rule does not run");
  set_activation(vm, 11, true, true);
  inv.deadline = {};
  require(run_handler(vm, handler, inv).status == RunOutcome::Status::Ok &&
              std::get<bool>(state.at(std::string("ran"))),
          "activation updates the cached closure declaration");
  state.clear();
  set_activation(vm, 11, true, false);
  inv.deadline = {};
  require(run_handler(vm, handler, inv).status == RunOutcome::Status::Missing && state.empty(),
          "deactivation takes effect without reloading the closure");
}
void luau_bound_values_and_events() {
  Fixture f;
  auto projection = f.values();
  Vm vm({4096, 50, 5, 64});
  bind_values(vm, projection, f.react_state);
  bind_events(vm, f.terms);
  f.terms.rows[2].kind = Kind::Event;
  f.terms.rows[3].kind = Kind::Action;
  auto compiled = compile_script(R"(return {on={run=function(self)
    self:set('x.1',3); self:rate('x.1',-1,0,3)
    self:emit(self.handle,'x.2',{target=self.handle,amount=2})
    self:after(0,'x.2',true)
    local result=self:act('x.3',self.handle,{amount=1})
    assert(result.ok and result.effect=='applied' and result.created[1]==self.handle)
    self.state.observed=self:get('x.1')
    self:after(0.1,'x.2')
    self:emit(self.handle,'x.2')
  end}})");
  ScriptDeclaration d;
  d.id = 1;
  d.sets = {1};
  require(load_script(vm, compiled, d).status == RunOutcome::Status::Ok, "load bindings script");
  ScriptState state;
  DeferredEffects effects;
  std::uint64_t rng = 7;
  Invocation bound_invocation;
  bound_invocation.self = {f.a, 1};
  bound_invocation.state = &state;
  bound_invocation.random = &rng;
  bound_invocation.effects = &effects;
  bound_invocation.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  auto result = run_handler(vm, {1, "run"}, bound_invocation);
  require(result.status == RunOutcome::Status::Action && result.action &&
              std::get<ScriptHolder>(result.action->request.caller) == ScriptHolder{1, f.a} &&
              result.action->arguments == nlohmann::json{{"amount", 1}},
          "act suspends at its call site as the installed script holder");
  require(!state.contains(StateKey{std::string("observed")}),
          "Lua statements after act wait for the actual action result");
  result = resume_handler(vm, bound_invocation,
                          {{"ok", true}, {"effect", "applied"}, {"created", {to_string(f.a)}}});
  if (result.status != RunOutcome::Status::Ok)
    throw std::runtime_error(result.error);
  near(std::get<double>(state.at(StateKey{std::string("observed")})), 3,
       "bindings read immediate changes");
  require(effects.ordered.size() == 6, "writes, events and timers survive action suspension");
  auto &timer = std::get<react::Timer>(effects.ordered[3]);
  require(timer.due == 1, "after zero next step");
  const auto &empty_timer = std::get<react::Timer>(effects.ordered[4]);
  const auto &empty_event = std::get<react::EventWork>(effects.ordered[5]);
  require(empty_timer.due == 2 && !std::get<Value>(empty_timer.event.payload).present() &&
              !std::get<Value>(empty_event.payload).present(),
          "omitted optional timer and event payloads are ordinary absent scalars");
  Vm foreign({4096, 50, 5, 64});
  bind_values(foreign, projection, f.react_state);
  ScriptDeclaration foreign_declaration;
  foreign_declaration.id = 3;
  auto foreign_code = compile_script(R"(return {on={run=function(self, sender, target)
    self.state.foreign=self:get('x.1',target)
  end,read_foreign=function(self, sender, target)
    self.state.attempted=true
    self.state.copied=self:read(target,'x.1')
    self.state.returned=true
  end}})");
  require(load_script(foreign, foreign_code, foreign_declaration).status == RunOutcome::Status::Ok,
          "load own-value signature check");
  f.properties[1].set(f.b.slot(), Value(7.0));
  ScriptState foreign_state;
  DeferredEffects foreign_effects;
  std::uint64_t foreign_rng = 0;
  Invocation foreign_invocation;
  foreign_invocation.self = {f.a, 3};
  foreign_invocation.payload = Value(f.b);
  foreign_invocation.state = &foreign_state;
  foreign_invocation.random = &foreign_rng;
  foreign_invocation.effects = &foreign_effects;
  require(run_handler(foreign, {3, "run"}, foreign_invocation).status == RunOutcome::Status::Error,
          "get rejects a foreign target argument");
  foreign_invocation.acquired = {f.b};
  foreign_invocation.deadline = {};
  require(run_handler(foreign, {3, "run"}, foreign_invocation).status == RunOutcome::Status::Error,
          "acquisition does not change get's own-object scope");
  foreign_invocation.deadline = {};
  require(run_handler(foreign, {3, "read_foreign"}, foreign_invocation).status ==
                  RunOutcome::Status::Error &&
              std::get<bool>(foreign_state.at(StateKey{std::string("attempted")})) &&
              !foreign_state.contains(StateKey{std::string("copied")}) &&
              !foreign_state.contains(StateKey{std::string("returned")}) &&
              read(projection, f.b, 1, 0, 20).value.numeric() == 7,
          "an acquired handle does not expose a foreign value read host or continue past it");
  Vm cyclic({4096, 50, 5, 64});
  bind_events(cyclic, f.terms);
  ScriptDeclaration bad;
  bad.id = 2;
  auto cycle = compile_script(
      "return {on={run=function(self) local p={}; p.loop=p; self:emit(self.handle,'x.2',p) end}}");
  require(load_script(cyclic, cycle, bad).status == RunOutcome::Status::Ok,
          "load scratch event cycle");
  ScriptState cycle_state;
  DeferredEffects cycle_effects;
  require(execute(cyclic, cycle_state, cycle_effects, f.a, 2).status == RunOutcome::Status::Error,
          "cyclic event table rejected before recursion");

  auto armed = threshold_timers(Subscriptions{}, f.processes, {f.a, 1}, 0, 20, {});
  require(armed.empty(), "unsubscribed process schedules nothing");
}
void synchronous_actions_and_nested_deadlines() {
  Fixture f;
  f.terms.rows[3].kind = Kind::Action;
  Vm vm({4096, 50, 5, 64});
  bind_events(vm, f.terms);
  ScriptDeclaration declaration;
  declaration.id = 1;
  auto compiled = compile_script(R"(return {on={run=function(self, sender, target)
    self.state.before=(self.state.before or 0)+1
    local result=self:act('x.3',target,{object=self.handle,point=vector.create(1,2,3),amount=34,fraction=0.5})
    if result.ok then self.state.after=result.created[1]
    else self.state.denied=result.denied.need end
  end}})");
  require(load_script(vm, compiled, declaration).status == RunOutcome::Status::Ok,
          "load nested action script");
  ScriptState outer_state, inner_state;
  DeferredEffects outer_effects, inner_effects;
  std::uint64_t rng = 1;
  Invocation outer, inner;
  outer.self = {f.a, 1};
  outer.payload = Value(f.b);
  outer.state = &outer_state;
  outer.effects = &outer_effects;
  outer.random = &rng;
  outer.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  inner.self = {f.b, 1};
  inner.payload = Value{};
  inner.state = &inner_state;
  inner.effects = &inner_effects;
  inner.random = &rng;
  inner.deadline = outer.deadline;
  const HandlerRef handler{1, "run"};
  auto outcome = run_handler(vm, handler, outer);
  require(outcome.status == RunOutcome::Status::Action && outcome.action &&
              outcome.action->arguments["object"] == to_string(f.a) &&
              outcome.action->arguments["amount"].is_number_integer() &&
              outcome.action->arguments["amount"] == 34 &&
              outcome.action->arguments["fraction"].is_number_float() &&
              outcome.action->arguments["point"] == nlohmann::json::array({1, 2, 3}),
          "act preserves typed handles and vector arguments");
  const auto nested = run_handler(vm, handler, inner);
  require(nested.status == RunOutcome::Status::Action && !nested.action->request.target,
          "nested targetless ordinary actions accept nil while the caller waits");
  require(resume_handler(vm, inner, {{"ok", false}, {"denied", {{"need", "use"}}}}).status ==
                  RunOutcome::Status::Ok &&
              std::get<std::string>(inner_state.at(std::string("denied"))) == "use",
          "callee resumes with the actual canonical denial");
  require(resume_handler(vm, outer, {{"ok", true}, {"created", {to_string(f.c)}}}).status ==
                  RunOutcome::Status::Ok &&
              std::get<Handle>(outer_state.at(std::string("after"))) == f.c,
          "outer call retains its own Lua stack and receives real created handles");
  outer.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  require(run_handler(vm, handler, outer).status == RunOutcome::Status::Action,
          "start another ordinary action");
  outer.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  require(resume_handler(vm, outer, {{"ok", true}}).status == RunOutcome::Status::Cutoff &&
              std::get<double>(outer_state.at(std::string("before"))) == 2,
          "nested work does not reset the caller deadline or roll back prior writes");
  outer.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  require(run_handler(vm, handler, outer).status == RunOutcome::Status::Action &&
              resume_handler(vm, outer, {{"ok", false}, {"denied", {{"need", "take"}}}}).status ==
                  RunOutcome::Status::Ok,
          "cutoff unwinds the active VM call without poisoning later runs");
}
void nested_calls_share_live_state() {
  Fixture f;
  f.terms.rows[3].kind = Kind::Action;
  Vm vm({4096, 50, 1000, 64});
  bind_events(vm, f.terms);
  ScriptDeclaration declaration;
  declaration.id = 1;
  const auto code = compile_script(R"(return {on={run=function(self,_,inner)
    if inner then self.state.large=string.rep('x',8192); return end
    local result=self:act('x.3',self.handle,{})
    assert(result.ok)
    self.state.large=false
    self.state.returned=true
  end}})");
  const auto loaded = load_script(vm, code, declaration);
  require(loaded.status == RunOutcome::Status::Ok, loaded.error.c_str());
  ScriptState state;
  DeferredEffects outer_effects, inner_effects;
  std::uint64_t rng = 1;
  Invocation outer, inner;
  outer.self = inner.self = {f.a, 1};
  outer.state = inner.state = &state;
  outer.random = inner.random = &rng;
  outer.effects = &outer_effects;
  inner.effects = &inner_effects;
  outer.payload = Value(false);
  inner.payload = Value(true);
  outer.deadline = inner.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  const HandlerRef handler{1, "run"};
  require(run_handler(vm, handler, outer).status == RunOutcome::Status::Action,
          "outer handler suspends before a child adds shared state");
  require(run_handler(vm, handler, inner).status == RunOutcome::Status::Ok &&
              std::get<std::string>(state.at(std::string("large"))) == std::string(8192, 'x'),
          "nested same-script call mutates the actual shared installation state");
  const auto outcome = resume_handler(vm, outer, {{"ok", true}});
  require(outcome.status == RunOutcome::Status::Ok &&
              !std::get<bool>(state.at(std::string("large"))) &&
              std::get<bool>(state.at(std::string("returned"))),
          "parent continues and replaces the state added by its nested call");
}
void candidate_start_uses_current_installation() {
  test_runtime::Fixture f(1000);
  const auto material = f.add(R"(return {kind='rule',applies_to={material='novel.mat'},
    sets={'novel.heat'},on={['novel.cue']=function(self)self:set('novel.heat',0)end},
    tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=0}}}})");
  const auto model = f.add(R"(return {kind='model',sets={'novel.heat'},on={
    ['novel.cue']=function(self)
      assert(self:get('novel.heat')==0)
      self:set('novel.heat',1);error('partial model effect') end}})");
  const auto removed = f.add(R"(return {kind='instance',sets={'novel.heat'},on={
    ['novel.cue']=function(self) self:set('novel.heat',999);self.state.recreated=true end}})");
  const auto retained = f.add(R"(return {kind='instance',sets={'novel.heat'},on={
    ['novel.cue']=function(self) assert(self:get('novel.heat')==1);self:set('novel.heat',2) end}})");
  f.models.rows[1].script = model;
  const std::array both{removed, retained};
  set_instances(f.installed, f.a, both, f.scripts);
  project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  project_instance(f.triggers, f.installed, f.scripts, f.terms, f.a);
  add_rule(f.rules, f.scripts, f.terms, material);
  project_rules(f.triggers, f.rules);
  const auto candidates = dispatch(f.triggers, f.values(), f.a, 6, 0, 20);
  require(candidates.size() == 4 && candidates[0].script == material &&
              candidates[1].script == model && candidates[2].script == removed &&
              candidates[3].script == retained,
          "material, model and instance candidates retain broadest-first order");
  auto &objects_state = f.objects_state;
  record::Journal journal(8, {});
  const auto start = [&](const HandlerRef &handler, Invocation &inv, CompletedInvocation &done) {
    InvocationInput input;
    input.receiver = f.a;
    input.trigger = 6;
    input.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    return start_invocation(f.react_state, objects_state, 7, f.catalog_state, journal, handler,
                            std::move(input), inv, done);
  };
  Invocation broad;
  CompletedInvocation broad_result;
  require(start(candidates[0], broad, broad_result) &&
              broad_result.outcome.status == RunOutcome::Status::Ok,
          "material rule runs first on the live object");
  Invocation first;
  CompletedInvocation first_result;
  require(start(candidates[1], first, first_result) &&
              first_result.outcome.status == RunOutcome::Status::Error,
          "model handler error retains its completed effect");
  // These are the same primary installation/state changes made by uninstall.
  f.states[{f.a, removed}][std::string("old")] = true;
  const std::array remaining{retained};
  set_instances(f.installed, f.a, remaining, f.scripts);
  project_instance(f.triggers, f.installed, f.scripts, f.terms, f.a);
  f.states.erase({f.a, removed});
  Invocation old;
  CompletedInvocation old_result;
  require(!start(candidates[2], old, old_result) && !f.states.contains({f.a, removed}) &&
              !old_result.origin.cause && old_result.outcome.status == RunOutcome::Status::Missing,
          "uninstalled copied candidate neither runs nor recreates its retired state");
  Invocation last;
  CompletedInvocation last_result;
  require(start(candidates[3], last, last_result) &&
              last_result.outcome.status == RunOutcome::Status::Ok &&
              read(f.values(), f.a, 1, 0, 20).value.numeric() == 2,
          "a prior error or unrelated uninstall never vetoes another applicable handler");
  require(journal.take().size() == 3, "skipped candidate emits no fictitious handler record");
}
void suspended_handler_sees_replaced_parts() {
  test_runtime::Fixture f(1000);
  const auto script = f.add(R"(return {kind='model',sets={'novel.heat'},on={
    ['novel.cue']=function(self)
      self.state.before=self:parts()[1]
      assert(self:act('novel.touch',self.handle,{}).ok)
      local parts=self:parts()
      assert(#parts==1 and parts[1]~=self.state.before)
      self:set('novel.heat',42,parts[1])
      self.state.after=parts[1]
    end}})");
  f.models.rows[1].script = script;
  project_model(f.triggers, f.models, f.scripts, f.terms, 1);
  auto &objects_state = f.objects_state;
  lifecycle::Parts &parts = objects_state.parts;
  Relation &declared = objects_state.declared_parts;
  ObjectInit initial;
  const auto born = lifecycle::instantiate(f.objects, parts, declared,
                                           {1, 0, 8, {{"old", 1, 8, {}, {}}}}, initial);
  const auto root = born.root, old_part = born.objects.at(1);
  InvocationInput input;
  input.receiver = root;
  input.trigger = 6;
  input.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  Invocation invocation;
  CompletedInvocation completed;
  record::Journal journal(8, {});
  const auto handlers = dispatch(f.triggers, f.values(), root, 6, 0, 20);
  require(start_invocation(f.react_state, objects_state, 7, f.catalog_state, journal,
                           handlers.front(), std::move(input), invocation, completed) &&
              completed.outcome.status == RunOutcome::Status::Action,
          "actual script suspends while its assembly still has the old part");
  const auto plan =
      lifecycle::plan_replace(f.models, parts, identity(f.objects), transforms(f.objects),
                              f.objects.params, root, {1, 0, 8, {{"new", 1, 8, {}, {}}}});
  const auto added = lifecycle::apply_replacement(f.objects, parts, declared, plan, 2, 0);
  lifecycle::release_rows(f.objects, parts, declared, plan.removed);
  const auto new_part = added.objects.at(0);
  require(!identity(f.objects).valid(old_part) && identity(f.objects).valid(new_part) &&
              new_part != old_part,
          "replacement retires the old part and creates a distinct live identity");
  refresh_parts(invocation, identity(f.objects), declared);
  const auto outcome = resume_handler(f.vm, invocation, {{"ok", true}});
  require(outcome.status == RunOutcome::Status::Ok &&
              std::get<Handle>(f.states.at({root, script}).at(std::string("after"))) == new_part &&
              read(f.values(), new_part, 1, 0, 20).value.numeric() == 42,
          "continued parts query and own-part state write use the live replacement closure");
}
} // namespace
int main() {
  source_resolution();
  owner_value_view_reads_bound_relations();
  process_thresholds_and_transfer();
  subscription_rebind_and_relations();
  allocation_rows_track_current_nonzero_shares_only();
  contribution_and_metering();
  dispatch_order();
  action_handlers_receive_the_actual_caller();
  luau_state_and_limits();
  live_activation_and_dispatch_authority();
  luau_bound_values_and_events();
  synchronous_actions_and_nested_deadlines();
  nested_calls_share_live_state();
  candidate_start_uses_current_installation();
  suspended_handler_sees_replaced_parts();
  std::cout << "reaction and real Luau checks passed\n";
}
