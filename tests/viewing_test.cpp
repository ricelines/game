#include "space_fixture.hpp"
#include "step/step.hpp"

#include <chrono>

using namespace amber;
namespace vi = amber::net;
using Json = vi::Json;
using testspace::require;
namespace {
void events_projection(bool benchmark) {
  vi::SessionViews views;
  views.rows[1].eye = {};
  vi::RecordFeed feed{{}, 4096};
  const std::string thing = "o:7.1";
  const auto add = [&](std::uint64_t id, std::uint64_t root, const char *actor,
                       const std::string &object, Vec3 position) {
    feed.rows.push_back(
        {{{"id", id}, {"root", root}, {"actor", actor}, {"things", Json::array({object})}},
         {position}});
  };
  add(1, 7, "p:9", thing, {});
  add(2, 7, "p:9", thing, {100, 0, 0});
  add(3, 7, "p:2", thing, {100, 0, 0});
  add(4, 8, "p:9", thing, {});
  add(5, 7, "p:9", "o:8.1", {});
  add(6, 7, "p:9", thing, {});
  add(7, 7, "p:9", thing, {});
  feed.rows.back().record["authors"] = {"p:2", "p:9"};
  feed.rows.back().record["cause"] = 6;
  feed.rows.back().record["result"] = "applied";
  feed.rows.back().record["observation"] = {{"things", Json::array({{{"h", thing}}})}};
  feed.rows.back().record["measured"] = {{"volume", 1}};
  feed.rows.back().record["source"] = "authored source";
  net::Invocation call;
  call.session = 1;
  call.principal.player = 2;
  call.principal.mode = net::Mode::Watch;
  call.request.fields = {{"since", 1}, {"root", 7}, {"thing", thing}, {"limit", 2}};
  auto result = vi::read_events(feed, views, 30, 40, call).fields;
  require(result["events"].size() == 2 && result["events"][0]["id"] == 6 &&
              result["events"][1]["id"] == 7 && result["events_omitted"] == 1 &&
              result["oldest_available"] == 1 && result["newest"] == 7 &&
              !result["truncated"].get<bool>(),
          "events ring changed chronological order, filters, privacy or omitted count");
  const auto full = result;
  call.request.fields["detail"] = false;
  result = vi::read_events(feed, views, 30, 40, call).fields;
  require(result["events_omitted"] == full["events_omitted"] &&
              result["oldest_available"] == full["oldest_available"] &&
              result["newest"] == full["newest"] && result["events"][0] == full["events"][0] &&
              result["events"][1]["id"] == 7 &&
              result["events"][1]["authors"] == full["events"][1]["authors"] &&
              result["events"][1]["cause"] == 6 && result["events"][1]["result"] == "applied" &&
              !result["events"][1].contains("observation") &&
              !result["events"][1].contains("measured") && !result["events"][1].contains("source"),
          "summary query changed privacy/filter/causal attribution or copied heavy details");
  call.request.fields.erase("detail");
  call.request.fields["since"] = 6;
  result = vi::read_events(feed, views, 30, 40, call).fields;
  require(result["events"].size() == 1 && result["events"][0]["id"] == 7 &&
              result["events_omitted"] == 0,
          "events since filter changed");
  call.principal.mode = net::Mode::Debug;
  call.request.fields["since"] = 1;
  call.request.fields["limit"] = 1;
  result = vi::read_events(feed, views, 30, 40, call).fields;
  require(result["events"].size() == 1 && result["events"][0]["id"] == 7 &&
              result["events_omitted"] == 3,
          "debug visibility or single-result wrap changed");
  call.request.fields["detail"] = false;
  const auto debug_summary = vi::read_events(feed, views, 30, 40, call).fields;
  require(debug_summary["events"][0]["id"] == 7 &&
              debug_summary["events_omitted"] == result["events_omitted"],
          "summary query changed debug visibility or tail selection");
  call.request.fields.erase("detail");
  call.request.fields["root"] = 999;
  result = vi::read_events(feed, views, 30, 40, call).fields;
  require(result["events"].empty() && result["events_omitted"] == 0,
          "empty filtered events query invented rows");
  if (!benchmark)
    return;
  feed.rows.clear();
  const Json details{{"text", std::string(1024, 'x')}, {"values", std::vector<double>(64, 1)}};
  for (std::uint64_t id = 1; id <= 4096; ++id) {
    add(id, id, "p:2", thing, {});
    feed.rows.back().record["detail"] = details;
  }
  call.principal.mode = net::Mode::Watch;
  call.request.fields = {{"limit", 1}};
  std::vector<double> samples;
  for (int i = 0; i < 110; ++i) {
    const auto started = std::chrono::steady_clock::now();
    result = vi::read_events(feed, views, 30, 40, call).fields;
    const auto us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started)
            .count();
    require(result["events"][0]["id"] == 4096 && result["events_omitted"] == 4095,
            "full event retention benchmark returned an incorrect page");
    if (i >= 10)
      samples.push_back(us);
  }
  std::sort(samples.begin(), samples.end());
  std::cout << "events4096_limit1_us p50=" << samples[50] << " p95=" << samples[95] << '\n';
}
} // namespace
int main(int argc, char **argv) {
  try {
    events_projection(argc == 2 && std::string_view(argv[1]) == "--events-bench");
    testspace::Scene scene;
    const auto body = scene.character({0, 0, 0});
    const auto visible = scene.box({1, 1, 2}, {0, 5, 0});
    const auto far = scene.box({12, 2, 10}, {10, 60, 0}, space::Motion::Static, 0, false);
    const auto held = scene.box({.2f, .2f, .2f}, {2, 0, 0}, space::Motion::Dynamic, 0, false);
    std::vector<space::RelationChange> changes;
    require(space::attach(scene.space, identity(scene.objects), transforms(scene.objects),
                          parents(scene.objects), space_flags(scene.objects),
                          scene.relations.contains, scene.relations.supports,
                          scene.relations.attached_to, {held, body, {{0, .3f, 1}, {}}, true},
                          changes)
                    .status == space::Status::Ok,
            "actual grasp for held observation");
    scene.objects.owner[body.slot()] = 2;
    scene.objects.owner[visible.slot()] = 3;
    scene.objects.model[far.slot()] = 3;
    scene.objects.model[held.slot()] = 4;
    for (const auto h : {body, visible, far, held})
      scene.objects.material[h.slot()] = 2;
    react::Terms terms;
    terms.rows.resize(17);
    for (TermId id = 1; id < 5; ++id) {
      terms.rows[id].id = id;
      terms.rows[id].name = "test.t" + std::to_string(id);
      terms.by_name[terms.rows[id].name] = id;
    }
    for (TermId id = 5; id <= 16; ++id) {
      auto &term = terms.rows[id];
      term.id = id;
      term.name = "novel.observed" + std::to_string(id);
      term.source = id <= 10 ? react::Source::Setting : react::Source::State;
      terms.by_name[term.name] = id;
    }
    terms.rows[13].combine = terms.rows[14].combine = react::Combine::Sum;
    terms.rows[15].source = react::Source::Intrinsic;
    terms.rows[16].source = react::Source::Measured;
    terms.rows[16].measurement = react::MeasurementRole::Volume;
    terms.rows[2].kind = react::Kind::Material;
    terms.rows[3].kind = react::Kind::Action;
    terms.rows[1].dimensions[1] = 1;
    terms.rows[3].inputs = {{"amount", react::InputType::Number, 1, false},
                            {"note", react::InputType::String, 0, true}};
    terms.rows[4].kind = react::Kind::Event;
    react::Materials materials;
    react::AssetMeasurements measurements;
    react::Properties properties(17);
    react::Processes processes(17);
    react::InstanceSettings settings;
    react::ModelSettings model_settings;
    react::PlotSettings plot_settings;
    react::WorldSettings world;
    react::SharedColumns shared(17);
    settings.columns.resize(17);
    settings.columns[6].set(visible.slot(), Value(0.0));
    model_settings.rows[1][7] = Value(7.0);
    scene.objects.plot[visible.slot()] = 1;
    plot_settings.rows[1][8] = Value(8.0);
    world.values[5] = Value(99.0);
    world.values[9] = Value(9.0);
    world.values[10] = Value(10.0);
    world.values[12] = Value(12.0);
    world.values[14] = Value(14.0);
    properties[11].set(visible.slot(), Value(11.0));
    shared[13].set(visible.slot(), react::Shared{13.0, {{held, 13.0}}});
    materials.rows[2][15] = Value(15.0);
    measurements.rows[1].volume = 1;
    react::RelationsRead relations;
    react::RuntimeMeasurements runtime;
    const react::ValueRead values{terms,
                                  identity(scene.objects),
                                  scene.objects.plot,
                                  scene.objects.params,
                                  materials,
                                  measurements,
                                  properties,
                                  processes,
                                  settings,
                                  model_settings,
                                  plot_settings,
                                  world,
                                  shared,
                                  relations,
                                  runtime,
                                  0};
    catalog::Models models;
    models.rows.resize(5);
    catalog::Assets assets;
    catalog::ParameterBlocks parameters;
    for (ModelId id = 1; id < 5; ++id) {
      const auto hash = catalog::sha256(std::to_string(id));
      catalog::Asset asset;
      asset.hash = hash;
      asset.minimum = {-.5f, -.5f, 0};
      asset.maximum = {.5f, .5f, 2};
      asset.volume = 1;
      asset.capacity = .1;
      asset.manifest = {
          {"params", Json::object()}, {"sites", Json::object()}, {"bindings", Json::object()}};
      if (id == 2)
        asset.manifest["bindings"]["observer"] = {
            {"eye", {0, 0, 1.55}}, {"forward", {0, 1, 0}}, {"dim_lux", 10}};
      if (id == 1) {
        asset.replicated = {10};
        asset.manifest["bindings"]["render"] = {
            {{"term", "novel.observed9"}, {"property", "emission"}}};
      }
      assets.rows[hash] = asset;
      models.rows[id].id = id;
      models.rows[id].asset = hash;
      models.rows[id].material = 2;
      models.rows[id].script = 1;
    }
    catalog::ModelTexts model_text;
    model_text.rows[1] = {"Claimed giant", "Maker text does not alter measured dimensions", ""};
    catalog::InstanceTexts instance_text;
    catalog::Scripts scripts;
    const react::VmSettings vm_settings{4096, 50, 5, 64};
    react::Vm vm(vm_settings);
    observe::Hearing hearing;
    const std::vector<observe::Listener> listeners{{11, body, {0, 0, 1.55f}},
                                                   {12, body, {100, 100, 0}}};
    react::GivesTo gives_to;
    react::bind_values(vm, values, properties, processes, shared, gives_to);
    auto prepared = catalog::prepare_script(
        R"(return {kind='model',actions={['test.t3']={right='use'}},on={
      ['test.t3']=function(self) end,
      ['test.t4']=function(self)
        self:set('test.t1',73)
        self.state[123]=42
        self.state['123']='distinct string key'
        self.state.retained=self.handle
        self.state.text='o:1.1'
      end}})",
        terms, {{"use", "take", "move", "edit", "copy", "transfer", "destroy"}},
        catalog::sha256("registry"), {}, vm_settings);
    auto compiled = prepared.compiled;
    catalog::import_script(scripts, std::move(prepared), 2, 0);
    require(react::load_script(vm, compiled, scripts.rows[1].declaration).status ==
                react::RunOutcome::Status::Ok,
            "real VM module load");
    react::InstanceScripts installed;
    react::NativeActions native;
    react::declare_native(
        native, terms, "engine.example", "An actual native descriptor", "", false,
        {{"type", "object"},
         {"required", {"amount"}},
         {"properties", {{"amount", {{"type", "integer"}}}, {"note", {{"type", "string"}}}}}},
        react::WritingAction::EditText);
    const std::vector<TermId> inspected{1, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    catalog::AuthoredTexts authored_texts;
    const auto detail_id = catalog::append_authored_text(
        authored_texts, visible, 1, 12, catalog::AuthoredTextKind::Detail, "A worn brass edge");
    catalog::append_authored_text(authored_texts, visible, 1, 13,
                                  catalog::AuthoredTextKind::Refusal, "private refusal reason");
    catalog::append_authored_text(authored_texts, far, 1, 14, catalog::AuthoredTextKind::Detail,
                                  "Other object");
    catalog::append_authored_text(authored_texts,
                                  Handle::make(visible.slot(), visible.generation() + 1), 1, 15,
                                  catalog::AuthoredTextKind::Detail, "Different generation");
    vi::SessionViews views;
    observe::ActiveViews active(8);
    observe::Lights lights(8);
    observe::ZoneAmbients zones;
    const std::vector<observe::MotionSample> motion;
    step::Intents intents;
    Relation parts;
    Step now = 0;
    const observe::Settings perception{30, 1.5f, 256, 1, .1f, .1, 40, 8, 40};
    const auto look = [&](const net::Invocation &request) {
      return vi::read_look(models, parameters, assets, scripts, model_text, instance_text,
                           installed, native, values, inspected, authored_texts, views, active,
                           hearing, scene.space, transforms(scene.objects), parts,
                           scene.relations.attached_to, lights, zones, motion, intents, perception,
                           now, 20, 120, request);
    };
    net::Invocation call;
    call.session = 11;
    call.principal = {2, body, "Observer", {}, net::Mode::Play, false};
    call.request.kind = net::ControlKind::Look;
    call.request.fields = Json::object();
    auto first = look(call).fields;
    const std::string signature = "test.t3(amount: number [kg], note?: string)";
    require(first["body_actions"] == Json::array({signature}) &&
                first["player_actions"] ==
                    Json::array({"engine.example(amount: integer, note?: string)"}),
            "full observation omitted one-line authored/native input signatures");
    perm::Players players;
    players.rows.resize(4);
    perm::Grants grants;
    perm::OwnerLimits limits;
    const auto offered = [&](const net::Invocation &request) {
      return vi::read_actions(models, scripts, installed, native, values, views, hearing, 10, 40,
                              authority(scene.objects), limits, players, grants, now, request);
    };
    auto action_call = call;
    action_call.request.fields = {{"name", "test.t3"}};
    const auto offered_action = offered(action_call).fields.at("action");
    require(offered_action["name"] == "test.t3" && offered_action["signature"] == signature &&
                offered_action["inputs"][0]["quantity"]["unit"] == "kg",
            "rich action discovery lost callable name, canonical signature or units");
    auto &declared_action = scripts.rows[1].declaration.actions[0];
    const auto body_model = scene.objects.model[body.slot()];
    models.rows[body_model].script = 0;
    action_call.request.fields = {{"target", to_string(visible)}};
    auto available = offered(action_call).fields;
    require(available["total"] == 0 && available["actions"].empty() && available["more"] == false,
            "source discovery offered an action without the player's current selected right");
    action_call.request.fields["name"] = "test.t3";
    require(offered(action_call).fields.at("action")["name"] == "test.t3",
            "rights-filtered lists hid detailed help for an open-use action term");
    action_call.request.fields.erase("name");
    const auto scope = perm::Scope::object(visible);
    const auto use = perm::bit(perm::Verb::Use);
    const auto use_grant = perm::create_grant(grants, players, perm::Holder::player(3),
                                              {3, perm::Holder::player(2), scope, use, 0, 0, 10},
                                              {scope, 3, perm::all_rights, 0, false},
                                              {perm::Holder::player(2), 2, false}, now);
    require(bool(use_grant), "source discovery fixture needs a real explicit Use grant");
    now = 9;
    available = offered(action_call).fields;
    require(available["total"] == 1 && available["actions"][0]["name"] == "test.t3",
            "current explicit source permission did not expose its declared action");
    now = 10;
    action_call.request.fields["page"] = 1;
    available = offered(action_call).fields;
    require(available["total"] == 0 && available["actions"].empty() && available["more"] == false,
            "action permissions were stale or pagination ran before current rights filtering");
    action_call.request.fields.erase("page");
    scene.objects.preset[visible.slot()] = use;
    require(offered(action_call).fields["total"] == 1,
            "public object preset did not make the actual action available");
    scene.objects.preset[visible.slot()] = 0;
    models.rows[body_model].script = 1;
    require(
        offered(action_call).fields["total"] == 1,
        "body-first action filtering checked the foreign selection instead of its self endpoint");
    declared_action.reach = true;
    scene.objects.preset[far.slot()] = use;
    action_call.request.fields = {{"target", to_string(far)}};
    require(offered(action_call).fields["total"] == 1,
            "requested action discovery added a reach filter absent from the source contract");
    scene.objects.preset[far.slot()] = 0;
    declared_action.reach = false;
    now = 0;
    action_call.request.fields = Json::object();
    available = offered(action_call).fields;
    require(available["total"] == 2 && available["actions"][0]["name"] == "engine.example" &&
                available["actions"][1]["name"] == "test.t3",
            "self discovery lost owned body actions or native actions with no declared right");
    auto find = [](const Json &rows, Handle h) -> const Json * {
      for (const auto &row : rows)
        if (row["h"] == to_string(h))
          return &row;
      return nullptr;
    };
    const auto *seen = find(first["things"], visible);
    const auto *held_row = find(first["things"], held);
    require(held_row && (*held_row)["actions"] == Json::array({signature}),
            "object observation lists bare names instead of input signatures");
    require(seen && seen->contains("measured") && (*seen)["says"]["name"] == "Claimed giant",
            "structured full row and separate maker claim");
    const auto &observed_values = (*seen)["measured"]["values"];
    require(
        !observed_values.contains("novel.observed5") &&
            !observed_values.contains("novel.observed12") &&
            !observed_values.contains("novel.observed14") &&
            observed_values["novel.observed6"] == 0 && observed_values["novel.observed7"] == 7 &&
            observed_values["novel.observed8"] == 8 && observed_values["novel.observed9"] == 9 &&
            observed_values["novel.observed10"] == 10 &&
            observed_values["novel.observed11"] == 11 &&
            observed_values["novel.observed13"] == 13 &&
            observed_values["novel.observed15"] == 15 && observed_values["novel.observed16"] == 1,
        "compact look hid arbitrary relevant values or exposed absent/global-only defaults");
    require((*seen)["elaborated"].size() == 1 && (*seen)["elaborated"][0]["id"] == detail_id &&
                (*seen)["elaborated"][0]["text"] == "A worn brass edge" &&
                (*seen)["elaborated"][0]["author"] == "p:1" &&
                (*seen)["elaborated"][0]["created_s"] == 12,
            "public detail lost its provenance, merged maker claims or exposed a refusal");
    require(find(first["things"], held) && (*find(first["things"], held))["held"].get<bool>(),
            "held object included by real custody relationship");
    require(find(first["far"], far), "salient distant geometry included");
    const auto second = look(call).fields;
    require(find(second["things"], visible)->contains("measured"),
            "native observations must remain full for gateway compaction");
    require(views.rows.at(11).last_observation.size() >= 3,
            "capture retains handles from short and far rows");
    require(first.contains("body_actions") && !first["body_actions"].empty() &&
                first.contains("player_actions") &&
                second["body_actions"] == first["body_actions"] &&
                second["player_actions"] == first["player_actions"],
            "native projection lost action lists needed by a fresh gateway session");
    const auto *prepared_rows = views.rows.at(11).pending.things.data();
    const auto acquired = views.rows.at(11).acquired;
    const auto positions = scene.objects.position;
    const auto flags = scene.objects.flags;
    const auto model_ids = scene.objects.model;
    const auto full_again = look(call).fields;
    require(find(full_again["things"], visible) && *find(full_again["things"], visible) == *seen &&
                full_again["body_actions"] == first["body_actions"] &&
                full_again["player_actions"] == first["player_actions"] &&
                full_again["far"] == first["far"],
            "successive native looks must supply full descriptions and actions");
    require(views.rows.at(11).pending.ready &&
                views.rows.at(11).pending.things.data() == prepared_rows &&
                views.rows.at(11).acquired == acquired && !active.dirty.contains(11) &&
                scene.objects.position == positions && scene.objects.flags == flags &&
                scene.objects.model == model_ids,
            "unchanged look rescanned perception or mutated world state");
    call.request.fields["limit"] = 1;
    const auto limited = look(call).fields;
    require(limited["things"].size() == 1 && limited["far"] == first["far"] &&
                views.rows.at(11).pending.things.data() == prepared_rows &&
                views.rows.at(11).pending.things.size() > 1,
            "smaller same-step limit ignored the requested near-row cap or rescanned perception");
    const auto &reported = views.rows.at(11).last_observation;
    require(reported.size() == 1 + limited["things"].size() + limited["far"].size() &&
                std::ranges::all_of(reported,
                                    [&](Handle h) {
                                      return h == body || find(limited["things"], h) ||
                                             find(limited["far"], h);
                                    }),
            "limited observation capture includes handles not returned to the player");
    call.request.fields.erase("limit");
    model_settings.rows[1][7] = Value(17.0);
    vi::invalidate(views, std::span(&visible, 1));
    const auto setting_changed = look(call).fields;
    require(find(setting_changed["things"], visible)
                    ->at("measured")
                    .at("values")
                    .at("novel.observed7") == 17,
            "compact look did not replace a short row after arbitrary authored setting change");
    authored_texts.rows[detail_id].text = "A polished brass edge";
    vi::invalidate(views, std::span(&visible, 1));
    const auto rewritten = look(call).fields;
    const auto *rewritten_row = find(rewritten["things"], visible);
    require(rewritten_row && (*rewritten_row)["elaborated"][0]["text"] == "A polished brass edge" &&
                (*rewritten_row)["says"]["name"] == "Claimed giant",
            "same-step detail rewrite remained hidden behind an unchanged observation row");
    react::ScriptStates states;
    react::RuleVersions rules;
    react::DeferredEffects effects;
    react::Invocation invocation;
    invocation.self = {visible, 1};
    invocation.state = &states[{visible, 1}];
    invocation.effects = &effects;
    require(react::run_handler(vm, {1, "test.t4"}, invocation).status ==
                react::RunOutcome::Status::Ok,
            "actual VM changes inspected object's own state");
    scripts.rows[1].source = "verified public source";
    perm::Plots plots;
    plots.rows.push_back({1, 2, 0, 0, {80, 30, 90, 40}, {}});
    plots.rows.push_back({2, 3, 0, 0, {-20, -10, -10, 0}, {}});
    plots.rows.push_back({3, 2, 0, 0, {95, 30, 105, 40}, {}});
    perm::Groups groups;
    perm::Ledger ledger;
    const auto inspect = [&](const net::Invocation &request) {
      return vi::read_inspect(models, parameters, assets, scripts, model_text, instance_text,
                              installed, native, values, inspected, authored_texts, views, hearing,
                              transforms(scene.objects), authority(std::as_const(scene.objects)),
                              parents(scene.objects), attribution(scene.objects), relations, limits,
                              players, grants, plots, ledger, now, 20, 40, states, rules, request);
    };
    auto grant_call = call;
    grant_call.request.fields = {{"target", "grants"}};
    const auto grant_inspection = inspect(grant_call).fields;
    require(grant_inspection["held"].size() == 1 &&
                grant_inspection["held"][0]["id"] == "g:" + std::to_string(use_grant.grant) &&
                grant_inspection["held"][0]["parent"].is_null(),
            "grant inspection must return the same public identifier used for revocation");
    const auto current_source = catalog::sha256("verified current CAD");
    models.rows[1].source_hash = current_source;
    models.rows[1].canonical_params = "{}";
    assets.rows[models.rows[1].asset].source = "verified current CAD";
    auto current_variant = assets.rows.at(models.rows[1].asset);
    current_variant.hash = catalog::sha256("current variant");
    current_variant.source_hash = current_source;
    assets.rows[current_variant.hash] = current_variant;
    parameters.rows.push_back({1, 1, current_variant.hash, R"({"width":2.0})", {}});
    auto historical_variant = current_variant;
    historical_variant.hash = catalog::sha256("historical variant");
    historical_variant.source_hash = catalog::sha256("previous source");
    assets.rows[historical_variant.hash] = historical_variant;
    parameters.rows.push_back({2, 1, historical_variant.hash, R"({"old_parameter":3.0})", {}});
    call.request.fields = {{"target", "m:1"}};
    const auto model_inspection = inspect(call).fields;
    require(model_inspection["source"] == "verified current CAD" &&
                model_inspection["source_hash"] == current_source &&
                model_inspection["canonical_params"] == "{}" &&
                model_inspection["variants"].size() == 1 &&
                model_inspection["variants"][0]["params"] == 1,
            "live authoring inspection lost exact source/default or included historical variants");
    call.request.fields = {{"target", to_string(visible)}};
    const auto current = inspect(call).fields;
    require(current["values"]["test.t1"] == 73,
            "inspect reads current VM-mutated state after an earlier observation");
    require(current["values"]["novel.observed5"] == 99 &&
                !current["values"].contains("novel.observed12") &&
                !current["values"].contains("novel.observed14"),
            "inspect lost exhaustive inherited settings or invented state/shared defaults");
    require(current["elaborated"].size() == 1 &&
                current["elaborated"][0]["text"] == "A polished brass edge",
            "public inspection omitted the shared elaborated detail");
    require(!current.contains("script_states"),
            "inspection leaked another owner's private flat state");
    call.principal.player = 3;
    const auto private_state = inspect(call).fields;
    const auto &entries = private_state["script_states"][0]["state"];
    auto entry = [&](const Json &key) -> const Json & {
      for (const auto &row : entries)
        if (row["key"] == key)
          return row;
      throw std::runtime_error("missing typed script state key");
    };
    require(entry(123)["type"] == "number" && entry(123)["value"] == 42 &&
                entry("123")["type"] == "string" &&
                entry("123")["value"] == "distinct string key" &&
                entry("retained")["type"] == "handle" &&
                entry("retained")["value"] == to_string(visible) &&
                entry("text")["type"] == "string" && entry("text")["value"] == "o:1.1",
            "actual VM flat state lost integer/string key or typed-handle distinctions");
    call.principal.player = 2;
    scene.objects.preset[visible.slot()] = perm::bit(perm::Verb::Edit);
    require(inspect(call).fields.contains("script_states"),
            "current Edit did not permit generic installed state inspection");
    scene.objects.preset[visible.slot()] = 0;
    auto rule_prepared = catalog::prepare_script(
        R"(return {kind='rule',applies_to={material='test.t2'},
          on={['test.t4']=function(self)self.state[2]=19 end},
          tests={{when={steps=0},expect={subject='target',term='test.t1',equals=73}}}})",
        terms, {{"use", "edit"}}, catalog::sha256("registry"), {}, vm_settings);
    const auto rule_code = rule_prepared.compiled;
    const auto rule_id = catalog::import_script(scripts, std::move(rule_prepared), 1, 0);
    scripts.rows[rule_id].declaration.approved = true;
    scripts.rows[rule_id].declaration.active = true;
    react::add_rule(rules, scripts, terms, rule_id);
    require(react::load_script(vm, rule_code, scripts.rows[rule_id].declaration).status ==
                react::RunOutcome::Status::Ok,
            "actual applicable rule VM load");
    react::Invocation rule_invocation;
    rule_invocation.self = {visible, rule_id};
    rule_invocation.state = &states[{visible, rule_id}];
    rule_invocation.effects = &effects;
    require(react::run_handler(vm, {rule_id, "test.t4"}, rule_invocation).status ==
                react::RunOutcome::Status::Ok,
            "actual rule writes its numeric flat state");
    call.principal.player = 3;
    const auto with_rule = inspect(call).fields;
    require(with_rule["script_states"].size() == 2 &&
                with_rule["script_states"][1]["script"] == "s:" + std::to_string(rule_id) &&
                with_rule["script_states"][1]["state"][0]["key"] == 2 &&
                with_rule["script_states"][1]["state"][0]["value"] == 19,
            "owner inspection omitted actual applicable world-rule state");
    react::set_rule_enabled(rules, {4, 2, false}, false);
    require(inspect(call).fields["script_states"].size() == 1,
            "inspection exposed a no-longer-applicable rule state as an installed script");
    call.principal.player = 2;
    call.request.fields = {{"target", "plots"}};
    const auto owned = inspect(call).fields;
    require(owned["plots"].size() == 2 && owned["plots"][0]["id"] == 1 &&
                owned["plots"][1]["id"] == 3 &&
                owned["plots"][0]["bounds"]["min"] == Json::array({80.f, 30.f}),
            "owned-land inspection omitted actual remote bounds or included a foreign plot");
    call.request.fields = {{"target", "plot:2"}};
    const auto land = inspect(call).fields;
    require(land["plot"]["owner"] == "p:3" && land["plot"]["id"] == 2 &&
                land["plot"]["bounds"]["max"] == Json::array({-10.f, 0.f}),
            "public plot inspection invented ownership or failed by actual ID");
    call.request.fields = {{"target", "plot:0"}};
    require(!inspect(call).fields["ok"].get<bool>(), "plot lookup accepted reserved ID zero");
    call.request.fields = {{"target", "plot:999"}};
    require(!inspect(call).fields["ok"].get<bool>(), "plot lookup accepted nonexistent ID");
    vi::invalidate(views, std::span(&visible, 1));
    call.request.fields = Json::object();
    const auto refreshed = look(call).fields;
    require(find(refreshed["things"], visible)->at("measured").at("values").at("test.t1") == 73,
            "same-step invalidation refreshes a changed visible value");
    require(!current["rights"]["take"]["allowed"].get<bool>(),
            "inspect exposes actual denial without requiring Use to inspect");
    observe::emit_sound(
        hearing, {body, {0, 0, 1.55f}, 12, std::string("audible"), 2, {}, "Nearby speaker", true},
        listeners);
    call.request.fields = Json::object();
    const auto heard = look(call).fields;
    require(heard["heard"].size() == 1 && heard["heard"][0]["text"] == "audible" &&
                heard["heard"][0]["from"] == "p:2" && heard["heard"][0]["name"] == "Nearby speaker",
            "native response retains real nearby hearing");
    require(!hearing.queues.contains(12) || hearing.queues.at(12).empty(),
            "distant listener receives no sound");
    call.request.fields = {
        {"target", to_string(Handle::make(visible.slot(), visible.generation() + 1))}};
    require(!inspect(call).fields["ok"].get<bool>(), "stale generation rejected");
    vi::RecordFeed feed{{}, 2};
    const std::array<Json, 3> records{
        {{{"id", 1}, {"root", 1}, {"actor", "p:9"}, {"things", Json::array({to_string(visible)})}},
         {{"id", 2}, {"root", 1}, {"actor", "p:9"}, {"things", Json::array({to_string(far)})}},
         {{"id", 3}, {"root", 3}, {"actor", "p:2"}, {"things", Json::array()}}}};
    vi::append_records(feed, records, identity(scene.objects), transforms(scene.objects));
    call.request.fields = {{"since", 0}};
    auto filtered = vi::read_events(feed, views, 30, 40, call).fields;
    require(filtered["events"].size() == 1 && filtered["events"][0]["id"] == 3,
            "ordinary feed excludes remote world records");
    call.principal.mode = net::Mode::Debug;
    require(vi::read_events(feed, views, 30, 40, call).fields["events"].size() == 2,
            "explicit debug feed can show retained global records");
    vi::remove_session(views, active, hearing, 11);
    require(!views.rows.contains(11) && !active.rows.contains(11) && !hearing.queues.contains(11),
            "close erases transient session read state");
    std::cout << "Viewing actual Space/VM boundary checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
