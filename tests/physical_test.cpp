#include "bindings/physical.hpp"
#include "space_fixture.hpp"
namespace ob = amber::observe;
namespace re = amber::react;
namespace bi = amber::bindings;
namespace pe = amber::perm;
using namespace testspace;
struct Fixture {
  Scene scene;
  pe::Players players;
  pe::Grants grants;
  pe::Plots plots;
  pe::OwnerLimits limits;
  Relation parts;
  step::Intents intents;
  std::vector<step::IntentEnding> endings;
  std::vector<sp::RelationChange> changes;
  bi::MechanicalCauses causes;
  re::Properties properties;
  re::Processes processes;
  re::Terms terms;
  re::Vm vm{{4096, 50, 5, 64}};
  re::ScriptState state;
  re::DeferredEffects effects;
  SparseColumn<std::vector<ScriptId>> installations;
  catalog::Models models;
  ob::Hearing hearing;
  std::vector<ob::Listener> listeners;
  Handle body, source, destination, other;
  bi::NavigationBinding navigation;
  bi::AttachmentBinding attachment;
  bi::ImpulseBinding impulse;
  bi::SweepBinding sweep;
  bi::QuantityBinding quantity;
  bi::AvatarControlBinding avatar;
  bi::SoundBinding sound;
  Fixture()
      : body(scene.character({0, 0, 0})),
        source(scene.box({.2f, .2f, .3f}, {0, 1, 0}, sp::Motion::Dynamic, 0, false)),
        destination(scene.box({.5f, .5f, .5f}, {1, 0, 0}, sp::Motion::Dynamic, 0, false)),
        other(scene.box({.5f, .5f, .5f}, {-1, 0, 0}, sp::Motion::Dynamic, 0, false)),
        navigation{intents,
                   endings,
                   identity(scene.objects),
                   authority(scene.objects),
                   limits,
                   players,
                   grants,
                   plots,
                   parts},
        attachment{scene.space,
                   identity(scene.objects),
                   transforms(scene.objects),
                   parents(scene.objects),
                   space_flags(scene.objects),
                   scene.relations,
                   {10, 11, 12, 13},
                   parts,
                   authority(scene.objects),
                   limits,
                   players,
                   grants,
                   1.5f,
                   causes},
        impulse{scene.space,
                identity(scene.objects),
                authority(scene.objects),
                limits,
                players,
                grants,
                parts,
                causes},
        sweep{scene.space,
              identity(scene.objects),
              transforms(scene.objects),
              authority(scene.objects),
              limits,
              players,
              grants,
              parts,
              causes},
        quantity{properties,
                 processes,
                 terms,
                 identity(scene.objects),
                 authority(scene.objects),
                 parents(scene.objects),
                 parts,
                 scene.relations.attached_to,
                 limits,
                 players,
                 grants},
        avatar{grants,        players, identity(scene.objects), authority(scene.objects),
               installations, models},
        sound{hearing, identity(scene.objects), transforms(scene.objects), listeners} {
    players.rows.resize(3);
    models.rows.resize(3);
    scene.objects.owner[body.slot()] = 1;
    scene.objects.owner[source.slot()] = 1;
    scene.objects.owner[destination.slot()] = 2;
    scene.objects.owner[other.slot()] = 1;
    terms.rows.resize(2);
    terms.rows[1].id = 1;
    terms.rows[1].name = "author.parcel";
    terms.by_name["author.parcel"] = 1;
    terms.rows[1].conserved = true;
    properties.resize(2);
    processes.resize(2);
    properties[1].set(source.slot(), Value(5.));
    properties[1].set(destination.slot(), Value(1.));
    auto bind = [&](const char *name, auto function, auto &data) {
      re::register_binding(vm, name, function, &data);
    };
    bind("navigate", bi::navigate_binding, navigation);
    bind("cancel_motion", bi::cancel_motion_binding, navigation);
    bind("attach", bi::attach_binding, attachment);
    bind("grasp", bi::grasp_binding, attachment);
    bind("release", bi::release_binding, attachment);
    bind("impulse", bi::impulse_binding, impulse);
    bind("sweep", bi::sweep_binding, sweep);
    bind("transfer", bi::transfer_binding, quantity);
    bind("allow_avatar", bi::allow_avatar_binding, avatar);
    bind("revoke_avatar", bi::revoke_avatar_binding, avatar);
    bind("sound", bi::sound_binding, sound);
  }
  void load(std::string_view source) {
    auto c = re::compile_script(source);
    require(c.errors.empty(), "script compile");
    re::ScriptDeclaration d;
    d.id = 1;
    auto result = re::load_script(vm, c, d);
    require(result.status == re::RunOutcome::Status::Ok, result.error.c_str());
  }
  re::RunOutcome run(const char *name, bool direct = true, Step step = 0,
                     std::vector<std::string> mechanics = {"transfer", "grasp", "release",
                                                           "impulse", "sweep", "attachment",
                                                           "navigation", "avatar_control", "sound"},
                     Handle selected = {}) {
    re::Invocation inv;
    inv.self = {body, 1};
    inv.step = step;
    inv.hz = 20;
    inv.acquired = {source, destination, other};
    inv.payload = std::vector<EventField>{
        {"source", Value(source)}, {"destination", Value(destination)}, {"other", Value(other)}};
    inv.state = &state;
    std::uint64_t rng = 1;
    inv.random = &rng;
    inv.effects = &effects;
    inv.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    if (direct)
      inv.direct_body = re::BodyEndpoints{body, selected ? selected : destination, source, 1,
                                          std::move(mechanics)};
    return re::run_handler(vm, {1, name}, inv);
  }
  void ok(const char *name, bool direct = true, Step step = 0,
          std::vector<std::string> mechanics = {"transfer", "grasp", "release", "impulse", "sweep",
                                                "attachment", "navigation", "avatar_control",
                                                "sound"},
          Handle selected = {}) {
    auto r = run(name, direct, step, std::move(mechanics), selected);
    require(r.status == re::RunOutcome::Status::Ok, r.error.c_str());
  }
  double amount(Handle h, Step now = 0) {
    return re::read_state(properties, processes, h, 1, now, 20).value.numeric();
  }
};
void transfer_authority_and_process_conservation() {
  Fixture f;
  f.load(R"(return {on={
    first_name=function(self,_,a) local r=self:transfer(a.source,a.destination,'author.parcel',2); self.state.result=type(r)=='number' and r or r.effect end,
    other_name=function(self,_,a) local r=self:transfer(a.source,a.destination,'author.parcel',1); self.state.result=type(r)=='number' and r or r.effect end,
    unrelated=function(self,_,a) self:transfer(a.other,a.destination,'author.parcel',1) end,
    retain=function(self,_,a) self.state.saved=a.source end,
    resume=function(self,_,a) self:transfer(self.state.saved,a.destination,'author.parcel',1) end
  }})");
  require(sp::attach(f.scene.space, identity(f.scene.objects), transforms(f.scene.objects),
                     parents(f.scene.objects), space_flags(f.scene.objects),
                     f.scene.relations.contains, f.scene.relations.supports,
                     f.scene.relations.attached_to, {f.source, f.body, {{0, 1, 1}, {}}, true},
                     f.changes)
                  .status == sp::Status::Ok,
          "fixture grasp");
  f.ok("first_name");
  require(f.amount(f.source) == 5 && f.amount(f.destination) == 1,
          "denied receive cannot debit source");
  f.scene.objects.preset[f.destination.slot()] = pe::bit(pe::Verb::Use);
  f.ok("first_name");
  require(f.amount(f.source) == 3 && f.amount(f.destination) == 3,
          "first authored handler transfers through primitive");
  f.ok("other_name");
  require(f.amount(f.source) == 2 && f.amount(f.destination) == 4,
          "differently named handler composes same primitive");
  f.properties[1].set(f.other.slot(), Value(9.));
  f.ok("unrelated");
  require(f.amount(f.other) == 9 && f.amount(f.destination) == 4,
          "acquired unrelated endpoint cannot use player authority");
  f.ok("retain");
  f.ok("resume", false);
  require(f.amount(f.source) == 2, "timer cannot reuse retained actor authority");
  f.ok("other_name", true, 0, {});
  require(f.amount(f.source) == 2, "unlisted mechanical scope grants no direct actor authority");
  f.processes[1].set(f.source.slot(), re::Process{5, 0, -1, 0, 10, 1});
  f.processes[1].set(f.destination.slot(), re::Process{1, 0, 1, 0, 10, 1});
  f.ok("first_name", true, 20);
  require(f.amount(f.source, 20) == 2 && f.amount(f.destination, 20) == 4,
          "process values settled before equal transfer");
  require(f.processes[1].get(f.source.slot())->rate == -1 &&
              f.processes[1].get(f.destination.slot())->rate == 1,
          "transfer preserves ongoing rates");
  f.processes[1].set(f.destination.slot(), re::Process{9, 20, 1, 0, 10, 3});
  auto before = f.amount(f.source, 20);
  require(f.run("first_name", true, 20).status == re::RunOutcome::Status::Error,
          "process bound failure reports handler error");
  require(f.amount(f.source, 20) == before && f.amount(f.destination, 20) == 9,
          "failed bound preflight leaves both endpoints unchanged");
}
void hold_release_navigation_sound_and_avatar() {
  Fixture f;
  f.load(R"(return {on={
    hold=function(self,_,a) self.state.result=self:grasp(a.source,{position=vector.create(0,1,1)}).effect end,
    drop=function(self,_,a) self.state.result=self:release(a.source,vector.create(2,0,2)).effect end,
    walk=function(self) self:navigate(vector.create(2,4,0),{speed_m_s=0.3}) end,
    stop=function(self) self:cancel_motion() end,
    audible=function(self) self:sound({text='hello',level=2},5) end,
    spoken=function(self) self:sound('spoken words',5,nil,{kind='speech'}) end,
    stored_and_spoken=function(self) self.state.utterances=(self.state.utterances or 0)+1; self:sound('stored words',5,nil,{kind='speech'}) end,
    consent=function(self,_,a) local r=self:allow_avatar(a.destination,7,3,1); if type(r)=='number' then self.state.grant=r end end,
    instance_consent=function(self,_,a) local r=self:allow_avatar(a.destination,8,1,1); if type(r)=='number' then self.state.instance_grant=r end end,
    revoke=function(self) self:revoke_avatar(self.state.grant) end
  }})");
  f.ok("hold", true, 0, {"grasp"}, f.source);
  require(f.scene.objects.parent[f.source.slot()] == f.body, "selected Take performs actual grasp");
  // Revoke item Take after grasp; relinquishing must remain possible.
  f.scene.objects.owner[f.source.slot()] = 2;
  f.ok("drop");
  require(!(f.scene.objects.flags[f.source.slot()] & Held),
          "release clears held after Take revocation");
  auto initial = f.scene.objects.position[f.source.slot()];
  for (int i = 0; i < 5; ++i)
    f.scene.tick();
  require(f.scene.objects.position[f.source.slot()].x > initial.x + .2f,
          "released velocity produces actual flight");
  f.effects = {};
  f.ok("walk");
  require(!f.effects.changed && !f.intents.moving.empty(),
          "own navigation starts an intent without changing world facts");
  require(f.intents.moving.at(f.body.bits).speed_m_s == .3f,
          "authored navigation selects requested speed");
  f.ok("stop");
  require(!f.effects.changed, "canceling own navigation does not change world facts");
  require(f.intents.moving.empty(), "authored cancellation");
  f.listeners = {{1, f.body, {0, 0, 1}}, {2, f.body, {20, 0, 1}}};
  f.effects = {};
  f.ok("audible");
  std::size_t omitted;
  auto heard = ob::take_heard(f.hearing, 1, 40, omitted);
  require(heard.size() == 1 && !f.hearing.queues.contains(2), "typed sound delivered by distance");
  const auto &audio_record = std::get<record::Event>(f.effects.ordered.back());
  require(audio_record.kind == record::Kind::System && audio_record.name == "sound" &&
              audio_record.detail["data"]["text"] == "hello" && !audio_record.changed &&
              !audio_record.detail.at("agent").get<bool>() && !f.effects.changed,
          "ordinary sound records typed payload without inferring speech");
  f.ok("spoken");
  const auto &speech_record = std::get<record::Event>(f.effects.ordered.back());
  require(speech_record.kind == record::Kind::Speech &&
              speech_record.actor.kind == record::Actor::Kind::Player &&
              speech_record.actor.id == 1 && speech_record.detail["data"] == "spoken words" &&
              speech_record.result == record::Result::Applied && !speech_record.changed &&
              !speech_record.detail.at("agent").get<bool>() && !f.effects.changed,
          "authored channel explicitly produces attributed Speech record");
  f.ok("spoken", false);
  require(std::get<record::Event>(f.effects.ordered.back()).actor.kind ==
              record::Actor::Kind::Script,
          "autonomous speech retains actual script actor");
  f.effects = {};
  f.ok("stored_and_spoken");
  require(f.effects.changed && std::get<double>(f.state.at(std::string("utterances"))) == 1 &&
              !std::get<record::Event>(f.effects.ordered.back()).changed,
          "speech preserves a stored state change from the same handler without counting its "
          "event as another mutation");
  f.models.rows[f.scene.objects.model[f.destination.slot()]].script = 7;
  f.ok("consent");
  auto holder = pe::Holder::script(7, f.destination);
  auto scope = pe::Scope::avatar(f.body);
  require(bool(pe::check_scope(holder, scope, 3, f.grants, 0)),
          "explicit body consent creates ordinary grant");
  require(!pe::check_scope(holder, scope, 3, f.grants, 20), "avatar consent expires");
  f.installations.set(f.destination.slot(), std::vector<ScriptId>{8});
  f.ok("instance_consent");
  require(bool(pe::check_scope(pe::Holder::script(8, f.destination), scope, 1, f.grants, 0)),
          "explicit instance script consent alongside active model default");
  auto count = f.grants.rows.size();
  f.ok("consent", false);
  require(f.grants.rows.size() == count, "timer cannot grant actor avatar authority");
  f.ok("revoke");
  require(!pe::check_scope(holder, scope, 3, f.grants, 0),
          "explicit direct action revokes prior consent");
}
void real_sweep_and_body_attachment() {
  Fixture f;
  bi::PartPoseBinding part_pose{f.scene.space, identity(f.scene.objects),
                                transforms(f.scene.objects), f.parts, f.causes};
  re::register_binding(f.vm, "move_part", bi::move_part_binding, &part_pose);
  f.load(R"(return {on={
    slide=function(self,_,a) self.state.result=self:sweep(a.other,vector.create(-2,0,0)).effect end,
    part=function(self,_,a) self.state.result=self:move_part(a.other,vector.create(-0.5,0,1),{x=0,y=0,z=0,w=1}).effect end,
    foreign=function(self,_,a) self.state.result=self:move_part(a.destination,vector.create(9,9,9),{x=0,y=0,z=0,w=1}).effect end,
    invalid=function(self,_,a) self:move_part(a.other,vector.create(9,9,9),{x=0,y=0,z=0,w=2}) end,
    seat=function(self,_,a) self.state.result=self:attach(self.handle,a.destination,{position=vector.create(0,0,0.5)}).effect end,
    leave=function(self) self.state.result=self:release(self.handle).effect end
  }})");
  f.parts.out.set(f.body.slot(), std::vector<Handle>{f.other});
  f.parts.in.set(f.other.slot(), std::vector<Handle>{f.body});
  require(sp::attach(f.scene.space, identity(f.scene.objects), transforms(f.scene.objects),
                     parents(f.scene.objects), space_flags(f.scene.objects),
                     f.scene.relations.contains, f.scene.relations.supports,
                     f.scene.relations.attached_to, {f.other, f.body, {{-1, 0, 0}, {}}, false},
                     f.changes)
                  .status == sp::Status::Ok,
          "ordinary moving part");
  require(sp::body_motion(f.scene.space, identity(f.scene.objects), f.other)->motion ==
              sp::Motion::Kinematic,
          "ordinary attachment creates the actual kinematic scripted part");
  f.scene.box({.5f, .5f, 1}, {-2, 0, 0}, sp::Motion::Static, 0, false);
  f.ok("slide", false);
  require(std::get<std::string>(f.state.at(std::string("result"))) == "obstructed" &&
              f.scene.objects.position[f.other.slot()].x > -1.6f,
          "own-part sweep stops at real chair geometry");
  f.ok("part", false);
  require(f.scene.objects.position[f.other.slot()] == Vec3{-.5f, 0, 1},
          "move_part sets actual declared independent solid part world pose");
  f.ok("foreign", false);
  require(std::get<std::string>(f.state.at(std::string("result"))) == "denied" &&
              f.scene.objects.position[f.destination.slot()] == Vec3{1, 0, 0},
          "move_part does not authorize foreign endpoints");
  require(f.run("invalid", false).status == re::RunOutcome::Status::Error &&
              f.scene.objects.position[f.other.slot()] == Vec3{-.5f, 0, 1},
          "move_part rejects nonunit rotation before changing geometry");
  f.scene.tick();
  require(f.scene.objects.position[f.other.slot()] ==
              f.scene.objects.position[f.body.slot()] + Vec3{-.5f, 0, 1},
          "move_part updates actual attachment offset across physics steps");
  f.scene.objects.preset[f.destination.slot()] = pe::bit(pe::Verb::Use);
  f.ok("seat");
  require(f.scene.objects.parent[f.body.slot()] == f.destination,
          "authored attachment seats actual body");
  f.ok("leave");
  require(!f.scene.objects.parent[f.body.slot()], "authored release detaches own body");
}

void checked_pose_batches(bool benchmark = false) {
  Fixture f;
  f.scene.box({100, 100, .1f}, {0, 0, -.1f});
  bi::PoseBatchBinding batch{f.scene.space,
                             identity(f.scene.objects),
                             transforms(f.scene.objects),
                             authority(f.scene.objects),
                             f.limits,
                             f.players,
                             f.grants,
                             f.parts,
                             1024,
                             f.causes};
  bi::PositionBinding position{identity(f.scene.objects), transforms(f.scene.objects)};
  re::RelationsRead relation_read;
  Relation members;
  re::RelationBinding related{relation_read, f.terms};
  f.terms.rows.resize(3);
  f.terms.rows[2].id = 2;
  f.terms.rows[2].name = "maker.members";
  f.terms.rows[2].kind = re::Kind::Relation;
  f.terms.by_name["maker.members"] = 2;
  relation_read.columns[2] = &members;
  re::register_binding(f.vm, "pose_batch", bi::pose_batch_binding, &batch);
  re::register_binding(f.vm, "related", re::related_binding, &related);
  re::register_binding(f.vm, "transform", bi::transform_binding, &position);
  f.load(R"(return {on={
    cycle=function(self)
      self.state.phase=(self.state.phase or 0)+0.01
      local rows={};for index,object in self:related('maker.members','out') do
        rows[index]={object=object,position=vector.create(index%20*0.05,index//20*0.05,1+math.sin(self.state.phase)*0.2)}
      end
      local r=self:pose_batch(rows);self.state.result=type(r)=='number' and r or r.effect
    end,
    badmode=function(self,_,args)
      local rows=self:related('maker.members','out');self:pose_batch({{object=rows[1],position=vector.create(20,20,20)}, {object=args.destination,position=vector.create(0,0,0)}})
    end,
    frame=function(self,_,args) self.state.rotation=self:transform(args.destination).rotation.w end
  }})");
  Box shape({.03f, .08f, .03f});
  auto geometry = sp::import_geometry(f.scene.space, shape.source());
  std::vector<Handle> objects;
  std::vector<GrantId> grants;
  for (int index = 0; index < 300; ++index) {
    ObjectInit init;
    init.model = 1;
    init.owner = 2;
    init.flags = ScriptedMotion;
    init.position = {0, 0, 1};
    auto h = create(f.scene.objects, init);
    objects.push_back(h);
    require(sp::add(f.scene.space, identity(f.scene.objects), transforms(f.scene.objects),
                    {h,
                     geometry.geometry,
                     sp::Motion::Kinematic,
                     {.5f, 0, 0},
                     .2f,
                     false,
                     false,
                     {},
                     false})
                    .status == sp::Status::Ok,
            "non-solid ordinary body add");
    auto holder = pe::Holder::script(1, f.body);
    auto scope = pe::Scope::object(h);
    auto result = pe::create_grant(f.grants, f.players,
                                   {2, holder, scope, pe::bit(pe::Verb::Move), 0, 0, 10000},
                                   {scope, 2, pe::all_rights, 0, false}, {holder, 1, true}, 0);
    require(bool(result), "installed script grant for ordinary batch endpoint");
    grants.push_back(result.grant);
  }
  members.out.set(f.body.slot(), objects);
  f.ok("cycle", false);
  require(std::get<double>(f.state.at(std::string("result"))) == 300,
          "one actual Lua batch changes300 independent granted entities");
  auto first = f.scene.objects.position[objects[0].slot()];
  f.ok("badmode", false);
  require(f.scene.objects.position[objects[0].slot()] == first,
          "unsupported later batch row leaves earlier pose unchanged");
  pe::revoke(f.grants, f.players, grants.back());
  f.ok("cycle", true);
  require(f.scene.objects.position[objects[0].slot()] == first,
          "later denied endpoint cannot borrow player's authority or half-apply batch");
  // Re-arm the explicit installed holder, then measure actual movement every iteration.
  const auto holder = pe::Holder::script(1, f.body);
  auto scope = pe::Scope::object(objects.back());
  require(bool(pe::create_grant(f.grants, f.players,
                                {2, holder, scope, pe::bit(pe::Verb::Move), 0, 0, 10000},
                                {scope, 2, pe::all_rights, 0, false}, {holder, 1, true}, 0)),
          "re-arm revoked script grant");
  f.ok("frame");
  require(std::get<double>(f.state.at(std::string("rotation"))) == 1,
          "read-only transform exposes current quaternion");
  std::vector<double> handler, frame;
  for (int index = 0; index < (benchmark ? 200 : 2); ++index) {
    auto start = std::chrono::steady_clock::now();
    f.effects = {};
    f.ok("cycle", false, Step(index));
    auto end = std::chrono::steady_clock::now();
    f.scene.tick();
    handler.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    frame.push_back(
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
            .count());
  }
  if (benchmark) {
    std::sort(handler.begin(), handler.end());
    std::sort(frame.begin(), frame.end());
    std::cout << "Lua_pose_batch_300 us p50=" << handler[100] << " p95=" << handler[190]
              << " max=" << handler.back() << '\n';
    std::cout << "Lua_batch_and_Jolt_step_300 us p50=" << frame[100] << " p95=" << frame[190]
              << " max=" << frame.back() << '\n';
  }
}

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--bench") {
      checked_pose_batches(true);
      return 0;
    }
    checked_pose_batches();
    transfer_authority_and_process_conservation();
    hold_release_navigation_sound_and_avatar();
    real_sweep_and_body_attachment();
    std::cout << "physical Luau binding checks passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
