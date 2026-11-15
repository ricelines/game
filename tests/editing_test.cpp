#include "bindings/editing/editing.hpp"
#include "bindings/geometry/geometry.hpp"
#include "bindings/illumination.hpp"
#include "bindings/metadata/metadata.hpp"
#include "bindings/viewing/viewing.hpp"
#include "runtime_fixture.hpp"
#include "space_fixture.hpp"
#include "step/snapshot.hpp"

#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace amber;
namespace ed = amber::bindings::editing;
using test_runtime::require;
using Json = nlohmann::json;
namespace {
drive::NativeCall call(PlayerId caller, Handle target, Json arguments) {
  return {{caller, 0, target, {}}, std::move(arguments), {}, 10, 20, {}};
}
bool dirty(const drive::NativeCompletion &out, Handle object, TermId term) {
  return std::any_of(out.effects.ordered.begin(), out.effects.ordered.end(),
                     [&](const auto &effect) {
                       const auto *watch = std::get_if<step::WatchWork>(&effect);
                       return watch && watch->source == FactKey{object, term};
                     });
}
} // namespace
int main() {
  try {
    test_runtime::Fixture f;
    auto add = [&](std::string name, react::Source source,
                   react::Kind kind = react::Kind::Quantity) {
      const auto id = static_cast<TermId>(f.terms.rows.size());
      react::Term term;
      term.id = id;
      term.name = std::move(name);
      term.source = source;
      term.kind = kind;
      f.terms.by_name[term.name] = id;
      f.terms.rows.push_back(std::move(term));
      return id;
    };
    const auto gain = add("novel.gain", react::Source::Setting);
    const auto intrinsic = add("novel.hardness", react::Source::Intrinsic);
    const auto transmission = add("novel.transmission", react::Source::Intrinsic);
    const auto measured = add("novel.volume", react::Source::Measured);
    const auto mechanical = add("novel.attachment", react::Source::State, react::Kind::Relation);
    const auto connection = add("novel.connection", react::Source::State, react::Kind::Relation);
    command::NativeActions native;
    const auto declared = ed::declare_editing(f.terms, native);
    const auto declaration_count = f.terms.rows.size();
    require(ed::declare_editing(f.terms, native).install == declared.install &&
                f.terms.rows.size() == declaration_count,
            "restore duplicated reserved native vocabulary");
    f.terms.rows[declared.install].owner = 123;
    bool reserved_rejected = false;
    try {
      ed::declare_editing(f.terms, native);
    } catch (const std::exception &) {
      reserved_rejected = true;
    }
    f.terms.rows[declared.install].owner = 0;
    require(reserved_rejected, "restore accepted forged reserved native metadata");
    const react::RightsDeclaration rights{{"use", "edit"}, {"spend", "animate", "camera"}};
    const auto wallet_slot =
        catalog::prepare_script("return {kind='instance',slots={'spend'},on={}}", f.terms, rights,
                                catalog::sha256("editing-registry"), {}, {4096, 50, 5, 64});
    require(wallet_slot.declaration.slots == std::vector<std::string>{"spend"},
            "real validator rejected a financial slot");
    const auto bad_action = react::analyze_script(
        "return {kind='instance',actions={['novel.touch']={right='spend'}},on={}}", f.terms, rights,
        {4096, 50, 5, 64});
    require(!bad_action.errors.empty(),
            "financial slot vocabulary leaked into object action rights");
    require(native.rows.size() == 12 &&
                native.rows.at(declared.install).inputs_schema["properties"]["choices"]["type"] ==
                    "array",
            "native declarations lost their structured input schema");
    f.objects.owner[f.a.slot()] = 1;
    f.objects.owner[f.b.slot()] = 1;
    f.objects.owner[f.c.slot()] = 2;
    f.models.rows[1].author = 1;
    f.models.rows.resize(3);
    f.models.rows[2].id = 2;
    f.models.rows[2].author = 2;
    f.objects.model[f.c.slot()] = 2;
    perm::Players players;
    players.rows.resize(4);
    perm::Grants grants;
    perm::Groups groups;
    perm::OwnerLimits limits;
    perm::Plots plots;
    control::Profiles profiles;
    profiles.rows.resize(4);
    for (PlayerId id = 1; id < 4; ++id)
      profiles.rows[id].id = id;
    profiles.rows[3].roles = {"elaborator"};
    const auto values = f.values();
    react::register_binding(f.vm, "setting", react::setting_binding, &*f.reads);
    react::GivesTo gives;
    observe::LightSubscriptions light_points{8};
    bindings::viewing::SessionViews views;
    bindings::IlluminanceReads illuminance{light_points, f.subscriptions, identity(f.objects),
                                           transforms(f.objects), 0};
    bindings::MetadataChanges changes{views, illuminance, {}, {}};
    const ed::EditAccess access{identity(f.objects), authority(f.objects), limits, players, grants};
    ed::ScriptEditing scripts{f.installed, f.triggers, f.subscriptions, f.links,
                              f.states,    f.shared,   gives,           f.allocations,
                              values,      f.models,   f.scripts,       authority(f.objects),
                              limits,      players,    grants,          plots,
                              profiles,    6,          changes,         groups};
    catalog::ModelTexts model_text;
    catalog::InstanceTexts instance_text;
    catalog::AuthoredTexts details;
    ed::TextEditing texts{instance_text, model_text, details, f.models, access, changes};
    ed::PropertyEditing properties{
        f.instance_settings,   f.model_settings, f.properties, f.processes, f.relations,
        f.relation_view,       values,           f.models,     profiles,    access,
        {mechanical, 0, 0, 0}, changes};
    const auto registry_directory =
        std::filesystem::current_path() / ".cache/editing-registry-test";
    std::filesystem::remove_all(registry_directory);
    react::PriorityLinks priorities;
    bindings::CatalogPublication publication{f.models,
                                             f.scripts,
                                             f.terms,
                                             f.materials,
                                             model_text,
                                             instance_text,
                                             values,
                                             authority(f.objects),
                                             parents(f.objects),
                                             f.triggers,
                                             priorities,
                                             changes,
                                             {}};
    catalog::MechanicalBindings mechanical_roles;
    catalog::RegistryDocument document;
    bindings::RegistryPersistence persistence{
        f.terms,  f.materials,        f.rules,    f.scripts, mechanical_roles,
        document, registry_directory, publication};
    space::Space physics(testspace::settings());
    bindings::GeometryAssets geometries;
    catalog::Assets baked;
    catalog::ParameterBlocks parameters;
    testspace::Box box({1, 1, 1});
    catalog::Asset box_asset;
    box_asset.hash = std::string(64, 'a');
    box_asset.volume = 1;
    box_asset.manifest = {{"bindings", Json::object()}};
    baked.rows.emplace(box_asset.hash, box_asset);
    geometries.rows[box_asset.hash].collision =
        space::import_geometry(physics, box.source()).geometry;
    f.materials.rows[8] = {{intrinsic, Value(1.)}, {transmission, Value(0.)}};
    f.models.rows[1].asset = box_asset.hash;
    f.models.rows[2].asset = box_asset.hash;
    bindings::MaterialRefresh physical_materials{
        physics, geometries, f.models, parameters, baked, values, {intrinsic, transmission},
        {.6f, 0}};
    for (auto object : {f.a, f.b, f.c}) {
      const auto physical = bindings::describe_physics(
          geometries, box_asset, values, {f.objects.model[object.slot()], 0, 8, 0, object},
          f.objects.flags[object.slot()], {intrinsic, transmission}, {.6f, 0}, 0, 20);
      bindings::install_physics(physics, identity(f.objects), transforms(f.objects),
                                space_flags(f.objects), object, physical);
    }
    ed::RegistryEditing registry{f.terms, f.materials,       values, profiles, persistence,
                                 changes, physical_materials};
    drive::NativeOperations operations;
    ed::register_actions(operations, declared, scripts, texts, properties, registry);
    require(operations.rows.size() == 12 &&
                operations.rows.at(declared.install).context == &scripts &&
                operations.rows.at(declared.model_script).context == &scripts &&
                operations.rows.at(declared.edit_text).context == &texts &&
                operations.rows.at(declared.edit_detail).context == &texts &&
                operations.rows.at(declared.model_text).context == &texts &&
                operations.rows.at(declared.set_setting).context == &properties &&
                operations.rows.at(declared.model_setting).context == &properties &&
                operations.rows.at(declared.patch_state).context == &properties &&
                operations.rows.at(declared.relate).context == &properties &&
                operations.rows.at(declared.define_term).context == &registry &&
                operations.rows.at(declared.define_material).context == &registry,
            "editing actions did not share their four capability contexts");
    const auto invoke = [&](TermId action, drive::NativeCall request) {
      request.request.action = action;
      return drive::invoke_native(operations, std::move(request));
    };
    const auto validate = [&](TermId action, const Json &arguments) {
      return operations.rows.at(action).validate(arguments);
    };
    require(validate(declared.install, {{"script", "s:1"}, {"choices", Json::array()}}).empty() &&
                !validate(declared.install, {{"script", "s:1"}, {"unknown", 1}}).empty() &&
                validate(declared.edit_detail, {{"id", 1}, {"text", "revised"}}).empty() &&
                !validate(declared.edit_detail, {{"id", 0}, {"text", "revised"}}).empty() &&
                !validate(declared.edit_detail, {{"id", 1.5}, {"text", "revised"}}).empty() &&
                !validate(declared.patch_state, {{"values", {{"novel.heat", "not a quantity"}}}})
                     .empty(),
            "native structured input validation incorrect");
    const auto script = f.add(R"(return {kind='instance',slots={'edit'},
      watches={'novel.gain'},contributes={'novel.signal'},
      actions={['novel.touch']={right='use'}},on={
      ['novel.gain']=function(self)self:set('novel.result',self:setting('novel.gain'))end,
      ['novel.cue']=function(self)self.state.started=(self.state.started or 0)+1 end,
      ['novel.touch']=function(self)self:set('novel.result',77)end}})");
    const auto sid = "s:" + std::to_string(script);
    const auto preview = invoke(declared.install, call(1, f.a, {{"script", sid}}));
    require(preview.reply["ok"] && !preview.changed && preview.reply["requires_commit"] &&
                !f.installed.get(f.a.slot()),
            "preview installed a script or hid its slots");
    const Json choice = {{"slot", "edit"},
                         {"scope", {{"object", to_string(f.a)}}},
                         {"rights", {"edit"}},
                         {"expires_s", 30}};
    const auto installed = invoke(
        declared.install, call(1, f.a, {{"script", sid}, {"choices", Json::array({choice})}}));
    require(installed.changed && installed.reply["grants"].size() == 1 &&
                f.subscriptions.by_consumer.contains({f.a, script}),
            "actual script/watch/ordinary slot grant not installed");
    const auto grant = installed.reply["grants"][0]["grant"].get<GrantId>();
    require(grants.rows[grant].holder == perm::Holder::script(script, f.a) &&
                grants.rows[grant].expires_step == 610 && grants.rows[grant].issuer == 1,
            "slot grant borrowed hidden authority or lost its expiry");
    bool started = false;
    for (const auto &effect : installed.effects.ordered)
      if (const auto *event = std::get_if<step::EventWork>(&effect);
          event && event->event == 6 && event->recipient_script == script)
        started = true;
    require(started, "new installation did not emit its configured startup event");
    require(!invoke(declared.install, call(2, f.a, {{"script", sid}, {"choices", Json::array()}}))
                 .reply["ok"]
                 .get<bool>(),
            "foreign installation ignored current Edit permission");
    const auto model_installed =
        invoke(declared.model_script, call(1, {}, {{"model", "m:1"}, {"script", sid}}));
    require(
        model_installed.changed &&
            install::installed_scripts(f.models, f.installed, identity(f.objects), f.a).size() ==
                1 &&
            react::dispatch(f.triggers, values, f.a, 7, 10, 20).size() == 1,
        "same default/instance script executed twice");
    f.states[{f.a, script}][std::string("kept")] = true;
    require(invoke(declared.uninstall, call(1, f.a, {{"script", sid}})).changed &&
                f.states.contains({f.a, script}) && grants.rows[grant].live,
            "removing duplicate installation erased the remaining default's state/grant");
    react::set_contribution(f.shared, gives, f.terms.rows[4], f.a, f.b, 5, f.effects);
    const auto removed =
        invoke(declared.model_script, call(1, {}, {{"model", "m:1"}, {"script", nullptr}}));
    require(
        removed.changed && !f.states.contains({f.a, script}) && !grants.rows[grant].live &&
            !f.subscriptions.by_consumer.contains({f.a, script}) &&
            react::read_shared(f.shared, f.relation_view, f.b, f.terms.rows[4]).value.numeric() ==
                0 &&
            react::dispatch(f.triggers, values, f.a, 7, 10, 20).empty(),
        "removed behavior retained state, grants, contributions or actions");
    auto foreign_choice = choice;
    foreign_choice["scope"] = {{"object", to_string(f.c)}};
    const auto limited =
        invoke(declared.install,
               call(1, f.a, {{"script", sid}, {"choices", Json::array({foreign_choice})}}));
    require(limited.changed && limited.reply["grants"].empty() &&
                limited.reply["unfilled_slots"].size() == 1,
            "installation invented foreign grant rights");
    model_text.rows[1] = {"Default name", "Default description", "Default writing"};
    require(invoke(declared.edit_text, call(1, f.a, {{"text", {{"name", ""}}}})).changed &&
                instance_text.rows.at(f.a.bits).name.empty() &&
                instance_text.rows.at(f.a.bits).description == "Default description",
            "partial claim edit lost inherited fields or could not clear text");
    const auto detail_id = catalog::append_authored_text(
        details, f.a, 3, 4.5, catalog::AuthoredTextKind::Detail, "Original detail");
    const auto refusal_id = catalog::append_authored_text(
        details, f.a, 3, 5, catalog::AuthoredTextKind::Refusal, "Original refusal");
    require(!invoke(declared.edit_detail, call(2, f.a, {{"id", detail_id}, {"text", "Denied"}}))
                    .reply["ok"]
                    .get<bool>() &&
                !invoke(declared.edit_detail,
                        call(1, f.b, {{"id", detail_id}, {"text", "Wrong target"}}))
                     .reply["ok"]
                     .get<bool>() &&
                !invoke(declared.edit_detail,
                        call(1, f.a, {{"id", refusal_id}, {"text", "Not a detail"}}))
                     .reply["ok"]
                     .get<bool>(),
            "detail rewrite bypassed Edit, target identity or public-detail kind");
    views.rows[1].body = f.a;
    views.rows[1].pending.ready = true;
    const auto rewritten_detail =
        invoke(declared.edit_detail, call(1, f.a, {{"id", detail_id}, {"text", "Revised detail"}}));
    require(rewritten_detail.changed && details.rows[detail_id].text == "Revised detail" &&
                details.rows[detail_id].author == 3 && details.rows[detail_id].created_s == 4.5 &&
                rewritten_detail.reply["author"] == "p:3" && changes.objects.back() == f.a &&
                !views.rows[1].pending.ready &&
                !invoke(declared.edit_detail,
                        call(1, f.a, {{"id", detail_id}, {"text", "Revised detail"}}))
                     .changed,
            "ordinary detail rewrite lost original provenance, cache invalidation or idempotence");
    require(
        !invoke(declared.model_text, call(2, {}, {{"model", "m:1"}, {"text", {{"name", "Other"}}}}))
                .reply["ok"]
                .get<bool>() &&
            invoke(declared.model_text,
                   call(1, {}, {{"model", "m:1"}, {"text", {{"name", "New default"}}}}))
                .changed,
        "model text ignored actual authorship");
    auto changed = invoke(declared.model_setting,
                          call(1, {}, {{"model", "m:1"}, {"values", {{"novel.gain", 2}}}}));
    require(changed.changed && dirty(changed, f.a, gain) && dirty(changed, f.b, gain) &&
                !dirty(changed, f.c, gain),
            "model setting failed to project actual current instances");
    auto deliveries = f.changed({f.a, gain}, 10);
    f.run(deliveries, 10);
    require(react::read(values, f.a, 2, 10, 20).value.numeric() == 2,
            "native model setting did not drive actual installed Luau watch");
    require(invoke(declared.set_setting, call(1, f.a, {{"values", {{"novel.gain", 3}}}})).changed,
            "ordinary instance setting denied");
    changed = invoke(declared.model_setting,
                     call(1, {}, {{"model", "m:1"}, {"values", {{"novel.gain", 4}}}}));
    require(!dirty(changed, f.a, gain) && dirty(changed, f.b, gain) &&
                react::read(values, f.a, gain, 10, 20).value.numeric() == 3,
            "instance override did not retain precedence");
    changed = invoke(declared.set_setting, call(1, f.a, {{"values", {{"novel.gain", nullptr}}}}));
    require(dirty(changed, f.a, gain) &&
                react::read(values, f.a, gain, 10, 20).value.numeric() == 4,
            "removing instance override did not expose live model default");
    for (const auto &name :
         {f.terms.rows[intrinsic].name, f.terms.rows[measured].name, std::string("novel.heat")})
      require(!invoke(declared.set_setting, call(1, f.a, {{"values", {{name, 99}}}}))
                   .reply["ok"]
                   .get<bool>(),
              "builder could set intrinsic/measured/state through settings");
    require(!invoke(declared.patch_state, call(1, f.a, {{"values", {{"novel.heat", 20}}}}))
                    .reply["ok"]
                    .get<bool>() &&
                !invoke(declared.patch_state, call(3, f.c, {{"values", {{"novel.heat", 20}}}}))
                     .reply["ok"]
                     .get<bool>(),
            "state repair bypassed role or current Edit permission");
    f.objects.preset[f.c.slot()] = perm::bit(perm::Verb::Edit);
    require(
        invoke(declared.patch_state, call(3, f.c, {{"values", {{"novel.heat", 20}}}})).changed &&
            react::read(values, f.c, 1, 10, 20).value.numeric() == 20,
        "authorized elaborator could not repair current state");
    changed = invoke(
        declared.relate,
        call(1, f.a, {{"term", "novel.connection"}, {"other", to_string(f.c)}, {"present", true}}));
    require(changed.changed && dirty(changed, f.a, connection) && dirty(changed, f.c, connection) &&
                f.relation_view.columns.at(connection)->out.get(f.a.slot())->front() == f.c,
            "authored outgoing relation did not use real endpoints/projection/effects");
    require(
        !invoke(declared.relate,
                call(1, f.a,
                     {{"term", "novel.attachment"}, {"other", to_string(f.c)}, {"present", true}}))
             .reply["ok"]
             .get<bool>(),
        "editor could forge Space mechanical relation");
    const auto stale = Handle::make(f.c.slot(), f.c.generation() + 1);
    require(!invoke(declared.relate, call(1, f.a,
                                          {{"term", "novel.connection"},
                                           {"other", to_string(stale)},
                                           {"present", true}}))
                 .reply["ok"]
                 .get<bool>(),
            "editor accepted a stale generation");
    require(!changes.objects.empty() && !changes.models.empty(),
            "edits never invalidated presentation caches");
    auto run_targeted = [&](const step::EventWork &event) {
      const auto planned = drive::plan_event(f.triggers, values, event, 10, 20);
      for (const auto &plan : planned) {
        require(!plan.input.action_caller, "startup inherited installation player's authority");
        const install::InvocationPlan invocation{plan.handler,       plan.input.receiver,
                                                 plan.input.sender,  plan.input.trigger,
                                                 plan.input.payload, plan.input.acquired};
        f.run(std::span{&invocation, 1}, 10);
      }
      return planned.size();
    };
    require(run_targeted({f.a, {}, 6, {}, {}, script}) == 1, "existing script startup did not run");
    const auto second_script = f.add(
        "return {kind='instance',on={['novel.cue']=function(self)self.state.started=true end}}");
    const auto second_install = invoke(
        declared.install,
        call(1, f.a,
             {{"script", "s:" + std::to_string(second_script)}, {"choices", Json::array()}}));
    step::EventWork targeted;
    for (const auto &effect : second_install.effects.ordered)
      if (const auto *event = std::get_if<step::EventWork>(&effect); event && event->event == 6)
        targeted = *event;
    require(targeted.recipient_script == second_script && run_targeted(targeted) == 1 &&
                std::get<double>(f.states.at({f.a, script}).at(std::string("started"))) == 1 &&
                std::get<bool>(f.states.at({f.a, second_script}).at(std::string("started"))),
            "installing new behavior replayed an existing startup");
    step::WorkQueues queued;
    step::TimerWheel timers;
    step::Intents intents;
    step::enqueue_event(queued, targeted, 128);
    step::schedule(timers, {20, targeted, {}});
    snap::Buffer buffer;
    snap::Output output{buffer};
    step::write_queues(output, queued, intents, 1);
    step::write_timers(output, timers);
    output.adapter().flush();
    buffer.resize(output.adapter().writtenBytesCount());
    step::WorkQueues restored;
    step::TimerWheel restored_timers;
    step::Intents restored_intents;
    snap::Input input{buffer.data(), buffer.size()};
    step::read_queues(input, restored, restored_intents);
    step::read_timers(input, restored_timers);
    snap::check_complete(input);
    require(std::get<step::EventWork>(restored.causal.front().work).recipient_script ==
                    second_script &&
                step::timers(restored_timers).front().event.recipient_script == second_script,
            "snapshot lost internal targeted event routing");
    require(
        invoke(declared.uninstall, call(1, f.a, {{"script", "s:" + std::to_string(second_script)}}))
                .changed &&
            drive::plan_event(f.triggers, values, targeted, 10, 20).empty(),
        "queued startup ran after its installation was removed");
    targeted.receiver = Handle::make(f.a.slot(), f.a.generation() + 1);
    require(drive::plan_event(f.triggers, values, targeted, 10, 20).empty(),
            "targeted startup accepted a stale receiver generation");
    const Json quantity = {{"name", "novel_scalar"}, {"kind", "quantity"}, {"source", "state"}};
    require(!invoke(declared.define_term, call(1, {}, {{"proposal", quantity}}))
                 .reply["ok"]
                 .get<bool>(),
            "ordinary player could edit vocabulary");
    const auto previewed = invoke(declared.define_term, call(3, {}, {{"proposal", quantity}}));
    require(!previewed.changed && previewed.reply["requires_choice"] &&
                !previewed.reply["duplicates"].empty(),
            "vocabulary proposal skipped literal duplicate comparison");
    const auto same =
        invoke(declared.define_term,
               call(3, {}, {{"proposal", quantity}, {"choice", {{"same_as", "novel.heat"}}}}));
    require(!same.changed && same.reply["term"] == 1 && f.terms.rows.size() == declaration_count,
            "explicit same_as invented another kind");
    const auto created =
        invoke(declared.define_term,
               call(3, {},
                    {{"proposal", quantity},
                     {"choice", {{"none_fits", "Tracks a separate authored phenomenon."}}}}));
    const auto new_term = created.reply.at("term").get<TermId>();
    const auto pin = created.reply.at("registry").get<std::string>();
    const auto actual_registry = catalog::read_registry(registry_directory, pin);
    require(created.changed && f.terms.rows[new_term].name == "p3.novel_scalar" &&
                f.terms.rows[new_term].owner == 3 && !f.terms.rows[new_term].compared.empty() &&
                actual_registry.data["terms"].back()["id"] == new_term,
            "new term lost namespace, authorship, comparison or immutable registry publication");
    require(!invoke(declared.define_term,
                    call(3, {},
                         {{"proposal", {{"name", "base.forged"}, {"kind", "event"}}},
                          {"choice", {{"none_fits", "forged namespace"}}}}))
                 .reply["ok"]
                 .get<bool>(),
            "proposer forged reserved namespace");
    require(
        !invoke(declared.define_term, call(3, {},
                                           {{"proposal",
                                             {{"name", "wrong_pair"},
                                              {"kind", "quantity"},
                                              {"combine", "share_equal"},
                                              {"paired", "novel.link"}}},
                                            {"choice", {{"none_fits", "invalid pair fixture"}}}}))
             .reply["ok"]
             .get<bool>(),
        "registry accepted an invalid allocation pairing");
    const auto previous_material = f.materials.rows.at(8);
    const auto previous_pin = document.hash;
    const auto invalid_physics =
        invoke(declared.define_material,
               call(3, {},
                    {{"material", "novel.mat"},
                     {"values", {{"novel.hardness", -1}, {"novel.transmission", 0}}}}));
    require(invalid_physics.reply.at("error") == "bad_input" &&
                f.materials.rows.at(8) == previous_material && document.hash == previous_pin,
            "invalid material physics changed catalog rows or registry pin");
    const auto material =
        invoke(declared.define_material,
               call(3, {},
                    {{"material", "novel.mat"},
                     {"values", {{"novel.hardness", 4}, {"novel.transmission", 0}}}}));
    require(material.changed && dirty(material, f.a, intrinsic) &&
                dirty(material, f.c, intrinsic) &&
                react::read(values, f.a, intrinsic, 10, 20).value.numeric() == 4,
            "material registry edit failed to project current intrinsic values");
    require(!invoke(declared.define_material,
                    call(3, {}, {{"material", "novel.mat"}, {"values", {{"novel.volume", 999}}}}))
                    .reply["ok"]
                    .get<bool>() &&
                f.materials.rows.at(8).at(intrinsic).numeric() == 4,
            "material declaration forged measured geometry");
    const auto blocked = registry_directory / "not-a-directory";
    std::ofstream(blocked) << "registry output is deliberately blocked";
    const auto child = fork();
    require(child >= 0, "start registry disk-failure subprocess");
    if (child == 0) {
      const rlimit no_core{0, 0};
      setrlimit(RLIMIT_CORE, &no_core);
      persistence.directory = blocked;
      invoke(declared.define_term,
             call(3, {},
                  {{"proposal", {{"name", "unpublished"}, {"kind", "event"}}},
                   {"choice", {{"none_fits", "real disk failure fixture"}}}}));
      _exit(1);
    }
    int child_status = 0;
    require(waitpid(child, &child_status, 0) == child && WIFSIGNALED(child_status) &&
                WTERMSIG(child_status) == SIGABRT,
            "registry disk failure continued under an obsolete persisted pin");
    const auto script_edit = [&](TermId action, Handle target, Json arguments) {
      auto request = call(1, target, std::move(arguments));
      request.request.caller = ScriptHolder{script, f.a};
      return invoke(action, std::move(request));
    };
    const auto denied = [](const drive::NativeCompletion &out, std::string_view need,
                           const Json &on) {
      return !out.reply.at("ok").get<bool>() && out.reply.contains("denied") &&
             out.reply.at("denied").at("need") == need && out.reply.at("denied").at("on") == on &&
             !out.reply.at("denied").at("why").get<std::string>().empty() &&
             !out.reply.at("denied").at("fix").get<std::string>().empty() &&
             !out.reply.contains("error");
    };
    require(script_edit(declared.edit_text, f.a, {{"text", {{"name", "Self-authored"}}}}).changed &&
                script_edit(declared.set_setting, f.a, {{"values", {{"novel.gain", 8}}}}).changed,
            "installed script could not use its own implicit Edit");
    require(denied(script_edit(declared.edit_text, f.b, {{"text", {{"name", "Borrowed"}}}}), "edit",
                   to_string(f.b)),
            "script borrowed its installation owner's Edit on another object");
    f.objects.preset[f.c.slot()] = 0;
    const auto editor = perm::Holder::script(script, f.a);
    const auto edit_grant = perm::create_grant(
        grants, players, perm::Holder::player(1),
        {1, editor, perm::Scope::object(f.b), perm::bit(perm::Verb::Edit), 0, 0, 100},
        {perm::Scope::object(f.b), 1, perm::all_rights}, {editor, 1, true}, 10);
    require(
        edit_grant &&
            script_edit(declared.edit_text, f.b, {{"text", {{"name", "Granted edit"}}}}).changed,
        "script with an actual other-object Edit grant could not edit it");
    for (const auto &[action, arguments] : std::vector<std::pair<TermId, Json>>{
             {declared.model_text, {{"model", "m:1"}, {"text", {{"name", "Borrowed author"}}}}},
             {declared.model_setting, {{"model", "m:1"}, {"values", {{"novel.gain", 99}}}}},
             {declared.model_script, {{"model", "m:1"}, {"script", nullptr}}}})
      require(denied(script_edit(action, {}, arguments), "model_author", "m:1"),
              "script inherited its installation owner's model authorship");
    profiles.rows[1].roles = {"elaborator"};
    require(denied(script_edit(declared.patch_state, f.a, {{"values", {{"novel.heat", 50}}}}),
                   "player_caller", to_string(f.a)) &&
                denied(script_edit(declared.define_term, {}, {{"proposal", quantity}}),
                       "player_caller", "administration"),
            "script inherited its installation owner's elaborator role");
    profiles.rows[1].roles.clear();
    const auto invalid_setting =
        script_edit(declared.set_setting, f.a, {{"values", {{"novel.volume", 999}}}});
    require(invalid_setting.reply.at("error") == "bad_input" &&
                !invalid_setting.reply.at("problem").get<std::string>().empty() &&
                !invalid_setting.reply.contains("denied"),
            "invalid property input was misreported as a permission denial");
    perm::Plot plot;
    plot.id = 1;
    plot.owner = 1;
    plots.rows.push_back(plot);
    f.objects.plot[f.b.slot()] = 1;
    const Json install_other = {{"script", "s:" + std::to_string(second_script)},
                                {"choices", Json::array()}};
    require(denied(script_edit(declared.install, f.b, install_other), "run_scripts", "plot:1"),
            "script borrowed its installation owner's plot RunScripts right");
    const auto run_right = perm::Rights{1} << static_cast<unsigned>(perm::LandVerb::RunScripts);
    const auto run_grant =
        perm::create_grant(grants, players, perm::Holder::player(1),
                           {1, editor, perm::Scope::plot(1), run_right, 0, 0, 100},
                           {perm::Scope::plot(1), 1, perm::Rights{7}}, {editor, 1, true}, 10);
    require(run_grant && script_edit(declared.install, f.b, install_other).changed,
            "script with actual Edit and RunScripts grants could not install behavior");
    f.objects.plot[f.b.slot()] = 0;
    std::filesystem::remove_all(registry_directory);
    for (int index = 0; index < 2000; ++index) {
      ObjectInit object;
      object.model = 1;
      object.material = 8;
      create(f.objects, object);
    }
    const auto started_at = std::chrono::steady_clock::now();
    for (int index = 0; index < 20; ++index)
      require(invoke(declared.model_setting,
                     call(1, {}, {{"model", "m:1"}, {"values", {{"novel.gain", index + 10}}}}))
                  .changed,
              "bulk current-instance setting edit failed");
    const auto elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started_at)
            .count();
    std::cout
        << "Editing: actual VM/watch/install/dedup/slot grants, claims, role+Edit repair, settings "
           "and authored relation boundaries passed; 2002-instance model setting edit mean "
        << elapsed / 20 << " ms\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
