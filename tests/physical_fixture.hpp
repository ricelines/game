#pragma once

#include "runtime_fixture.hpp"
#include "space_fixture.hpp"
#include "step/action_private.hpp"

namespace amber::test_physical {
struct Fixture {
  test_runtime::Fixture f{1000};
  space::Space space;
  space::PhysicalRelations physical;
  Relation parts;
  perm::Players players;
  perm::Grants grants;
  perm::Plots plots;
  perm::OwnerLimits limits;
  net::Profiles profiles;
  step::Intents intents;
  std::vector<step::IntentEnding> endings;
  step::MechanicalCauses causes{3};
  observe::Hearing hearing;
  std::vector<observe::Listener> listeners;
  catalog::InstanceTexts instance_texts;
  catalog::ModelTexts model_texts;
  catalog::ParameterBlocks parameters;
  catalog::Assets assets;
  react::MeterRows meters;
  react::NativeActions declarations;
  step::Clock clock;
  record::Journal journal{8, {}};
  step::WorkQueues queues;
  step::TimerWheel timers;
  ScriptId script = 0;
  float reach_m = 2;

  explicit Fixture(space::Settings config = testspace::settings(),
                   space::Motion item = space::Motion::Dynamic)
      : space(config) {
    players.rows.resize(4);
    profiles.rows.resize(4);
    for (PlayerId id = 1; id < 4; ++id)
      profiles.rows[id].id = id;
    profiles.rows[1].body = f.a;
    profiles.rows[2].body = f.c;
    f.models.rows.resize(3);
    f.models.rows[2].id = 2;
    f.objects.model[f.a.slot()] = 2;
    f.objects.flags[f.a.slot()] = Body;
    f.objects.owner[f.a.slot()] = 1;
    f.objects.owner[f.b.slot()] = 1;
    f.objects.owner[f.c.slot()] = 2;
    f.objects.position[f.a.slot()] = {};
    f.objects.position[f.b.slot()] = {0, 1, 0};
    f.objects.position[f.c.slot()] = {1, 0, 0};
    f.terms.rows[1].conserved = true;
    f.properties.resize(24);
    f.processes.resize(24);
    f.terms.rows.resize(24);
    for (TermId id = 20; id < 24; ++id) {
      f.terms.rows[id].id = id;
      f.terms.rows[id].name = "mechanical." + std::to_string(id);
      f.terms.rows[id].kind = react::Kind::Relation;
      f.terms.by_name[f.terms.rows[id].name] = id;
    }
    f.relation_view.columns[22] = &physical.attached_to;
    step::declare_physical_actions(f.terms, declarations);
    step::bind_physical(f.vm, space, identity(f.objects), transforms(f.objects), parts, causes,
                        plot_membership(f.objects), plots, intents, endings, players, grants, 1024,
                        hearing, listeners, instance_texts, model_texts);
    step::bind_metadata(f.vm, identity(f.objects), transforms(f.objects), authority(f.objects),
                        parents(f.objects), f.objects.params, f.models, f.installed, parameters,
                        assets, f.terms, f.materials, parts, meters, plots, 0);
    add_body(f.a, {.6f, .6f, 1.8f}, space::Motion::Character);
    add_body(f.b, {.4f, .4f, .4f}, item);
    add_body(f.c, {.5f, .5f, .5f}, space::Motion::Dynamic);
  }
  void add_body(Handle h, Vec3 size, space::Motion motion, bool solid = true) {
    const std::uint32_t bits =
        motion == space::Motion::Character
            ? 0u
            : std::uint32_t(motion == space::Motion::Static    ? Static
                            : motion == space::Motion::Dynamic ? Dynamic
                                                               : ScriptedMotion);
    space_flags(f.objects).set(h.slot(), bits);
    testspace::Box shape(size);
    const auto geometry = space::import_geometry(space, shape.source());
    space::BodySpec body{h, geometry.geometry, motion, {.6f, 0, 0}, 10, false, false, {}, solid};
    if (motion == space::Motion::Character) {
      body.mass_kg = 70;
      body.character = space::CharacterSpec{
          .3f, 1.8f, 70, 100, 1.8f, 8, .24f, std::numbers::pi_v<float> / 4, .3f};
    }
    test_runtime::require(
        space::add(space, identity(f.objects), transforms(f.objects), body).status ==
            space::Status::Ok,
        "actual fixture body installs");
  }
  Handle box(Vec3 size, Vec3 at, space::Motion motion = space::Motion::Static, bool solid = true) {
    ObjectInit init;
    init.model = 1;
    init.material = 8;
    init.owner = 1;
    init.position = at;
    init.flags = motion == space::Motion::Static    ? Static
                 : motion == space::Motion::Dynamic ? Dynamic
                                                    : ScriptedMotion;
    const auto h = create(f.objects, init);
    add_body(h, size, motion, solid);
    return h;
  }
  ScriptId load(std::string_view source, PlayerId author = 1) {
    script = f.add(source, author);
    f.models.rows[2].script = script;
    react::project_model(f.triggers, f.models, f.scripts, f.terms, 2);
    return script;
  }
  step::ActionRun begin_as(Caller caller, std::string_view name, Handle target, nlohmann::json args,
                           record::Origin parent = {}) {
    step::NativeCall call;
    call.request = {caller, f.terms.by_name.at(std::string(name)), target, {}};
    call.arguments = std::move(args);
    call.step = clock.step;
    call.hz = 20;
    return step::begin_action(f.values(), f.models, f.scripts, f.installed, f.rules, f.triggers,
                              declarations, profiles, players, grants, limits, authority(f.objects),
                              attribution(f.objects), transforms(f.objects), space, intents,
                              journal, reach_m, std::move(call), parent,
                              std::chrono::steady_clock::now() + std::chrono::seconds(1));
  }
  step::ActionRun begin(std::string_view name, Handle target, nlohmann::json args,
                        record::Origin parent = {}) {
    return begin_as(ScriptHolder{script, f.a}, name, target, std::move(args), std::move(parent));
  }
  step::ActionOutcome finish(step::ActionRun &run, step::NativeCompletion completion) {
    const auto outcome = step::finish_native(run, completion, identity(f.objects),
                                             attribution(f.objects), f.models, f.scripts, journal);
    step::publish_effects(std::move(completion.effects), outcome.origin, clock.step, queues, timers,
                          journal, 10000);
    return outcome;
  }
  step::ActionOutcome transfer(Handle target, nlohmann::json args) {
    auto run = begin("engine.transfer", target, std::move(args));
    if (run.outcome)
      return *run.outcome;
    return finish(run,
                  step::transfer_action(f.properties, f.processes, f.terms, identity(f.objects),
                                        authority(f.objects), limits, players, grants, run.call));
  }
  step::ActionOutcome grasp(Handle target, nlohmann::json args = nlohmann::json::object(),
                            record::Origin parent = {}) {
    auto run = begin("engine.grasp", target, std::move(args), parent);
    if (run.outcome)
      return *run.outcome;
    return finish(run, step::grasp_action(space, identity(f.objects), transforms(f.objects),
                                          parents(f.objects), space_flags(f.objects), physical,
                                          parts, {20, 21, 22, 23}, reach_m, causes,
                                          plot_membership(f.objects), plots, run.call));
  }
  step::ActionOutcome release(Handle target, nlohmann::json args = nlohmann::json::object(),
                              record::Origin parent = {}) {
    auto run = begin("engine.release", target, std::move(args), parent);
    if (run.outcome)
      return *run.outcome;
    return finish(run, step::release_action(space, identity(f.objects), transforms(f.objects),
                                            parents(f.objects), space_flags(f.objects), physical,
                                            parts, {20, 21, 22, 23}, authority(f.objects), limits,
                                            players, grants, causes, plot_membership(f.objects),
                                            plots, run.call));
  }
  step::ActionOutcome attach(Handle target, nlohmann::json args) {
    auto run = begin("engine.attach", target, std::move(args));
    if (run.outcome)
      return *run.outcome;
    return finish(run, step::attach_action(space, identity(f.objects), transforms(f.objects),
                                           parents(f.objects), space_flags(f.objects), physical,
                                           parts, {20, 21, 22, 23}, authority(f.objects), limits,
                                           players, grants, reach_m, causes,
                                           plot_membership(f.objects), plots, run.call));
  }
  step::ActionOutcome impulse(Handle target, nlohmann::json args, record::Origin parent = {}) {
    auto run = begin("engine.impulse", target, std::move(args), parent);
    if (run.outcome)
      return *run.outcome;
    return finish(run, step::impulse_action(space, identity(f.objects), causes, run.call));
  }
  struct HandlerResult {
    react::RunOutcome outcome;
    record::Origin origin;
    bool changed;
  };
  HandlerResult invoke(EventPayload payload = {}, Handle sender = {}, record::Origin parent = {}) {
    react::InvocationInput input;
    input.receiver = f.a;
    input.sender = sender;
    input.trigger = 7;
    input.payload = std::move(payload);
    input.step = clock.step;
    input.hz = 20;
    input.origin = std::move(parent);
    input.action_caller = PlayerId{1}; // descriptive speech attribution, never native authority
    input.action_body = f.a;
    react::Invocation invocation;
    react::CompletedInvocation completed;
    const auto handlers = react::dispatch(f.triggers, f.values(), f.a, 7, clock.step, 20);
    test_runtime::require(handlers.size() == 1 && handlers.front().script == script,
                          "actual current fixture handler is installed");
    react::start_invocation(f.vm, f.states, f.random, f.values(), f.triggers, parts, 7,
                            attribution(f.objects), f.models, f.scripts, journal, handlers.front(),
                            std::move(input), invocation, completed);
    test_runtime::require(completed.outcome.status != react::RunOutcome::Status::Action,
                          "nested checked actions belong in the actual server process tests");
    react::finish_invocation(identity(f.objects), attribution(f.objects), f.models, f.scripts,
                             journal, invocation, completed);
    HandlerResult result{completed.outcome, completed.origin, completed.effects.changed};
    step::publish_effects(std::move(completed.effects), completed.origin, clock.step, queues,
                          timers, journal, 10000);
    return result;
  }
  GrantId grant(Handle object, perm::Rights rights) {
    const auto holder = perm::Holder::script(script, f.a);
    const auto owner = f.objects.owner[object.slot()];
    const auto scope = perm::Scope::object(object);
    const auto granted = perm::create_grant(
        grants, players, perm::Holder::player(owner), {owner, holder, scope, rights, 0, 0, 10000},
        {scope, owner, perm::all_rights, 0, false}, {holder, owner, true}, clock.step);
    test_runtime::require(bool(granted), "actual script object grant");
    return granted.grant;
  }
  space::StepResult tick() {
    space::StepResult result;
    space::step(space, identity(f.objects), transforms(f.objects), parents(f.objects),
                space_flags(f.objects), physical, ++clock.step, .05f, {}, result);
    return result;
  }
};
} // namespace amber::test_physical
