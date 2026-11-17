#include "catalog/catalog.hpp"
#include "runtime_fixture.hpp"
#include "space_fixture.hpp"
#include "step/action_private.hpp"
#include "step/step.hpp"

#include <csignal>
#include <fstream>
#include <iostream>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace amber;
namespace ed = amber::step::editing;
using test_runtime::require;
using Json = nlohmann::json;
namespace {
step::NativeCall call(PlayerId player, Handle target, Json arguments) {
  return {{player, 0, target, {}}, std::move(arguments), {}, 10, 20, {}};
}
bool dirty(const step::NativeCompletion &out, Handle object, TermId term) {
  return std::any_of(out.effects.ordered.begin(), out.effects.ordered.end(), [&](const auto &row) {
    const auto *work = std::get_if<react::WatchWork>(&row);
    return work && work->source == FactKey{object, term};
  });
}
void priority_setting_actions_update_shares_immediately() {
  test_runtime::Fixture f;
  f.terms.rows.resize(13);
  for (TermId id : {10u, 11u, 12u}) {
    auto &term = f.terms.rows[id];
    term.id = id;
    term.name = id == 10 ? "novel.available" : id == 11 ? "novel.wanted" : "novel.priority";
    f.terms.by_name[term.name] = id;
  }
  f.terms.rows[10].combine = react::Combine::Sum;
  f.terms.rows[11].combine = react::Combine::SharePriority;
  f.terms.rows[11].paired = 10;
  f.terms.rows[11].priority = 12;
  f.terms.rows[12].source = react::Source::Setting;
  react::validate_shared_terms(f.terms);
  f.models.rows.resize(3);
  for (ModelId id : {1u, 2u}) {
    f.models.rows[id].id = id;
    f.models.rows[id].author = 1;
  }
  f.objects.model[f.b.slot()] = 2;
  const perm::Players players;
  const std::array<std::string, 1> roles{"architect"};
  const auto edit = [&](react::ValueAction operation, Handle target, Json arguments) {
    return step::native_values(players, f.models, f.values(), f.react_state, {}, roles, operation,
                               call(1, target, std::move(arguments)));
  };
  require(edit(react::ValueAction::ModelSetting, {},
               {{"model", "m:1"}, {"values", {{"novel.priority", 1}}}})
              .changed,
          "initial model priority is an ordinary native setting action");
  react::set_contribution(f.shared, f.gives_to, f.terms.rows[10], f.c, f.c, 4, f.effects);
  for (auto demander : {f.a, f.b})
    react::set_contribution(f.shared, f.gives_to, f.terms.rows[11], demander, f.c, 4, f.effects);
  react::reconcile_allocations(f.values(), f.react_state, f.effects, 10, 20);
  const auto share = [&](Handle object) {
    return react::get_share(f.allocations, f.c, 11, object);
  };
  require(share(f.a) == 4 && share(f.b) == 0, "the initial model priority receives the supply");
  const auto override =
      edit(react::ValueAction::SetSetting, f.b, {{"values", {{"novel.priority", 3}}}});
  require(override.changed && share(f.b) == 4 && share(f.a) == 0 && dirty(override, f.c, 11),
          "instance priority reallocates before the action returns and notifies demand watchers");
  const auto remove =
      edit(react::ValueAction::SetSetting, f.b, {{"values", {{"novel.priority", nullptr}}}});
  require(remove.changed && share(f.a) == 4 && share(f.b) == 0,
          "removing an instance priority immediately restores the inherited allocation");
  const auto model = edit(react::ValueAction::ModelSetting, {},
                          {{"model", "m:1"}, {"values", {{"novel.priority", -1}}}});
  require(model.changed && share(f.b) == 4 && share(f.a) == 0,
          "model priority edits immediately change their contributors' shares");
  const auto world =
      edit(react::ValueAction::WorldSetting, {}, {{"values", {{"novel.priority", -2}}}});
  require(world.changed && share(f.a) == 4 && share(f.b) == 0 &&
              react::read(f.values(), f.a, 12, 10, 20).value.numeric() == -1,
          "world priority edits reallocate inheriting contributors and preserve model overrides");
  require(
      !edit(react::ValueAction::WorldSetting, {}, {{"values", {{"novel.priority", -2}}}}).changed,
      "an unchanged world priority has no action effect");
}
} // namespace
int main() {
  try {
    priority_setting_actions_update_shares_immediately();
    test_runtime::Fixture f;
    auto add = [&](std::string name, react::Source source,
                   react::Kind kind = react::Kind::Quantity) {
      react::Term term;
      term.id = static_cast<TermId>(f.terms.rows.size());
      term.name = std::move(name);
      term.source = source;
      term.kind = kind;
      f.terms.by_name[term.name] = term.id;
      f.terms.rows.push_back(term);
      return term.id;
    };
    const auto gain = add("novel.gain", react::Source::Setting);
    const auto density = add("novel.density", react::Source::Intrinsic);
    const auto transmission = add("novel.transmission", react::Source::Intrinsic);
    const auto mechanical = add("novel.attachment", react::Source::State, react::Kind::Relation);
    const auto connection = add("novel.connection", react::Source::State, react::Kind::Relation);
    react::NativeActions native;
    const auto declarations = ed::declare_editing(f.terms, native);
    const auto count = f.terms.rows.size();
    require(ed::declare_editing(f.terms, native).install == declarations.install &&
                f.terms.rows.size() == count,
            "restored reserved vocabulary changed identities");
    require(step::check_input(native.rows.at(declarations.install).inputs_schema,
                              {{"script", "s:1"}, {"choices", Json::array()}})
                    .empty() &&
                !step::check_input(native.rows.at(declarations.install).inputs_schema,
                                   {{"script", "s:1"}, {"unknown", 1}})
                     .empty() &&
                !step::check_input(native.rows.at(declarations.edit_detail).inputs_schema,
                                   {{"id", 0}, {"text", "invalid"}})
                     .empty(),
            "native schema validation lost typed input rules");
    f.objects.owner[f.a.slot()] = f.objects.owner[f.b.slot()] = 1;
    f.objects.owner[f.c.slot()] = 2;
    f.models.rows[1].author = 1;
    f.models.rows.resize(3);
    f.models.rows[2].id = 2;
    f.models.rows[2].author = 2;
    f.objects.model[f.c.slot()] = 2;
    amber::perm::State perm_state;
    perm::Players &players = perm_state.players;
    players.rows.resize(4);
    perm::Grants &grants = perm_state.grants;

    perm::OwnerLimits &limits = perm_state.owner_limits;

    const std::vector<std::string> no_roles, elaborator{"elaborator"};
    const auto values = f.values();
    {
      const auto body_controls = f.add(R"(return {kind='instance',
        slots={'use','take','move','animate','camera'},on={}})",
                                       1, {{"use", "take", "move", "animate", "camera"}});
      const auto flags_before = f.objects.flags[f.b.slot()];
      f.objects.flags[f.b.slot()] |= Body;
      Json choices = Json::array();
      for (const auto *right : {"use", "take", "move", "animate", "camera"})
        choices.push_back(
            {{"slot", right},
             {"scope",
              {{std::string(right) == "animate" || std::string(right) == "camera" ? "avatar"
                                                                                  : "object",
                to_string(f.b)}}},
             {"rights", {right}},
             {"expires_s", 20}});
      const auto install_call =
          call(1, f.b, {{"script", "s:" + std::to_string(body_controls)}, {"choices", choices}});
      const auto grouped = ed::install_script(install_call, f.react_state, f.catalog_state,
                                              f.objects_state, perm_state, 6);
      require(grouped.reply.at("ok") && grouped.reply.at("grants").size() == 5 &&
                  grouped.reply.at("unfilled_slots").empty(),
              "body slots sharing a scope must all fill one ordinary grant");
      const auto &rows = grouped.reply.at("grants");
      const auto object_grant = step::identifier(rows[0].at("grant"), "g:");
      const auto avatar_grant = step::identifier(rows[3].at("grant"), "g:");
      require(rows[1].at("grant") == rows[0].at("grant") &&
                  rows[2].at("grant") == rows[0].at("grant") &&
                  rows[4].at("grant") == rows[3].at("grant") && object_grant != avatar_grant,
              "one holder/scope must have one grant with the explicitly combined rights");
      const auto holder = perm::Holder::script(body_controls, f.b);
      require(bool(perm::check(holder, perm::Verb::Take,
                               perm::target(identity(f.objects), authority(f.objects), limits,
                                            perm_state.plots, f.b),
                               players, grants, 10)) &&
                  bool(perm::check_scope(holder, perm::Scope::avatar(f.b), 3, grants, 10)),
              "explicit body consent did not authorize the requested ordinary rights");
      auto conflicting = choices;
      conflicting[1]["expires_s"] = 21;
      const auto before = grants.rows.size();
      const auto rejected = ed::install_script(
          call(1, f.b,
               {{"script", "s:" + std::to_string(body_controls)}, {"choices", conflicting}}),
          f.react_state, f.catalog_state, f.objects_state, perm_state, 6);
      require(!rejected.reply.at("ok").get<bool>() && grants.rows.size() == before,
              "conflicting limits on one grant scope were silently changed");
      perm::revoke(grants, players, object_grant);
      require(!perm::check(holder, perm::Verb::Take,
                           perm::target(identity(f.objects), authority(f.objects), limits,
                                        perm_state.plots, f.b),
                           players, grants, 10) &&
                  !perm::check_scope(holder, perm::Scope::avatar(f.b), 3, grants, 410),
              "body authority survived revocation or finite expiry");
      f.objects.flags[f.b.slot()] = flags_before;
    }
    const auto script = f.add(R"(return {kind='instance',slots={'edit'},
      watches={'novel.gain'},contributes={'novel.signal'},
      actions={['novel.touch']={right='use'}},on={
      ['novel.gain']=function(self) self:set('novel.result',self:setting('novel.gain')) end,
      ['novel.cue']=function(self) self.state.started=true end,
      ['novel.touch']=function(self) self:set('novel.result',77) end}})");
    const auto sid = "s:" + std::to_string(script);
    auto install = [&](step::NativeCall input) {
      return ed::install_script(input, f.react_state, f.catalog_state, f.objects_state, perm_state,
                                6);
    };
    auto default_script = [&](Json selected) {
      return ed::model_script(call(1, {}, {{"model", "m:1"}, {"script", selected}}), f.react_state,
                              values, f.catalog_state, perm_state, 6);
    };
    const auto preview = install(call(1, f.a, {{"script", sid}}));
    require(!preview.changed && preview.reply.at("requires_commit") && !f.installed.get(f.a.slot()),
            "preview installed behavior");
    const Json choice = {{"slot", "edit"},
                         {"scope", {{"object", to_string(f.a)}}},
                         {"rights", {"edit"}},
                         {"expires_s", 30}};
    const auto installed = install(call(1, f.a, {{"script", sid}, {"choices", {choice}}}));
    require(installed.changed && installed.permissions_changed,
            "explicit installation did not report its granted authority");
    const auto grant = step::identifier(installed.reply.at("grants")[0].at("grant"), "g:");
    require(grants.rows[grant].holder == perm::Holder::script(script, f.a) &&
                grants.rows[grant].expires_step == 610,
            "slot grant lost its actual holder or bounded expiry");
    require(std::any_of(installed.effects.ordered.begin(), installed.effects.ordered.end(),
                        [&](const auto &row) {
                          const auto *event = std::get_if<react::EventWork>(&row);
                          return event && event->event == 6 && event->recipient_script == script;
                        }),
            "installation did not return a targeted ordinary startup event");
    require(default_script(sid).changed &&
                react::installed_scripts(f.models, f.installed, identity(f.objects), f.a).size() ==
                    1,
            "default plus instance installation duplicated actual behavior");
    f.states[{f.a, script}].insert(std::string("kept"), true);
    const auto uninstalled = ed::uninstall_script(call(1, f.a, {{"script", sid}}), f.react_state,
                                                  values, f.catalog_state, perm_state, 6);
    require(uninstalled.changed && f.states.contains({f.a, script}) && grants.rows[grant].live,
            "removing duplicate installation erased surviving default state or rights");
    require(default_script(nullptr).changed && !f.states.contains({f.a, script}) &&
                !grants.rows[grant].live,
            "removing last behavior retained its state or authority");
    (void)default_script(sid);
    const auto setting =
        ed::model_setting(call(1, {}, {{"model", "m:1"}, {"values", {{"novel.gain", 2}}}}),
                          f.react_state, values, f.models, players);
    require(setting.changed && dirty(setting, f.a, gain) && dirty(setting, f.b, gain) &&
                !dirty(setting, f.c, gain),
            "model setting did not update the effective settings of its actual instances");
    f.run(f.changed({f.a, gain}, 10), 10);
    require(react::read(values, f.a, 2, 10, 20).value.numeric() == 2,
            "direct setting did not drive the actual installed Luau watch");
    const auto override =
        ed::set_setting(call(1, f.a, {{"values", {{"novel.gain", 3}}}}), f.react_state, values);
    require(override.changed && dirty(override, f.a, gain), "instance setting did not dirty fact");
    f.run(f.changed({f.a, gain}, 10), 10);
    require(react::read(values, f.a, 2, 10, 20).value.numeric() == 3,
            "instance setting did not reach the installed Luau watch");
    const auto clear = ed::set_setting(call(1, f.a, {{"values", {{"novel.gain", nullptr}}}}),
                                       f.react_state, values);
    require(clear.changed && react::read(values, f.a, gain, 10, 20).value.numeric() == 2,
            "removing override failed to expose model default");
    f.run(f.changed({f.a, gain}, 10), 10);
    require(react::read(values, f.a, 2, 10, 20).value.numeric() == 2,
            "removing an override did not notify the watch of its inherited value");
    const std::array<std::string, 1> architect{"architect"};
    auto world_change = call(1, {}, {{"values", {{"novel.gain", 4}}}});
    require(!ed::world_setting(world_change, f.react_state, values, {}).reply.at("ok") &&
                !f.world.values.contains(gain),
            "ordinary player changed world settings");
    world_change.request.caller = ScriptHolder{script, f.a};
    require(!ed::world_setting(world_change, f.react_state, values, architect).reply.at("ok") &&
                !f.world.values.contains(gain),
            "script inherited its author's architect authority");
    world_change.request.caller = PlayerId{1};
    const auto world_default = ed::world_setting(world_change, f.react_state, values, architect);
    require(world_default.changed && dirty(world_default, f.c, gain) &&
                !dirty(world_default, f.a, gain) && !dirty(world_default, f.b, gain) &&
                react::read(values, f.c, gain, 10, 20).value.numeric() == 4 &&
                react::read(values, f.a, gain, 10, 20).value.numeric() == 2,
            "world setting did not change inheriting objects while preserving overrides");
    require(!ed::world_setting(world_change, f.react_state, values, architect).changed,
            "repeating the same world default reported a change");
    const auto invalid_world =
        ed::world_setting(call(1, {}, {{"values", {{"novel.gain", 5}, {"novel.result", 6}}}}),
                          f.react_state, values, architect);
    require(!invalid_world.reply.at("ok") && f.world.values.at(gain).numeric() == 4,
            "invalid world settings partly applied or wrote a state quantity");
    const auto removed_world = ed::world_setting(
        call(1, {}, {{"values", {{"novel.gain", nullptr}}}}), f.react_state, values, architect);
    require(removed_world.changed && !react::read(values, f.c, gain, 10, 20).present &&
                react::read(values, f.a, gain, 10, 20).value.numeric() == 2,
            "removing a world default damaged a model override");
    catalog::ModelTexts model_text;
    catalog::InstanceTexts text;
    catalog::AuthoredTexts details;
    model_text.rows[1] = {"Name", "Description", "Writing"};
    require(ed::edit_text(call(1, f.a, {{"text", {{"name", ""}}}}), text, model_text,
                          identity(f.objects))
                    .changed &&
                text.rows.at(f.a.bits).name.empty() &&
                text.rows.at(f.a.bits).description == "Description",
            "partial text edit dropped inherited fields or could not clear text");
    require(!ed::model_text(call(2, {}, {{"model", "m:1"}, {"text", {{"name", "Foreign"}}}}),
                            model_text, f.models, identity(f.objects), players)
                 .reply.at("ok")
                 .get<bool>(),
            "model metadata ignored actual authorship");
    f.models.rows[1].group = 1;
    players.rows[2].groups = {1};
    require(ed::model_text(call(2, {}, {{"model", "m:1"}, {"text", {{"name", "Shared"}}}}),
                           model_text, f.models, identity(f.objects), players)
                    .changed &&
                !ed::model_text(call(3, {}, {{"model", "m:1"}, {"text", {{"name", "Denied"}}}}),
                                model_text, f.models, identity(f.objects), players)
                     .reply.at("ok")
                     .get<bool>(),
            "model group members can edit defaults and nonmembers cannot");
    const auto id = catalog::append_authored_text(details, f.a, 3, 4.5,
                                                  catalog::AuthoredTextKind::Detail, "Original");
    const auto detail = ed::edit_detail(call(1, f.a, {{"id", id}, {"text", "Revised"}}), details);
    require(detail.changed && detail.changed_objects == std::vector<Handle>{f.a} &&
                details.rows[id].author == 3 && details.rows[id].created_s == 4.5,
            "detail lost original provenance or exact invalidation delta");
    const auto related = ed::relate(
        call(1, f.a, {{"term", "novel.connection"}, {"other", to_string(f.c)}, {"present", true}}),
        f.relations, f.relation_view, values, {mechanical, 0, 0, 0});
    require(related.changed && dirty(related, f.a, connection) && dirty(related, f.c, connection),
            "relation edit did not return both real endpoint facts");
    require(!ed::relate(
                 call(1, f.a,
                      {{"term", "novel.attachment"}, {"other", to_string(f.c)}, {"present", true}}),
                 f.relations, f.relation_view, values, {mechanical, 0, 0, 0})
                 .reply.at("ok")
                 .get<bool>(),
            "editor forged a mechanical relation");
    auto script_call = call(1, f.a, {{"values", {{"novel.heat", 5}}}});
    script_call.request.caller = ScriptHolder{script, f.a};
    require(!ed::patch_state(script_call, f.properties, f.processes, values, elaborator)
                 .reply.at("ok")
                 .get<bool>(),
            "script inherited a player's elaborator role");
    space::Space physics(testspace::settings());
    step::GeometryAssets geometry;
    auto &baked = f.catalog_state.assets;

    testspace::Box box({1, 1, 1});
    catalog::Asset asset;
    asset.hash = std::string(64, 'a');
    asset.volume = 1;
    asset.manifest = {{"bindings", Json::object()}};
    baked.rows.emplace(asset.hash, asset);
    geometry.rows[asset.hash].collision = space::import_geometry(physics, box.source()).geometry;
    f.materials.rows[8] = {{density, Value(1.)}, {transmission, Value(0.)}};
    for (auto object : {f.a, f.b, f.c}) {
      f.models.rows[f.objects.model[object.slot()]].asset = asset.hash;
      const auto definition = step::describe_physics(
          geometry, asset, values, {f.objects.model[object.slot()], 0, 8, 0, object},
          f.objects.flags[object.slot()], {density, transmission}, {.6f, 0},
          amber::step::Clock{.step = 0, .hz = 20});
      step::install_physics(physics, identity(f.objects), transforms(f.objects),
                            space_flags(f.objects), object, definition);
    }
    auto &mechanical_roles = f.catalog_state.mechanical;
    auto &registry = f.catalog_state.registry;
    auto directory = std::filesystem::current_path() / ".cache/editing-registry-test";
    std::filesystem::remove_all(directory);
    const Json proposal = {{"name", "separate_heat"}, {"kind", "quantity"}, {"source", "state"}};
    const auto definition = ed::define_term(
        call(3, {}, {{"proposal", proposal}, {"choice", {{"none_fits", "Separate phenomenon"}}}}),
        elaborator, f.terms, f.materials, f.rules, mechanical_roles, registry, directory);
    require(
        definition.changed && definition.changed_terms.size() == 1 &&
            catalog::read_registry(directory, registry.hash).data.at("terms").back().at("name") ==
                "p3.separate_heat",
        "term edit lost actual registry pin or author namespace");
    const auto previous = registry.hash;
    auto material =
        ed::define_material(call(3, {},
                                 {{"material", "novel.mat"},
                                  {"values", {{"novel.density", -1}, {"novel.transmission", 0}}}}),
                            elaborator, f.react_state, values, f.catalog_state, geometry, physics,
                            {density, transmission}, {.6f, 0}, directory);
    require(!material.reply.at("ok").get<bool>() && material.changed && registry.hash != previous &&
                f.materials.rows.at(8).at(density).numeric() == -1 && dirty(material, f.a, density),
            "failed native refresh must retain and report the applied material edit");
    material =
        ed::define_material(call(3, {},
                                 {{"material", "novel.mat"},
                                  {"values", {{"novel.density", 4}, {"novel.transmission", 0}}}}),
                            elaborator, f.react_state, values, f.catalog_state, geometry, physics,
                            {density, transmission}, {.6f, 0}, directory);
    require(material.changed && dirty(material, f.a, density) &&
                material.changed_materials == std::vector<TermId>{8},
            "material update did not return actual facts and material delta");
    const auto blocked = directory / "not-a-directory";
    std::ofstream(blocked) << "deliberately blocked registry directory";
    const auto child = fork();
    require(child >= 0, "registry disk-failure child could not start");
    if (child == 0) {
      const rlimit no_core{0, 0};
      setrlimit(RLIMIT_CORE, &no_core);
      (void)ed::persist_registry(registry, blocked, f.terms, f.materials, f.rules,
                                 mechanical_roles);
      _exit(1);
    }
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
                WTERMSIG(status) == SIGABRT,
            "critical registry publication failure continued under an old pin");
    std::filesystem::remove_all(directory);
    std::cout << "Direct editing: real VM watches/installations, explicit grants, exact deltas, "
                 "authorship, immutable registry and native material checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
