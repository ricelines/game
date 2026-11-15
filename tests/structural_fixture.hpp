#pragma once
#include "space_fixture.hpp"

#include "bindings/commerce/commerce.hpp"
#include "bindings/construction/construction.hpp"
#include "bindings/installation/installation.hpp"
#include "bindings/metadata/metadata.hpp"
#include "bindings/structural/structural.hpp"
#include "bindings/turns/turns.hpp"
#include "runtime_fixture.hpp"
#include "scene_fixture.hpp"

namespace amber::test_structural {
using namespace react;
using test_runtime::require;
struct Fixture {
  test_runtime::Fixture f;
  lifecycle::Parts parts;
  Relation declared;
  lifecycle::Instances instances{f.objects, parts, declared};
  catalog::Assets assets;
  catalog::ParameterBlocks parameters;
  catalog::InstanceTexts instance_texts;
  bindings::GeometryAssets geometry;
  space::PhysicalRelations physical;
  std::vector<space::RelationChange> physical_changes;
  space::Space world;
  perm::OwnerLimits limits;
  perm::Players players;
  perm::Grants grants;
  perm::Plots plots;
  perm::Ledger ledger;
  bindings::StructuralRights rights;
  bindings::GeometryInstallation installation;
  perm::Offers offers;
  perm::Trades trades;
  testscene::Projection projections;
  testscene::Lifecycle lifecycle;
  bindings::AssemblyPlacement placement;
  bindings::CreateOperation creation;
  bindings::RemoveOperation removal;
  bindings::ReplaceOperation replacement;
  bindings::ModelMeasureBinding model_measure{f.terms,    f.materials, f.models,
                                              parameters, assets,      10};
  bindings::InstanceMeasureBinding instance_measure{identity(f.objects),
                                                    transforms(f.objects),
                                                    f.objects.params,
                                                    declared,
                                                    f.terms,
                                                    f.materials,
                                                    f.models,
                                                    parameters,
                                                    assets,
                                                    10};
  bindings::Execution execution{f.vm, f.states, f.random, identity(f.objects), declared, 44};
  command::NativeActions declarations;
  drive::NativeOperations operations;
  drive::Clock clock;
  record::Journal journal{8, {}};
  step::WorkQueues queues;
  step::TimerWheel timers;
  control::Profiles profiles;
  std::vector<FactKey> mutated;
  std::vector<bindings::GapDraft> gaps;
  bindings::HandlerTurns handlers{
      execution, f.triggers, *f.projection, {f.subscriptions, f.links, f.scripts, f.rules},
      journal,   queues,     timers,        mutated,
      clock,     20,         10000,         lifecycle.allocation};
  bindings::ActionTurns actions{handlers,
                                declarations,
                                operations,
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
                                world,
                                lifecycle.intents,
                                gaps,
                                5};
  ScriptId script = 0;
  space::GeometryId solid = 0;
  explicit Fixture(std::uint32_t capacity = 64)
      : f(250), world([&] {
          auto settings = testspace::settings();
          settings.max_bodies = capacity;
          return settings;
        }()),
        rights{identity(f.objects),
               authority(f.objects),
               transforms(f.objects),
               declared,
               limits,
               players,
               grants,
               plots,
               world,
               5},
        installation{
            world,    geometry, *f.projection, transforms(f.objects), space_flags(f.objects),
            {10, 11}, {.6f, 0}},
        projections(f.objects, f.models, parameters, assets, *f.projection, f.runtime,
                    f.subscriptions, declared, {30, 31}),
        lifecycle(f, world, instances, parts, declared, physical, physical_changes, projections,
                  players, grants, limits, offers, trades, plots, ledger),
        placement{world,
                  identity(f.objects),
                  transforms(f.objects),
                  parents(f.objects),
                  space_flags(f.objects),
                  declared,
                  physical,
                  physical_changes},
        creation{
            instances,           {f.models, parameters, assets}, installation, placement, rights,
            lifecycle.lifecycle, plot_membership(f.objects)},
        removal{instances, rights, lifecycle.lifecycle}, replacement{instances,
                                                                     parts,
                                                                     {f.models, parameters, assets},
                                                                     installation,
                                                                     transforms(f.objects),
                                                                     parents(f.objects),
                                                                     physical,
                                                                     physical_changes,
                                                                     rights,
                                                                     lifecycle.lifecycle,
                                                                     plot_membership(f.objects)} {
    f.terms.rows.resize(32);
    for (auto [id, role] :
         {std::pair{30u, MeasurementRole::Mass}, std::pair{31u, MeasurementRole::Runtime}}) {
      auto &term = f.terms.rows[id];
      term.id = id;
      term.name = "mechanical.mass" + std::to_string(id);
      term.source = Source::Measured;
      term.measurement = role;
      term.dimensions = {0, 1, 0, 0, 0, 0, 0, 0};
      f.terms.by_name[term.name] = id;
    }
    f.projection->density = 10;
    for (TermId id : {10u, 11u}) {
      f.terms.rows[id].id = id;
      f.terms.rows[id].name = id == 10 ? "novel.density" : "novel.transmission";
      f.terms.rows[id].source = Source::Intrinsic;
      f.terms.by_name[f.terms.rows[id].name] = id;
    }
    f.materials.rows[8] = {{10, Value(1000.0)}, {11, Value(0.0)}};
    f.objects.owner[f.a.slot()] = 1;
    players.rows.resize(3);
    plots.rows.resize(2);
    plots.rows[1] = {1, 1, 0, 0, {-10, -10, 10, 10}, {}};
    testspace::Box root({1, 1, 1}), child({.2f, .2f, .4f});
    auto root_geometry = space::import_geometry(world, root.source());
    auto child_geometry = space::import_geometry(world, child.source());
    require(root_geometry.geometry && child_geometry.geometry,
            "real fixture collision assets import");
    solid = root_geometry.geometry;
    f.models.rows.resize(3);
    f.models.rows[1].material = 8;
    f.models.rows[1].asset = std::string(64, 'a');
    f.models.rows[1].parts = {{"leaf", 2}};
    f.models.rows[2].id = 2;
    f.models.rows[2].material = 8;
    f.models.rows[2].asset = std::string(64, 'b');
    catalog::Asset asset;
    asset.hash = f.models.rows[1].asset;
    asset.volume = 1;
    asset.minimum = root.bounds.min;
    asset.maximum = root.bounds.max;
    asset.manifest = {
        {"bindings", nlohmann::json::object()},
        {"assembly", {{"bounds", {{"min", {-0.5, -0.5, 0}}, {"max", {1.1, 0.5, 1}}}}}},
        {"parts", {{"leaf", {{"position", {1, 0, 0}}, {"rotation", {0, 0, 0, 1}}}}}}};
    assets.rows[asset.hash] = asset;
    geometry.rows[asset.hash] = {root_geometry.geometry, {}};
    asset.hash = f.models.rows[2].asset;
    asset.volume = .016;
    asset.minimum = child.bounds.min;
    asset.maximum = child.bounds.max;
    asset.manifest["parts"] = nlohmann::json::object();
    assets.rows[asset.hash] = asset;
    geometry.rows[asset.hash] = {child_geometry.geometry, {}};
    f.assets.rows[1].volume = 1;
    f.assets.rows[2].volume = .016;
    register_binding(f.vm, "measure_model", bindings::measure_model_binding, &model_measure);
    register_binding(f.vm, "measure_instance", bindings::measure_instance_binding,
                     &instance_measure);
    bindings::register_construction_actions(
        operations, bindings::declare_construction_actions(f.terms, declarations), creation,
        removal, replacement);
    profiles.rows.resize(3);
    for (PlayerId id = 1; id < 3; ++id)
      profiles.rows[id].id = id;
    profiles.rows[1].body = f.a;
    f.objects.position[f.a.slot()] = {0, -2, 0};
    f.state(f.a, 1, 10);
  }
  ScriptId add(std::string_view source) {
    script = f.add(source);
    f.models.rows[1].script = script;
    f.states[{f.a, script}][std::string("tokens")] = 10.;
    install::project_model(f.triggers, f.models, f.scripts, f.terms, 1);
    return script;
  }
  bindings::ActionOutcome act(std::string_view name, Handle target = {},
                              nlohmann::json args = nlohmann::json::object()) {
    return bindings::perform_action(
        actions,
        {ScriptHolder{script, f.a}, f.terms.by_name.at(std::string(name)), target, Value{}},
        std::move(args), {});
  }
  GrantId grant(Handle object, perm::Rights rights) {
    const auto holder = perm::Holder::script(script, f.a);
    const auto owner = f.objects.owner[object.slot()];
    const auto scope = perm::Scope::object(object);
    if (const auto prior = grants.by_key.find({holder, scope}); prior != grants.by_key.end()) {
      rights |= grants.rows[prior->second].verbs;
      perm::revoke(grants, players, prior->second);
    }
    auto result = perm::create_grant(
        grants, players, perm::Holder::player(owner), {owner, holder, scope, rights, 0, 0, 10000},
        {scope, owner, perm::all_rights, 0, false}, {holder, owner, true}, 0);
    require(bool(result), "ordinary explicit script grant");
    return result.grant;
  }
  void allow_build() {
    const auto holder = perm::Holder::script(script, f.a);
    const auto scope = perm::Scope::plot(1);
    auto result = perm::create_grant(
        grants, players, perm::Holder::player(1),
        {1, holder, scope, perm::Rights{1} << static_cast<unsigned>(perm::LandVerb::Build), 0, 0,
         10000},
        {scope, 1, perm::all_rights, 0, false}, {holder, 1, true}, 0);
    require(bool(result), "script Build grant");
  }
  std::vector<Handle> created(const bindings::ActionOutcome &out) {
    std::vector<Handle> result;
    if (out.reply.contains("created"))
      for (const auto &value : out.reply.at("created"))
        result.push_back(*parse_handle(value.get<std::string>()));
    return result;
  }
  bindings::CatalogPublication publication(catalog::ModelTexts &texts) {
    return {f.models,
            f.scripts,
            f.terms,
            f.materials,
            texts,
            instance_texts,
            *f.projection,
            authority(f.objects),
            parents(f.objects),
            f.triggers,
            lifecycle.priorities,
            projections.metadata,
            {}};
  }
  void variants() {
    f.materials.rows[9] = {{10, Value(500.)}, {11, Value(.5)}};
    f.models.rows.resize(4);
    f.models.rows[3].id = 3;
    f.models.rows[3].material = 9;
    f.models.rows[3].asset = std::string(64, 'd');
    auto child = assets.rows.at(std::string(64, 'b'));
    child.hash = f.models.rows[3].asset;
    assets.rows[child.hash] = child;
    geometry.rows[child.hash] = geometry.rows.at(std::string(64, 'b'));
    testspace::Box expanded({2, 1, 1});
    auto asset = assets.rows.at(std::string(64, 'a'));
    asset.hash = std::string(64, 'c');
    asset.volume = 2;
    asset.maximum = expanded.bounds.max;
    asset.minimum = expanded.bounds.min;
    asset.manifest["assembly"]["bounds"] = {{"min", {-2.1, -.5, 0}}, {"max", {2.1, .5, 1}}};
    asset.manifest["parts"] = {{"leaf", {{"position", {2, 0, 0}}, {"rotation", {0, 0, 0, 1}}}},
                               {"tip", {{"position", {-2, 0, 0}}, {"rotation", {0, 0, 0, 1}}}}};
    assets.rows[asset.hash] = asset;
    geometry.rows[asset.hash] = {space::import_geometry(world, expanded.source()).geometry, {}};
    parameters.rows.push_back({1, 1, asset.hash, "{}", {{"leaf", 2}, {"tip", 3}}});
    asset.hash = std::string(64, 'e');
    asset.volume = 0;
    asset.manifest["parts"].erase("leaf");
    assets.rows[asset.hash] = asset;
    geometry.rows[asset.hash] = {0, {}};
    parameters.rows.push_back({2, 1, asset.hash, "{}", {{"tip", 3}}});
    f.assets.rows[3].volume = .016;
    f.assets.parameter_rows[1].volume = 2;
    f.assets.parameter_rows[2].volume = 0;
  }
};
} // namespace amber::test_structural
