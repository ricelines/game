#include "react/react.hpp"
#include "space_fixture.hpp"
#include "step/step.hpp"

#include <chrono>
#include <iostream>

using namespace testspace;
namespace st = amber::step;
namespace pe = amber::perm;
namespace re = amber::react;
struct Fixture {
  Scene scene;
  pe::Players players;
  pe::Grants grants;
  pe::Plots plots;
  pe::OwnerLimits limits;
  Relation parts;
  st::DeliveryTargets delivery;
  lifecycle::Parts named_parts;
  std::vector<sp::RelationChange> changes;
  catalog::Models models;
  re::Terms terms;
  re::Materials materials;
  re::InstanceScripts installed;
  re::MeterRows meters;
  catalog::ParameterBlocks parameters;
  catalog::Assets assets;
  Handle controller, root, child, body;
  react::AssetMeasurements measurements;
  react::InstanceSettings instance_settings;
  react::ModelSettings model_settings;
  react::PlotSettings plot_settings;
  react::WorldSettings world_settings;
  react::SharedColumns shared;
  react::RelationsRead relations;
  react::RuntimeMeasurements runtime;
  react::Properties properties;
  react::Processes processes;
  re::Vm vm{{4096, 100, 5, 64}};
  re::ScriptState state;
  re::DeferredEffects effects;
  re::NativeActions declarations;
  bool changed = false;
  std::vector<Handle> hidden;
  nlohmann::json reply;
  TermId action = 0;
  Fixture()
      : controller(scene.box({.2f, .2f, .2f}, {20, 0, 0}, sp::Motion::Static, 0, false)),
        root(scene.box({1, 1, .5f}, {}, sp::Motion::Static, 0, false)),
        child(scene.box({.2f, .2f, .2f}, {0, 0, .5f}, sp::Motion::Kinematic, 0, false)),
        body(scene.character({0, -1, 0})) {
    models.rows.resize(3);
    const auto asset_hash = catalog::sha256("goods fixture");
    for (auto id : {1u, 2u}) {
      models.rows[id].id = id;
      models.rows[id].asset = asset_hash;
    }
    catalog::Asset asset;
    asset.hash = asset_hash;
    asset.canonical_params = "{}";
    asset.manifest = {{"bindings", nlohmann::json::object()}};
    assets.rows[asset_hash] = asset;
    players.rows.resize(3);
    plots.rows.resize(3);
    plots.rows[1].id = 1;
    plots.rows[1].owner = 1;
    plots.rows[2].id = 2;
    for (auto h : {root, child, body}) {
      scene.objects.owner[h.slot()] = 1;
      scene.objects.plot[h.slot()] = 1;
    }
    scene.objects.plot[controller.slot()] = 2;
    parts.out.set(root.slot(), std::vector<Handle>{child});
    parts.in.set(child.slot(), std::vector<Handle>{root});
    named_parts.set(root.slot(), std::vector<lifecycle::NamedPart>{{"part", child}});
    require(sp::attach(scene.space, identity(scene.objects), transforms(scene.objects),
                       parents(scene.objects), space_flags(scene.objects), scene.relations.contains,
                       scene.relations.supports, scene.relations.attached_to,
                       {child, root, {{0, 0, .5f}, {}}, false}, changes)
                    .status == sp::Status::Ok,
            "fixture declared child attachment");
    const react::ValueRead values{terms,
                                  identity(scene.objects),
                                  scene.objects.plot,
                                  scene.objects.params,
                                  materials,
                                  measurements,
                                  properties,
                                  processes,
                                  instance_settings,
                                  model_settings,
                                  plot_settings,
                                  world_settings,
                                  shared,
                                  relations,
                                  runtime,
                                  0,
                                  0};
    step::bind_metadata(vm, values, transforms(scene.objects), authority(scene.objects),
                        parents(scene.objects), models, installed, parameters, assets, parts,
                        meters, plots);
    st::declare_hold_goods_action(terms, declarations);
    action = terms.by_name.at("engine.hold_goods");
    auto code = re::compile_script(R"(
      return {
        inspect=function(self) self.state.count=#self:plot_objects(1) end
      }
    )");
    re::ScriptDeclaration declaration;
    declaration.id = 1;
    declaration.author = 2;
    require(re::load_script(vm, code, declaration).status == re::RunOutcome::Status::Ok,
            "fixture script load");
  }
  GrantId grant(pe::Scope scope, pe::Rights rights, Step expiry = 100) {
    auto holder = pe::Holder::script(1, controller);
    auto result = pe::create_grant(grants, players, perm::Holder::player(1),
                                   {1, holder, scope, rights, 0, 0, expiry},
                                   {scope, 1, pe::all_rights, 1, false}, {holder, 1, true}, 0);
    require(bool(result), "fixture owner-approved installed grant");
    return result.grant;
  }
  void inspect() {
    effects = {};
    re::Invocation i;
    i.self = {controller, 1};
    i.step = 1;
    i.hz = 20;
    i.state = &state;
    i.effects = &effects;
    i.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    const auto result = re::run_handler(vm, {1, "inspect"}, i);
    require(result.status == re::RunOutcome::Status::Ok, result.error.c_str());
  }
  void collect(Handle target = {}, Step step = 1, Handle installation = {}) {
    st::NativeCall call;
    call.request = {ScriptHolder{1, installation ? installation : controller},
                    action,
                    target ? target : root,
                    {}};
    call.arguments = nlohmann::json::object();
    call.step = step;
    call.hz = 20;
    auto result = st::hold_goods(players, grants, limits, identity(scene.objects),
                                 authority(scene.objects), parents(scene.objects),
                                 space_flags(scene.objects), parts, delivery, scene.space, 1, call);
    st::apply_custody(scene.space, identity(scene.objects), transforms(scene.objects),
                      parents(scene.objects), space_flags(scene.objects), scene.relations,
                      named_parts, {}, plot_membership(scene.objects), plots, result.custody,
                      result);
    hidden = result.hidden_objects;
    changed = result.changed;
    reply = std::move(result.reply);
  }
};
void explicit_authority_and_online_assembly() {
  Fixture f;
  f.inspect();
  require(std::get<double>(f.state.at(std::string("count"))) == 1,
          "placed plot query excludes Body and declared children");
  f.grant(pe::Scope::plot(1), pe::Rights{7});
  f.collect();
  require(!f.reply.value("ok", false) && !(f.scene.objects.flags[f.root.slot()] & Held),
          "plot authority never confers goods custody");
  f.grant(pe::Scope::owned_on_plot(1, 1), pe::bit(pe::Verb::Transfer));
  const auto id =
      f.grant(pe::Scope::object(f.root), pe::bit(pe::Verb::Transfer) | pe::bit(pe::Verb::Destroy));
  f.delivery.present_bodies[1] = f.body;
  require(sp::attach(f.scene.space, identity(f.scene.objects), transforms(f.scene.objects),
                     parents(f.scene.objects), space_flags(f.scene.objects),
                     f.scene.relations.contains, f.scene.relations.supports,
                     f.scene.relations.attached_to, {f.body, f.root, {{0, 0, .5f}, {}}, false},
                     f.changes)
                  .status == sp::Status::Ok,
          "owner seated on collected goods");
  f.collect();
  require(f.reply.value("ok", false) && f.players.rows[1].inventory == std::vector<Handle>{f.root},
          "online goods enter actual owner inventory");
  require(f.scene.objects.parent[f.root.slot()] == f.body &&
              f.scene.objects.parent[f.child.slot()] == f.root &&
              !f.scene.objects.parent[f.body.slot()],
          "custody preserves internal graph and detaches outside seated body");
  require(!sp::bounds(f.scene.space, identity(f.scene.objects), f.root) &&
              !sp::bounds(f.scene.space, identity(f.scene.objects), f.child) &&
              sp::body_motion(f.scene.space, identity(f.scene.objects), f.body)->motion ==
                  sp::Motion::Character,
          "whole assembly hidden while actual avatar controller remains in scene");
  require(f.scene.objects.owner[f.root.slot()] == 1 && f.scene.objects.owner[f.child.slot()] == 1 &&
              f.grants.rows[id].live &&
              bool(pe::check(pe::Holder::script(1, f.controller), pe::Verb::Destroy,
                             pe::target(identity(f.scene.objects), authority(f.scene.objects),
                                        f.limits, f.root),
                             f.players, f.grants, 1)),
          "custody retains owner, ceilings and explicit later salvage grant");
  require(std::ranges::find(f.hidden, f.root) != f.hidden.end() &&
              std::ranges::find(f.hidden, f.child) != f.hidden.end() &&
              std::ranges::find(f.hidden, f.body) == f.hidden.end() && f.changed,
          "custody reports hidden goods closure while preserving the avatar");
  f.collect();
  require(f.players.rows[1].inventory.size() == 1 && !f.changed,
          "repeated custody is idempotent and reports no state change");
  f.inspect();
  require(std::get<double>(f.state.at(std::string("count"))) == 0,
          "stored goods leave placed plot query");
}
void denials_and_offline_delivery() {
  Fixture f;
  auto id = f.grant(pe::Scope::object(f.root), pe::bit(pe::Verb::Transfer), 10);
  require(!st::validate_hold_goods({{"recipient", "p:2"}}).empty(),
          "caller cannot redirect custody to a different owner");
  f.collect({}, 10);
  require(!f.reply.value("ok", false), "expired goods grant denied");
  f.collect({}, 1, f.body);
  require(!f.reply.value("ok", false), "same script on another installation has no grant");
  f.limits.set(f.child.slot(), pe::inherited_rights & ~pe::bit(pe::Verb::Transfer));
  f.collect();
  require(!f.reply.value("ok", false) &&
              sp::bounds(f.scene.space, identity(f.scene.objects), f.root),
          "part ceiling denial leaves whole world assembly intact");
  f.limits.erase(f.child.slot());
  f.players.rows[1].mailbox.push_back(f.controller);
  f.collect();
  require(!f.reply.value("ok", false) && !(f.scene.objects.flags[f.root.slot()] & Held),
          "full mailbox fails before native custody changes");
  f.players.rows[1].mailbox.clear();
  permission_flags(f.scene.objects).set(f.root.slot(), Escrow);
  f.collect();
  require(!f.reply.value("ok", false), "escrow remains unavailable to custody script");
  permission_flags(f.scene.objects).set(f.root.slot(), 0);
  f.collect(f.body);
  require(!f.reply.value("ok", false), "avatar is never ordinary goods");
  f.collect(f.child);
  require(!f.reply.value("ok", false), "declared member cannot bypass assembly root custody");
  f.collect();
  require(f.reply.value("ok", false) && f.players.rows[1].mailbox == std::vector<Handle>{f.root} &&
              !f.scene.objects.parent[f.root.slot()] && f.grants.rows[id].live,
          "offline custody stores one root without revoking grants");
  f.collect();
  require(f.reply.value("ok", false) && f.players.rows[1].mailbox.size() == 1 && !f.changed,
          "full mailbox accepts same already stored root idempotently");
  pe::revoke(f.grants, f.players, id);
  f.collect();
  require(!f.reply.value("ok", false), "custody never retains revoked caller authority");
}
void benchmark() {
  Fixture f;
  ObjectInit init;
  init.model = 1;
  init.plot = 2;
  for (unsigned i = 0; i < 100000; ++i)
    create(f.scene.objects, init);
  std::vector<double> times;
  for (unsigned i = 0; i < 100; ++i) {
    auto begin = std::chrono::steady_clock::now();
    f.inspect();
    times.push_back(
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin)
            .count());
  }
  std::sort(times.begin(), times.end());
  std::cout << "plot_objects_100k_unrelated us p50=" << times[50] << " p95=" << times[95]
            << " max=" << times.back() << '\n';
}
int main(int argc, char **argv) {
  explicit_authority_and_online_assembly();
  denials_and_offline_delivery();
  if (argc > 1 && std::string_view(argv[1]) == "--bench")
    benchmark();
  std::cout << "goods custody checks passed\n";
}
