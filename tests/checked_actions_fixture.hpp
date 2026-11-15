#pragma once
#include "bindings/metadata/metadata.hpp"

#include "bindings/physical.hpp"
#include "bindings/turns/turns.hpp"
#include "runtime_fixture.hpp"
#include "space_fixture.hpp"

namespace amber::test_actions {
struct Fixture {
  test_runtime::Fixture f{250};
  space::Space space;
  space::PhysicalRelations physical;
  Relation parts;
  perm::Players players;
  perm::Grants grants;
  perm::Plots plots;
  perm::OwnerLimits limits;
  control::Profiles profiles;
  step::Intents intents;
  std::vector<step::IntentEnding> endings;
  bindings::MechanicalCauses causes{3};
  observe::Hearing hearing;
  std::vector<observe::Listener> listeners;
  catalog::InstanceTexts instance_texts;
  catalog::ModelTexts model_texts;
  bindings::NavigationBinding navigation{intents, endings, identity(f.objects),
                                         players, grants,  plots};
  bindings::AttachmentBinding attachment{space,
                                         identity(f.objects),
                                         transforms(f.objects),
                                         parents(f.objects),
                                         space_flags(f.objects),
                                         physical,
                                         {20, 21, 22, 23},
                                         parts,
                                         authority(f.objects),
                                         limits,
                                         players,
                                         grants,
                                         2.f,
                                         causes,
                                         plot_membership(f.objects),
                                         plots};
  bindings::ImpulseBinding impulse{space, identity(f.objects), causes};
  bindings::SweepBinding sweep{
      space, identity(f.objects), transforms(f.objects), parts, causes, plot_membership(f.objects),
      plots};
  bindings::PartPoseBinding part_pose{space,
                                      identity(f.objects),
                                      transforms(f.objects),
                                      parts,
                                      causes,
                                      plot_membership(f.objects),
                                      plots,
                                      1024};
  bindings::QuantityBinding quantity{
      f.properties,         f.processes, f.terms, identity(f.objects),
      authority(f.objects), limits,      players, grants};
  bindings::ObjectBinding object_read{identity(f.objects),  transforms(f.objects),
                                      authority(f.objects), parents(f.objects),
                                      f.objects.params,     f.models,
                                      f.installed};
  bindings::QueryBinding query{space, identity(f.objects), transforms(f.objects)};
  react::RelationBinding related{f.relation_view, f.terms};
  bindings::SoundBinding sound{hearing,   identity(f.objects), transforms(f.objects),
                               listeners, instance_texts,      model_texts};
  bindings::Execution execution{f.vm, f.states, f.random, identity(f.objects), parts, 7};
  command::NativeActions declarations;
  drive::NativeOperations native;
  drive::Clock clock{};
  record::Journal journal{8, {}};
  step::WorkQueues queues;
  step::TimerWheel timers;
  std::vector<FactKey> mutated;
  std::vector<bindings::GapDraft> gaps;
  react::MeterRows meters;
  react::PriorityLinks priorities;
  bindings::AllocationReconciliation allocation{f.allocations, meters, priorities};
  bindings::HandlerTurns handlers{
      execution, f.triggers, *f.projection, {f.subscriptions, f.links, f.scripts, f.rules},
      journal,   queues,     timers,        mutated,
      clock,     20,         10000,         allocation};
  bindings::ActionTurns actions{handlers,
                                declarations,
                                native,
                                f.models,
                                f.scripts,
                                f.installed,
                                f.rules,
                                profiles,
                                authority(f.objects),
                                transforms(f.objects),
                                limits,
                                players,
                                grants,
                                space,
                                intents,
                                gaps,
                                2.f};
  ScriptId script = 0;
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
    f.objects.position[f.a.slot()] = {0, 0, 0};
    f.objects.position[f.b.slot()] = {0, 1, 0};
    f.objects.position[f.c.slot()] = {1, 0, 0};
    f.terms.rows[1].conserved = true;
    f.properties.resize(64);
    f.processes.resize(64);
    f.terms.rows.resize(24);
    for (TermId id = 20; id < 24; ++id) {
      f.terms.rows[id].id = id;
      f.terms.rows[id].name = "mechanical." + std::to_string(id);
      f.terms.rows[id].kind = react::Kind::Relation;
      f.terms.by_name[f.terms.rows[id].name] = id;
    }
    f.relation_view.columns[22] = &physical.attached_to;
    f.terms.rows[7].inputs = {{"mode", react::InputType::String, 0, true},
                              {"target", react::InputType::Handle, 0, true}};
    auto bind = [&](const char *name, auto fn, auto &data) {
      react::register_binding(f.vm, name, fn, &data);
    };
    bind("navigate", bindings::navigate_binding, navigation);
    bind("cancel_motion", bindings::cancel_motion_binding, navigation);
    bind("sweep", bindings::sweep_binding, sweep);
    bind("move_part", bindings::move_part_binding, part_pose);
    bind("object", bindings::object_binding, object_read);
    bind("world.near", bindings::near_binding, query);
    bind("world.raycast", bindings::ray_binding, query);
    bind("related", react::related_binding, related);
    bind("sound", bindings::sound_binding, sound);
    bindings::register_physical_actions(f.terms, declarations, native, attachment, impulse,
                                        quantity);
    add_body(f.a, {.6f, .6f, 1.8f}, space::Motion::Character);
    add_body(f.b, {.4f, .4f, .4f}, item);
    add_body(f.c, {.5f, .5f, .5f}, space::Motion::Dynamic);
  }
  void add_body(Handle h, Vec3 size, space::Motion motion, bool solid = true) {
    testspace::Box shape(size);
    auto geometry = space::import_geometry(space, shape.source());
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
    auto h = create(f.objects, init);
    add_body(h, size, motion, solid);
    return h;
  }
  ScriptId load(std::string_view source, PlayerId author = 1) {
    script = f.add(source, author);
    f.models.rows[2].script = script;
    install::project_model(f.triggers, f.models, f.scripts, f.terms, 2);
    return script;
  }
  bindings::ActionOutcome act(std::string_view name, Handle target = {},
                              nlohmann::json args = nlohmann::json::object()) {
    return bindings::perform_action(
        actions,
        {ScriptHolder{script, f.a}, f.terms.by_name.at(std::string(name)), target, Value{}},
        std::move(args), {});
  }
  bindings::ActionOutcome player(Handle target = {},
                                 nlohmann::json args = nlohmann::json::object()) {
    return bindings::perform_action(actions, {PlayerId{1}, 7, target, Value{}}, std::move(args),
                                    {});
  }
  GrantId grant(Handle object, perm::Rights rights) {
    const auto holder = perm::Holder::script(script, f.a);
    const auto owner = f.objects.owner[object.slot()];
    const auto scope = perm::Scope::object(object);
    auto granted = perm::create_grant(
        grants, players, perm::Holder::player(owner), {owner, holder, scope, rights, 0, 0, 10000},
        {scope, owner, perm::all_rights, 0, false}, {holder, owner, true}, 0);
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
} // namespace amber::test_actions
